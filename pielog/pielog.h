/* -------- -------- -------- -------- -------- -------- -------- --------
 * P I E L O G
 * -------- -------- -------- -------- -------- -------- -------- --------
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bprint.h"

struct pielog_span { uint32_t proc, rec, gen; };

typedef struct pielog_span pielog_span;

struct Pielog {
    int abi_version;
    void (*pr)(pielog_span s, const char *text, size_t len);
    pielog_span (*begin)(pielog_span s, const char *text, size_t len);
    pielog_span (*end)(pielog_span s);
    pielog_span (*log_begin)(pielog_span s, const char *text, size_t len);
    pielog_span (*log_end)(pielog_span s);
    void (*set_text)(pielog_span s, const char *text, size_t len);
    void (*set_progress)(pielog_span s, double done, double total);
    pielog_span (*here)(void); // calling thread's current span
    void (*set)(pielog_span);
};

typedef struct Pielog Pielog;

pielog_span pielog_here(void);
void pielog_set(pielog_span span);
void pielog_pr(pielog_span s, const char *text, size_t len);
pielog_span pielog_begin(pielog_span s, const char *text, size_t len);
pielog_span pielog_end(pielog_span s);
pielog_span pielog_log_begin(pielog_span s, const char *text, size_t len);
pielog_span pielog_log_end(pielog_span s);
void pielog_set_text(pielog_span s, const char *text, size_t len);
void pielog_set_progress(pielog_span s, double done, double total);
pielog_span pielog_root(void);
const char *pielog_region_name(void);
Pielog *pielog_get(void) __attribute__((weak));
void pielog_start_terminal(void);
void pielog_stop_terminal(void);
void pielog_render_now(void);

/* The call table, linked with pielog.cpp: constant-initialized, so
 * every macro works even before main. Weak, so a consumer that takes
 * the table from the capsule instead (no pielog.cpp linked) carries no
 * symbol for it, starts with pielog == NULL, and assigns it once.
 */
extern Pielog pielog_api __attribute__((weak));

inline Pielog *pielog = &pielog_api;

/* Return the table the macros call through: bound already, or -- once
 * a fork dropped it -- acquired the normal way on first use.
 */
static inline Pielog *
_api(void)
{
    Pielog *p = pielog;
    return p ? p : (pielog_get ? pielog = pielog_get() : pielog);
}

static thread_local Buf _pielog_scratch;

struct _pielog_silent_scope {
    pielog_span saved;
    _pielog_silent_scope() : saved(_api()->here()) {}
    ~_pielog_silent_scope()
    {
        pielog_span _c = _api()->here();
        (void)_api()->end(_c);
        _api()->set(saved);
    }
};

struct _pielog_logged_scope {
    pielog_span saved;
    _pielog_logged_scope() : saved(_api()->here()) {}
    ~_pielog_logged_scope()
    {
        pielog_span _c = _api()->here();
        (void)_api()->log_end(_c);
        _api()->set(saved);
    }
};

struct _pielog_foreign_scope {
    pielog_span saved;
    _pielog_foreign_scope() : saved(_api()->here()) { }
    ~_pielog_foreign_scope() { _api()->set(saved); }
};

#define _PIELOG_CUR _api()->here()

#define pr(...)                                                                \
    (_pielog_scratch.clear(),                                                  \
     _pielog_scratch.print(__VA_ARGS__),                                       \
     _api()->pr(_PIELOG_CUR, _pielog_scratch.data, _pielog_scratch.len))

#define trace_text(...)                                                        \
    (_pielog_scratch.clear(),                                                  \
     _pielog_scratch.print(__VA_ARGS__),                                       \
     _api()->set_text(_PIELOG_CUR, _pielog_scratch.data, _pielog_scratch.len))

#define trace_enter(...)                                                       \
    (_pielog_scratch.clear(),                                                  \
     _pielog_scratch.print(__VA_ARGS__),                                       \
     _api()->set(_api()->begin(                                                \
         _PIELOG_CUR, _pielog_scratch.data, _pielog_scratch.len)))

#define log_trace_enter(...)                                                   \
    (_pielog_scratch.clear(),                                                  \
     _pielog_scratch.print(__VA_ARGS__),                                       \
     _api()->set(_api()->log_begin(                                      \
         _PIELOG_CUR, _pielog_scratch.data, _pielog_scratch.len)))

#define trace_exit() (_api()->set(_api()->end(_PIELOG_CUR)))
#define log_trace_exit() (_api()->set(_api()->log_end(_PIELOG_CUR)))
#define trace_progress(ndone, ntotal) _api()->set_progress(_PIELOG_CUR, ndone, ntotal)
#define trace_here() (_PIELOG_CUR)

#define _PIELOG_CAT2(a, b) a##b
#define _PIELOG_CAT(a, b) _PIELOG_CAT2(a, b)
#define _PIELOG_SCOPE _PIELOG_CAT(_pielog_scope_, __COUNTER__)

#define log_trace_scope(...)                                                   \
    _pielog_logged_scope _PIELOG_SCOPE{};                                      \
    log_trace_enter(__VA_ARGS__);

#define trace_scope(...)                                                       \
    _pielog_silent_scope _PIELOG_SCOPE{};                                      \
    trace_enter(__VA_ARGS__);

#define trace_scope_under(span)                                                \
    _pielog_foreign_scope _PIELOG_SCOPE{};                                     \
    _api()->set(span);
