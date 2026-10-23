/* -------- -------- -------- -------- -------- -------- -------- --------
 * P I E L O G
 * -------- -------- -------- -------- -------- -------- -------- --------
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "bprint.h"
#include "pielog.h"

#define PIELOG_MAGIC 0x504C4F47
#define PIELOG_ABI_VERSION 1
#define PIELOG_PART_SIZE (16 * 1024)
#define PIELOG_LABEL 128
#define PIELOG_DRAW_MAX_DEPTH 512

enum { SLOT_FREE = 0, SLOT_CLAIMED = 1, SLOT_LIVE = 2 };

struct Record {
    std::atomic<uint64_t> live_gen;
    int32_t depth;
    uint32_t label_len;
    uint64_t next;
    pielog_span parent;
    double start_time;
    double ndone, ntotal;
    char label[PIELOG_LABEL];
};

struct Slot {
    std::atomic<uint32_t> state, bump, dirty;
    std::atomic<uint64_t> returned; // records freed by others, owed to us
    int64_t pid;
    int64_t tid;
    Pielog *api_ptr; // this process's call table
};

struct RegionHeader {
    uint32_t magic;
    uint32_t abi;
    std::atomic<bool> sink;        // A sink owns the output.
    std::atomic<uint32_t> n_slots; // committed slots
    pthread_mutex_t grow;          // taken on growth only
    double t0;
    char sink_path[128]; // the sink's inbox, valid under sink
};

static_assert(std::atomic<uint32_t>::is_always_lock_free, "shared atomics");
static_assert(std::atomic<uint64_t>::is_always_lock_free, "shared atomics");

struct Map;

/* The client: the region we are attached to, and our identity in it.
 */
static struct {
    RegionHeader *header;
    int fd;
    std::atomic<Map *> map;
    pid_t pid, create_pid;
    char name[64];
} PielogG;

/* The sink: where our lines go -- the terminal renderer's inbox, or our
 * stderr -- and who owns the screen.
 */
static struct {
    std::atomic<bool> on{false}; // This process runs the sink.
    std::atomic<int> inbox{-1};  // our connection to the sink's inbox
    int out = STDERR_FILENO;
    int listen_fd = -1;          // the listening end (owner only)
    std::mutex mtx;              // The screen: our writes wait for repaints.
} PielogSink;

/* Per-context state: our current span, our record pool, our slots.
 */
static thread_local struct {
    pielog_span span = {0, 0, 0};
    uint64_t free_head;
    uint32_t slot;      // where we allocate from; 0 = none yet
    uint8_t *unit;
    std::vector<uint32_t> mine;
} PielogCtxG;

static void _clean_up_at_exit(void);

/* The region grows by doubling -- appended, never moved -- and every
 * process keeps every mapping it made, so an address given out never
 * stops being valid.
 */
#define HEADER_SIZE 4096
#define SLOT_SIZE 64
#define UNIT_SIZE (SLOT_SIZE + PIELOG_PART_SIZE)

static_assert(sizeof(Slot) <= SLOT_SIZE, "slot fits its unit");
static_assert(sizeof(RegionHeader) <= HEADER_SIZE, "header fits its page");
static_assert(sizeof(sockaddr_un::sun_path) <= sizeof(RegionHeader::sink_path), "sink_path holds a sun_path");

struct Map { uint8_t *base; size_t len; };

static Map *
_covering_map(size_t end)
{
    struct stat st;
    int frc = fstat(PielogG.fd, &st);
    assert(frc == 0);
    assert((size_t)st.st_size >= end);
    void *base = mmap(NULL,
                      (size_t)st.st_size,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED,
                      PielogG.fd,
                      0);
    assert(base != MAP_FAILED);
    Map *m = (Map *)malloc(sizeof(Map));
    assert(m != NULL);
    m->base = (uint8_t *)base;
    m->len = (size_t)st.st_size;
    PielogG.map.store(m, std::memory_order_release);
    return m;
}

static inline uint8_t *
_unit(uint32_t slot)
{
    if (likely(slot == PielogCtxG.slot)) return PielogCtxG.unit;
    Map *m = PielogG.map.load(std::memory_order_acquire);
    size_t end = HEADER_SIZE + (size_t)slot * UNIT_SIZE;
    if (unlikely(m == NULL || end > m->len)) m = _covering_map(end);
    return m->base + HEADER_SIZE + (size_t)(slot - 1) * UNIT_SIZE;
}

static inline Slot *_slotp(uint32_t slot) { return (Slot *)_unit(slot); }

static inline Record *
_rec(uint32_t slot, uint32_t idx)
{
    return (Record *)(_unit(slot) + SLOT_SIZE) + (idx - 1);
}

static inline size_t _cap() { return PIELOG_PART_SIZE / sizeof(Record); }

static inline uint32_t
_bump(Slot *s)
{
    uint32_t n = s->bump.load(std::memory_order_relaxed);
    return n > _cap() ? (uint32_t)_cap() : n;
}

static inline uint64_t
_packed(uint32_t slot, uint32_t idx)
{
    return ((uint64_t)slot << 32) | idx;
}

static inline uint32_t _hi(uint64_t c) { return (uint32_t)(c >> 32); }

static inline uint32_t _lo(uint64_t c) { return (uint32_t)c; }

static double
timestamp()
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_FAST)
    clock_gettime(CLOCK_MONOTONIC_FAST, &ts);
#elif defined(CLOCK_MONOTONIC_COARSE)
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Create the shared region and advertise it.
 */
static void
_create_region(void)
{
    snprintf(PielogG.name,
             sizeof(PielogG.name),
             "/pielog-%ld-%llx",
             (long)getpid(),
             (unsigned long long)(uint64_t)(timestamp() * 1e9));
    int fd = shm_open(PielogG.name, O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    int rc = ftruncate(fd, HEADER_SIZE + (off_t)UNIT_SIZE);
    assert(rc == 0);
    void *base =
        mmap(NULL, HEADER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(base != MAP_FAILED);
    memset(base, 0, HEADER_SIZE);
    RegionHeader *h = (RegionHeader *)base;
    h->magic = PIELOG_MAGIC;
    h->abi = PIELOG_ABI_VERSION;
    h->n_slots.store(1, std::memory_order_relaxed);
    pthread_mutexattr_t ma;
    int mrc = pthread_mutexattr_init(&ma);
    assert(mrc == 0);
    mrc = pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    assert(mrc == 0);
    mrc = pthread_mutex_init(&h->grow, &ma);
    assert(mrc == 0);
    pthread_mutexattr_destroy(&ma);
    h->t0 = timestamp();
    PielogG.header = h;
    PielogG.fd = fd;
    PielogG.create_pid = getpid();
    setenv("PIELOG", PielogG.name, 1);
    atexit(_clean_up_at_exit);
}

/* Attach to the region our creator advertised, and return whether
 * it worked: false when it is gone, truncated or not ours (a
 * stale PIELOG env var), so the caller falls back to a fresh
 * region instead of dying.
 */
static bool
_attach_to_region(const char *name)
{
    struct stat st;
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        return false;
    } else if (fstat(fd, &st) != 0 || st.st_size < (off_t)HEADER_SIZE) {
        close(fd);
        return false;
    } else {
        void *base =
            mmap(NULL, HEADER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        assert(base != MAP_FAILED);
        RegionHeader *h = (RegionHeader *)base;
        if (h->magic != PIELOG_MAGIC || h->abi != PIELOG_ABI_VERSION) {
            munmap(base, HEADER_SIZE);
            close(fd);
            return false;
        } else {
            PielogG.header = h;
            PielogG.fd = fd;
            snprintf(PielogG.name, sizeof(PielogG.name), "%s", name);
            return true;
        }
    }
}

/* Reclaim dead threads' slots. Caller must hold the growth
 * lock.
 */
static void
_reclaim_dead_slots(void)
{
    uint32_t n = PielogG.header->n_slots.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; i++) {
        Slot &s = *_slotp(i + 1);
        uint32_t st = s.state.load(std::memory_order_acquire);
        if (st == SLOT_LIVE && s.tid > 0 && kill((pid_t)s.tid, 0) != 0 &&
            errno != EPERM) {
            uint32_t expected = st;
            s.state.compare_exchange_strong(expected,
                                            SLOT_FREE,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed);
        }
    }
}

/* Grow the region: reclaim the dead slots, or double it.
 */
static void
_grow_region(uint32_t seen)
{
    pthread_mutex_lock(&PielogG.header->grow);
    uint32_t n = PielogG.header->n_slots.load(std::memory_order_acquire);
    if (n == seen) {
        _reclaim_dead_slots();
        bool full = true;
        for (uint32_t i = 1; (i <= n) && full; i++)
            full = _slotp(i)->state.load(std::memory_order_acquire) != SLOT_FREE;
        if (full) {
            int rc = ftruncate(PielogG.fd, HEADER_SIZE + (off_t)(2 * n) * (off_t)UNIT_SIZE);
            assert(rc == 0);
            PielogG.header->n_slots.store(2 * n, std::memory_order_release);
        }
    }
    pthread_mutex_unlock(&PielogG.header->grow);
}

static inline int64_t
_tid()
{
#if defined(SYS_gettid)
    return (int64_t)syscall(SYS_gettid);
#else
    uint64_t t;
    pthread_threadid_np(NULL, &t);
    return (int64_t)t;
#endif
}

static uint32_t
_slot(void)
{
    for (;;) {
        uint32_t n = PielogG.header->n_slots.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; i++) {
            Slot &s = *_slotp(i + 1);
            uint32_t expected = SLOT_FREE;
            if (s.state.compare_exchange_strong(expected,
                                                SLOT_CLAIMED,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                uint32_t b = s.bump.exchange(0, std::memory_order_relaxed);
                if (b > _cap()) b = (uint32_t)_cap();
                for (uint32_t j = 1; j <= b; j++) {
                    _rec(i + 1, j)->live_gen.fetch_and(
                        0xffffffff00000000ull, std::memory_order_relaxed);
                }
                s.returned.store(0, std::memory_order_relaxed);
                s.dirty.store(1, std::memory_order_relaxed);
                s.pid = (int64_t)getpid();
                s.tid = _tid();
                s.api_ptr = pielog ? pielog : &pielog_api;
                s.state.store(SLOT_LIVE, std::memory_order_release);
                return i + 1;
            }
        }
        _grow_region(n);
    }
}

/* Claim our thread's slot; it stamps our process's call table.
 */
static void
_claim_slot(void)
{
    uint32_t s = _slot();
    assert(s != 0);
    PielogCtxG.unit = _unit(s);
    PielogCtxG.slot = s;
    PielogCtxG.mine.push_back(s);
}

/* First use brings the region up, once: two first users must
 * not race the creation. A forked child redoes the setup by
 * hand: the once cannot run twice in a process, and may have
 * been in flight at the fork.
 */
static pthread_once_t _region_once = PTHREAD_ONCE_INIT;

/* Attach to our creator's region, or create our own.
 */
static void
_attach_or_create_region(void)
{
    const char *name = getenv("PIELOG");
    if (name && *name && _attach_to_region(name)) {
        return;
    } else {
        _create_region();
    }
    assert(PielogG.header != NULL);
}

static void
_ensure_region(void)
{
    if (likely(PielogG.header != NULL)) return;
    pid_t self = getpid();
    pid_t was = PielogG.pid;
    // Stamped before the once: a fork mid-setup must find it,
    // or the child would wait on the stuck once.
    PielogG.pid = self;
    if (was != 0 && was != self) {
        _attach_or_create_region(); // The fork landed mid-setup: the once is stuck.
    } else {
        pthread_once(&_region_once, _attach_or_create_region);
    }
}

/* Return the call table this process runs on: its slots carry
 * it, so every image of the process shares one table and one
 * thread context.
 */
static Pielog *
_call_table(void)
{
    uint32_t self = (uint32_t)getpid();
    uint32_t n = PielogG.header->n_slots.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; i++) {
        Slot &s = *_slotp(i + 1);
        // This might fail if pids are reused.
        if (s.state.load(std::memory_order_acquire) == SLOT_LIVE &&
            s.pid == (int32_t)self) {
            return s.api_ptr;
        }
    }
    return NULL;
}

/* fork() left one thread in a fresh process: drop the inherited
 * context and renderer. No slot is claimed: an exec keeps the
 * pid, and its replacement image must not inherit one.
 */
static void
_drop_inherited_state(void)
{
    PielogCtxG = {};
    PielogSink.on.store(false);
}

/* At fork, detach the child: drop the table too -- it re-acquires
 * one the normal way, if it ever wants to log.
 */
static void
_detach_forked_child(void)
{
    _drop_inherited_state();
    pielog = nullptr;
}

/* Ensure the record pool stays ours across a fork: a child must
 * not share its slot with the parent's thread. Only begin and
 * end touch the pool, so only they look -- and pay the getpid.
 */
static void
_ensure_pool(void)
{
    _ensure_region();
    pid_t self = getpid();
    if (likely(PielogG.pid == self)) {
        return;
    } else {
        PielogG.pid = self;
        _drop_inherited_state();
    }
}

const char *
pielog_region_name(void)
{
    _ensure_region();
    return PielogG.name;
}

/* Clean up what we own at exit.
 */
static void
_clean_up_at_exit(void)
{
    if (getpid() != PielogG.create_pid) return;
    if (PielogSink.listen_fd >= 0) unlink(PielogG.header->sink_path);
    if (PielogG.name[0]) shm_unlink(PielogG.name);
}

/* Connect to the sink's inbox, and return its fd, or -1.
 */
static int
_connect_to_inbox(void)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    memcpy(sa.sun_path, PielogG.header->sink_path, sizeof(sa.sun_path));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (unlikely(fd < 0 ||
                 connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0)) {
        if (fd >= 0) close(fd);
        return -1;
    } else {
#if defined(SO_NOSIGPIPE)
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, NULL, 0);
#endif
        return fd;
    }
}

/* Send to the sink's inbox and return the result: a dead
 * sink must cost us an EPIPE, not a signal (MSG_NOSIGNAL, or
 * SO_NOSIGPIPE where that is the only way).
 */
static ssize_t
_send_to_inbox(int fd, struct iovec *iov, size_t n)
{
    struct msghdr msg = {};
    msg.msg_iov = iov;
    msg.msg_iovlen = n;
#if defined(MSG_NOSIGNAL)
    return sendmsg(fd, &msg, MSG_NOSIGNAL);
#else
    return sendmsg(fd, &msg, 0);
#endif
}

/* Drop our connection to the sink; the next line goes to our
 * stderr.
 */
static void
_drop_connection(void)
{
    int old = PielogSink.inbox.exchange(-1, std::memory_order_acq_rel);
    if (old >= 0) close(old);
}

/* Return where the next line goes: the sink's inbox, or our stderr.
 */
static int
_out_fd(void)
{
    _ensure_region();
    int fd = PielogSink.inbox.load(std::memory_order_acquire);
    if (PielogG.header &&
        PielogG.header->sink.load(std::memory_order_acquire)) {
        if (fd < 0) {
            int mine = _connect_to_inbox(), expected = -1;
            if (!PielogSink.inbox.compare_exchange_strong(
                    expected,
                    mine,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                close(mine); // Another thread of ours connected first.
                fd = expected;
            } else {
                fd = mine;
            }
        }
        return fd >= 0 ? fd : PielogSink.out;
    } else {
        if (fd >= 0) _drop_connection();
        return PielogSink.out;
    }
}

static void
_emit(int depth,
      const char *marker,
      const char *text,
      size_t len,
      double dur = -1)
{
    int fd = _out_fd();
    char head[64 + 4 * PIELOG_DRAW_MAX_DEPTH], tail[64], sec[32];
    int n = 0, m = 0;
    n += snprintf(
        head + n, sizeof(head) - n, "%6.3f ", timestamp() - PielogG.header->t0);
    for (int i = 0; i < depth && n + 4 <= (int)sizeof(head); i++, n += 4)
        memcpy(head + n, "│ ", 4);
    if (dur >= 0) {
        fmt_seconds(sec, sizeof(sec), dur);
        m += snprintf(tail + m, sizeof(tail) - m, " (%s)", sec);
    }
    snprintf(tail + m, sizeof(tail) - m, "\n");
    struct iovec iov[4] = {{head, (size_t)n},
                           {(void *)marker, strlen(marker)},
                           {(void *)text, len},
                           {tail, strlen(tail)}};
    if (fd != PielogSink.out) {
        if (unlikely(_send_to_inbox(fd, iov, 4) < 0)) {
            _drop_connection();
        } else {
            return;
        }
    }
    if (PielogSink.on.load(std::memory_order_relaxed) && PielogG.header) {
        std::lock_guard<std::mutex> lock(PielogSink.mtx);
        ssize_t r = writev(PielogSink.out, iov, 4);
        (void)r;
    } else {
        ssize_t r = writev(PielogSink.out, iov, 4);
        (void)r;
    }
}

static Record *
_new_rec(pielog_span *ref)
{
    for (;;) {
        if (PielogCtxG.free_head) {
            uint64_t c = PielogCtxG.free_head;
            Record *r = _rec(_hi(c), _lo(c));
            PielogCtxG.free_head = r->next;
            uint64_t v = r->live_gen.fetch_add(((uint64_t)1 << 32) | 1, std::memory_order_acq_rel);
            *ref = pielog_span{_hi(c), _lo(c), (uint32_t)(v >> 32) + 1};
            return r;
        } else if (!PielogCtxG.slot) {
            _claim_slot();
        } else {
            bool any = false;
            for (uint32_t s : PielogCtxG.mine) {
                uint64_t c = _slotp(s)->returned.exchange(0, std::memory_order_acquire);
                if (c != 0) {
                    any = true;
                    while (c != 0) {
                        Record *r = _rec(_hi(c), _lo(c));
                        uint64_t next = r->next;
                        r->next = PielogCtxG.free_head;
                        PielogCtxG.free_head = c;
                        c = next;
                    }
                }
            }
            if (!any) {
                uint32_t i = _slotp(PielogCtxG.slot)
                                 ->bump.fetch_add(1, std::memory_order_relaxed);
                if (i < _cap()) {
                    Record *r = _rec(PielogCtxG.slot, i + 1);
                    uint64_t v = r->live_gen.fetch_add(((uint64_t)1 << 32) | 1,
                                                      std::memory_order_acq_rel);
                    *ref = pielog_span{PielogCtxG.slot, i + 1, (uint32_t)(v >> 32) + 1};
                    return r;
                } else {
                    PielogCtxG.slot = 0;
                }
            }
        }
    }
}

/* Free the record for this end, and return whether this end won
 * it: a record is freed at most once. The CAS carries the gen, so
 * a stale end -- one whose record changed hands after the ref was
 * resolved -- loses, and cannot free the new occupant's record.
 */
static bool
_free_rec(uint32_t slot, uint32_t idx, Record *r, uint32_t gen)
{
    uint64_t want = ((uint64_t)gen << 32) | 1;
    if (!r->live_gen.compare_exchange_strong(want, want & ~(uint64_t)1,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
        return false;
    } else {
        uint64_t c = _packed(slot, idx);
        if (slot == PielogCtxG.slot) {
            r->next = PielogCtxG.free_head;
            PielogCtxG.free_head = c;
        } else { // Ours of an older slot, or another thread's: return it.
            Slot &s = *_slotp(slot);
            uint64_t head = s.returned.load(std::memory_order_relaxed);
            do {
                r->next = head;
            } while (!s.returned.compare_exchange_weak(
                head, c, std::memory_order_release, std::memory_order_relaxed));
        }
        return true;
    }
}

static inline pielog_span _root(void) { return pielog_span{0, 0, 0}; }

static inline Record *
_live_rec(pielog_span ref)
{
    if (ref.proc == 0 || ref.rec == 0 ||
        ref.proc > PielogG.header->n_slots.load(std::memory_order_relaxed)) {
        return NULL;
    } else {
        Slot &s = *_slotp(ref.proc);
        if (s.state.load(std::memory_order_acquire) != SLOT_LIVE ||
            ref.rec > _bump(&s)) {
            return NULL;
        } else {
            Record *r = _rec(ref.proc, ref.rec);
            uint64_t v = r->live_gen.load(std::memory_order_acquire);
            return (uint32_t)v == 1 && (uint32_t)(v >> 32) == ref.gen ? r : NULL;
        }
    }
}

static inline uint32_t
_gen(const Record *r)
{
    return (uint32_t)(r->live_gen.load(std::memory_order_acquire) >> 32);
}

static inline int
_depth(pielog_span c)
{
    Record *r = _live_rec(c);
    return r ? (int)r->depth : -1;
}

static void
_set_dirty(uint32_t slot)
{
    if (slot) _slotp(slot)->dirty.store(1, std::memory_order_relaxed);
}

static void
_set_label(Record *r, const char *text, size_t len)
{
    if (unlikely(len > PIELOG_LABEL)) {
        len = PIELOG_LABEL - 3;
        while (len && ((unsigned char)text[len] & 0xC0) == 0x80)
            len--;
        memcpy(r->label, text, len);
        memcpy(r->label + len, "\xe2\x80\xa6", 3);
        len += 3;
    } else {
        memcpy(r->label, text, len);
    }
    r->label_len = (uint32_t)len;
}

void
pielog_pr(pielog_span s, const char *text, size_t len)
{
    _ensure_region();
    _emit(_depth(s) + 1, "", text, len);
}

pielog_span
pielog_begin(pielog_span s, const char *text, size_t len)
{
    _ensure_pool();
    pielog_span ref;
    Record *r = _new_rec(&ref);
    r->parent = s;
    r->start_time = timestamp();
    r->depth = _depth(s);
    r->ndone = r->ntotal = 0.0;
    _set_label(r, text, len);
    _set_dirty(ref.proc);
    return ref;
}

pielog_span
pielog_end(pielog_span s)
{
    _ensure_pool();
    Record *r = _live_rec(s);
    if (!r) {
        return _root();
    } else {
        pielog_span out = r->parent;
        if (!_free_rec(s.proc, s.rec, r, s.gen)) {
            return _root();
        } else {
            _set_dirty(s.proc);
            return out;
        }
    }
}

pielog_span
pielog_log_begin(pielog_span s, const char *text, size_t len)
{
    pielog_span out = pielog_begin(s, text, len);
    Record *r = _live_rec(out);
    r->depth++;
    _emit((int)r->depth, "╭ ", r->label, r->label_len);
    return out;
}

pielog_span
pielog_log_end(pielog_span s)
{
    _ensure_region();
    char label[PIELOG_LABEL];
    size_t len = 0;
    int depth = _depth(s);
    double t0 = timestamp();
    if (Record *r = _live_rec(s)) {
        len = r->label_len;
        memcpy(label, r->label, len);
        t0 = r->start_time;
    }
    pielog_span out = pielog_end(s);
    _emit(depth, "╰ ", label, len, timestamp() - t0);
    return out;
}

void
pielog_set_text(pielog_span s, const char *text, size_t len)
{
    _ensure_region();
    Record *r = _live_rec(s);
    if (!r) {
        return;
    } else {
        _set_label(r, text, len);
        _set_dirty(s.proc);
    }
}

void
pielog_set_progress(pielog_span s, double done, double total)
{
    _ensure_region();
    Record *r = _live_rec(s);
    if (!r) {
        return;
    } else if (!(isfinite(done) && isfinite(total))) {
        return;
    } else {
        r->ndone = done;
        r->ntotal = total;
        _set_dirty(s.proc);
    }
}

pielog_span
pielog_here(void)
{
    _ensure_region();
    return PielogCtxG.span;
}

void pielog_set(pielog_span span) { PielogCtxG.span = span; }

pielog_span
pielog_root(void)
{
    _ensure_region();
    return _root();
}

/* C API
 */
Pielog pielog_api = {
    PIELOG_ABI_VERSION,
    pielog_pr,
    pielog_begin,
    pielog_end,
    pielog_log_begin,
    pielog_log_end,
    pielog_set_text,
    pielog_set_progress,
    pielog_here,
    pielog_set,
};

Pielog *
pielog_get(void)
{
    _ensure_region();
    Pielog *t = _call_table();
    if (likely(t != NULL)) {
        return t;
    } else {
        if (!PielogCtxG.slot) _claim_slot();
        return _call_table();
    }
}

/* Connect to a parent pielog, or start our own.
 */
static void __attribute__((constructor))
_at_program_start(void)
{
    pielog = pielog_get();
    pthread_atfork(NULL, NULL, _detach_forked_child);
}

/* -------- -------- -------- -------- -------- -------- -------- --------
 * TERMINAL RENDERER
 * -------- -------- -------- -------- -------- -------- -------- --------
 */

#define PIELOG_DRAW_MAX_CHILDREN 16
#define PIELOG_FLUSH_MAX 1024

static struct {
    std::vector<int> client_fds;
    pthread_t thread;
    Buf lines;
    Buf tree; // the cached tree view, or the one-shot teardown
    long width;
    bool shown; // The view is on screen below the cursor.
    int last_lines;
    char rbuf[4096];
} PielogT;

void pielog_start_terminal(void);
void pielog_stop_terminal(void);

/* Listen on the sink's inbox, and return its fd or -1: one
 * connection per client process, lines pre-composed and passed
 * to the screen unchanged. The bound path goes in the header.
 */
static int
_listen_on_inbox(void)
{
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "/tmp%s-sink", PielogG.name);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    unlink(sa.sun_path);
    if (unlikely(fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
                 listen(fd, 16) != 0)) {
        if (fd >= 0) close(fd);
        return -1;
    } else {
        memcpy(PielogG.header->sink_path, sa.sun_path, sizeof(sa.sun_path));
        fcntl(fd, F_SETFL, O_NONBLOCK);
        return fd;
    }
}

/* Tree renderer: fit the live forest of spans to the screen. Stale
 * parents draw as [detached]; wide subtrees collapse into one
 * "… +N …" line -- what fits is the sink's choice.
 */
struct Node {
    const Record *r;
    std::vector<Node *> children;
    bool detached;
};

static void
_bprint_bar(Buf &b, double frac, int width)
{
    static const char *half[] = {"╶", "╸"};
    if (!(frac > 0)) frac = 0;
    if (!(frac < 1)) frac = 1;
    double f = width * frac;
    int n = (int)f;
    if (f < 0.5) {
        for (int i = 0; i < width; i++) b.puts("─");
    } else {
        for (int i = 0; i < n; i++) b.puts("━");
        if (n < width) {
            b.puts(half[int((f - n) * 2)]);
            for (int i = n + 1; i < width; i++) b.puts("─");
        }
    }
}

static void
_bprint_prefix(Buf &b, int depth, const int *is_last)
{
    if (depth == 0) {
        return b.puts("⯁ ");
    } else {
        b.puts("  ");
        for (int i = 1; i < depth; i++) b.puts(is_last[i] ? "   " : "│  ");
        b.puts(is_last[depth] ? "╰─ " : "├─ ");
    }
}

static long
_term_width(void)
{
    struct winsize ws;
    if (ioctl(PielogSink.out, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    } else {
        return 80;
    }
}

static long _draw(Buf &b, Node *n, int depth, std::vector<int> &is_last, bool last);

static long
_draw_children(Buf &b,
               std::vector<Node *> &children,
               int depth,
               std::vector<int> &is_last)
{
    long n = (long)children.size();
    long shown = n > PIELOG_DRAW_MAX_CHILDREN ? PIELOG_DRAW_MAX_CHILDREN - 1 : n;
    long count = 0;
    for (long i = 0; i < shown; i++)
        count += _draw(
            b, children[i], depth + 1, is_last, n == shown && i + 1 == shown);
    if (n > shown) {
        if ((int)is_last.size() <= depth + 2) is_last.resize(depth + 4, 0);
        is_last[depth + 1] = 1;
        _bprint_prefix(b, depth + 1, is_last.data());
        b._nprintf(32, "… +%ld …\033[K\n", n - shown);
        count++;
    }
    return count;
}

static long
_draw(Buf &b, Node *n, int depth, std::vector<int> &is_last, bool last)
{
    if ((int)is_last.size() <= depth + 1) is_last.resize(depth + 4, 0);
    is_last[depth] = last ? 1 : 0;
    _bprint_prefix(b, depth, is_last.data());
    const Record &f = *n->r;
    b.write(f.label, f.label_len);
    if (f.ntotal > 0) {
        b.putc(' ');
        _bprint_bar(b, f.ndone / f.ntotal, 20);
    }
    if (n->detached) b.puts(" \033[2m[detached]\033[0m");
    // A lone child chains inline -- "parent -> child -> grandchild ..." --
    // instead of a nested line per span, while the line fits. What does
    // not fit (and its subtree) drops back to the tree below.
    long used = (depth == 0 ? 2 : 2 + 3 * depth) + (long)f.label_len +
                (f.ntotal > 0 ? 21 : 0) + (n->detached ? 11 : 0);
    while (n->children.size() == 1) {
        const Record &g = *n->children[0]->r;
        long add = 3 + (long)g.label_len + (g.ntotal > 0 ? 21 : 0);
        if (used + add > PielogT.width) {
            break;
        } else {
            used += add;
            b.puts(" → ");
            b.write(g.label, g.label_len);
            if (g.ntotal > 0) {
                b.putc(' ');
                _bprint_bar(b, g.ndone / g.ntotal, 20);
            }
            n = n->children[0];
        }
    }
    b.puts("\033[K\n");
    long count = 1;
    if (depth >= PIELOG_DRAW_MAX_DEPTH) {
        return count;
    } else {
        return count + _draw_children(b, n->children, depth, is_last);
    }
}

/* Rescan the span tree and update PielogT.tree. Return whether
 * it changed.
 */
static bool
_recompose_tree(bool force)
{
    if (!PielogG.header) return false;
    PielogT.width = _term_width();
    uint32_t nslots = PielogG.header->n_slots.load(std::memory_order_acquire);
    std::deque<Node> nodes;
    std::vector<std::vector<Node *>> at(nslots);
    bool any = false;
    for (uint32_t s = 0; s < nslots; s++) {
        Slot &sl = *_slotp(s + 1);
        if (sl.state.load(std::memory_order_acquire) == SLOT_LIVE) {
            any |= sl.dirty.exchange(false, std::memory_order_acq_rel);
            at[s].assign(_bump(&sl), NULL);
            for (uint32_t j = 0; j < at[s].size(); j++) {
                const Record *r = _rec(s + 1, j + 1);
                if ((uint32_t)r->live_gen.load(std::memory_order_acquire) == 1) {
                    nodes.push_back(Node{r, {}, false});
                    at[s][j] = &nodes.back();
                }
            }
        }
    }
    if (!any && !force) {
        return false;
    } else if (nodes.empty()) {
        PielogT.tree.clear();
        PielogT.last_lines = 0;
        return PielogT.shown;
    } else {
        PielogT.tree.clear();
        Buf &b = PielogT.tree;
        for (auto &n : nodes) {
            pielog_span par = n.r->parent;
            if (par.rec != 0) {
                Node *t = NULL;
                if (par.proc > 0 && par.proc <= nslots &&
                    par.rec <= at[par.proc - 1].size()) {
                    t = at[par.proc - 1][par.rec - 1];
                    if (t && _gen(t->r) != par.gen) t = NULL;
                }
                if (t) {
                    t->children.push_back(&n);
                } else {
                    n.detached = true;
                }
            }
        }
        b.putc('\n');
        std::vector<Node *> roots;
        for (auto &n : nodes)
            if (n.detached || n.r->parent.rec == 0) roots.push_back(&n);
        std::vector<int> is_last(16, 0);
        long line_count = 1 + _draw_children(b, roots, -1, is_last);
        b.puts("\033[J");
        for (; line_count < PielogT.last_lines; line_count++)
            b.putc('\n');
        b._nprintf(16, "\033[%dA", (int)line_count);
        PielogT.last_lines = (int)line_count;
        PielogT.shown = true;
        return true;
    }
}

static void
_flush(void)
{
    if (!PielogT.shown && PielogT.lines.len == 0 && PielogT.tree.len == 0)
        return;
    struct iovec iov[3] = {{(void *)"\033[J", (size_t)(PielogT.shown ? 3 : 0)},
                           {PielogT.lines.data, PielogT.lines.len},
                           {PielogT.tree.data, PielogT.tree.len}};
    ssize_t r = writev(PielogSink.out, iov, 3);
    (void)r;
    PielogT.lines.clear();
    PielogT.shown = PielogT.tree.len > 0;
}

/* Fill the message buffer from the sink's inbox (render thread).
 */
static void
_fill_message_buffer(void)
{
    for (size_t i = 0; i < PielogT.client_fds.size();) {
        ssize_t k = read(
            PielogT.client_fds[i], PielogT.rbuf, sizeof(PielogT.rbuf));
        if (k > 0) {
            PielogT.lines.write(PielogT.rbuf, (size_t)k);
            if (PielogT.lines.len >= PIELOG_FLUSH_MAX) _flush();
        } else if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(PielogT.client_fds[i]);
            PielogT.client_fds.erase(PielogT.client_fds.begin() + i);
        } else {
            i++;
        }
    }
}

/* Start a terminal renderer of the trace, unless a different
 * one is already started by a parent process.
 */
void
pielog_start_terminal(void)
{
    _ensure_region();
    if (PielogSink.on.load()) return;
    if (PielogG.header && getpid() != PielogG.create_pid) return;
    PielogSink.listen_fd = PielogG.header ? _listen_on_inbox() : -1;
    PielogSink.on.store(true);
    atexit(pielog_stop_terminal);
    auto render = [](void *) -> void * {
        std::vector<struct pollfd> pfd;
        double painted = 0;
        while (PielogSink.on.load()) {
            pfd.clear();
            pfd.push_back(pollfd{PielogSink.listen_fd, POLLIN, 0});
            for (int fd : PielogT.client_fds)
                pfd.push_back(pollfd{fd, POLLIN, 0});
            poll(pfd.data(), (nfds_t)pfd.size(), 200);
            if (pfd[0].revents & POLLIN) {
                for (;;) {
                    int c = accept(PielogSink.listen_fd, NULL, NULL);
                    if (c < 0) {
                        break;
                    } else {
                        fcntl(c, F_SETFL, O_NONBLOCK);
                        fcntl(c, F_SETFD, FD_CLOEXEC);
                        PielogT.client_fds.push_back(c);
                    }
                }
            }
            std::lock_guard<std::mutex> lock(PielogSink.mtx);
            _fill_message_buffer();
            bool fresh = false;
            if (timestamp() - painted >= 0.2) {
                painted = timestamp();
                fresh = _recompose_tree(false);
            }
            if (PielogT.lines.len || fresh) _flush();
        }
        return NULL;
    };
    if (pthread_create(&PielogT.thread, NULL, render, NULL) != 0) {
        PielogSink.on.store(false); // No renderer: lines go to stderr.
    } else if (PielogSink.listen_fd >= 0) {
        PielogG.header->sink.store(true, std::memory_order_release);
    }
}

/* Stop the terminal renderer, if it was started by us.
 */
void
pielog_stop_terminal(void)
{
    if (!PielogSink.on.load()) return;
    PielogG.header->sink.store(false, std::memory_order_release);
    PielogSink.on.store(false);
    pthread_join(PielogT.thread, NULL);
    for (int fd : PielogT.client_fds) {
        ssize_t k;
        while ((k = read(fd, PielogT.rbuf, sizeof(PielogT.rbuf))) > 0)
            PielogT.lines.write(PielogT.rbuf, (size_t)k);
        close(fd);
    }
    PielogT.client_fds.clear();
    if (PielogT.lines.len || PielogT.tree.len) _flush();
    if (PielogSink.listen_fd >= 0) {
        close(PielogSink.listen_fd);
        unlink(PielogG.header->sink_path);
        PielogSink.listen_fd = -1;
    }
    PielogT.last_lines = 0;
    PielogT.shown = false;
    PielogT.tree.clear();
    ssize_t r = write(PielogSink.out, "\033[J", 3);
    (void)r;
}
