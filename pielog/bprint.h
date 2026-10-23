/* Formatted printing to a memory buffer.
 */

#pragma once

#include "utils.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string_view>
#include <vector>

struct Buf {
    char *data = NULL;
    size_t len = 0;
    size_t cap = 0;
    ~Buf() { free(data); data = NULL; len = cap = 0; }
    void
    _grow(size_t more)
    {
        size_t new_cap = cap ? cap * 2 : 128;
        while (new_cap < len + more) new_cap *= 2;
        data = (char *)realloc(data, new_cap);
        assert(data != NULL);
        cap = new_cap;
    }
    inline void _provision(size_t more) { if (unlikely(cap < len + more)) _grow(more); }
    void clear() { len = 0; }
    void write(const char *s, size_t n) { _provision(n); memcpy(data + len, s, n); len += n; }
    inline void puts(const char *s) { write(s, strlen(s)); }
    inline void putc(char c) { _provision(1); data[len++] = c; }
    template <typename... Args> inline void
    _nprintf(size_t n, const char *fmt, const Args &...args)
    {
        _provision(n + 1);
        int k = snprintf(data + len, n + 1, fmt, args...);
        assert((0 <= k) && ((size_t)k < n + 1));
        len += k;
    }
    template <typename... Args> inline void
    _printf(const char *fmt, const Args &...args)
    {
        _provision(16);
        for (;;) {
            size_t n = cap - len;
            int k = snprintf(data + len, n, fmt, args...);
            assert(k >= 0);
            if ((size_t)k < n) {
                len += k;
                break;
            } else {
                _grow(k + 16);
            }
        }
    }
    template <typename... Args> void
    print(const char *fmt, const Args &...args)
    {
        auto next_format = [](const char *fmt, ssize_t idx) {
            for (ssize_t i = idx;; i++) {
                if (fmt[i] == '{') return i + 2;
                if (fmt[i] == 0) return i;
            }
        };
        (void)next_format;
        ssize_t idx = 0, next;
        ((next = next_format(fmt, idx),
          (next - idx - 2 > 0) ? write(fmt + idx, (size_t)(next - idx - 2)) : (void)0,
          idx = next,
          bprint(*this, args)),
         ...);
        puts(fmt + idx);
    }
    std::string_view str() const { return {data, len}; }
};

/* Generic formatting. Users are expected to define bprint on
 * their types to make Buf.print work on them too.
 */

static inline void bprint(Buf &b, int n) { b._nprintf(22, "%d", n); }
static inline void bprint(Buf &b, long n) { b._nprintf(22, "%ld", n); }
static inline void bprint(Buf &b, long long n) { b._nprintf(41, "%lld", n); }
static inline void bprint(Buf &b, unsigned int n) { b._nprintf(22, "%u", n); }
static inline void bprint(Buf &b, unsigned long n) { b._nprintf(22, "%lu", n); }
static inline void bprint(Buf &b, unsigned long long n) { b._nprintf(41, "%llu", n); }
static inline void bprint(Buf &b, float n) { b._nprintf(17, "%.8e", n); }
static inline void bprint(Buf &b, double n) { b._nprintf(25, "%.16e", n); }
static inline void bprint(Buf &b, bool v) { b.puts(v ? "true" : "false"); }
static inline void bprint(Buf &b, const char *s) { b.puts(s); }

template <typename T> static void
bprint(Buf &b, const std::vector<T> &x)
{
    size_t size = x.size();
    for (size_t i = 0; i < size; i++) {
        if (i != 0) b.putc(' ');
        bprint(b, x[i]);
    }
}

template <typename T> struct BFMT { const char *fmt; const T &val; };
template <typename T> static BFMT<T> bfmt(const char *fmt, const T &val) { return {fmt, val}; };
template <typename T> static inline void bprint(Buf &b, const BFMT<T> &x) { b._printf(x.fmt, x.val); }

struct BFMT_SEC { const double seconds; };
static inline BFMT_SEC bfmt_seconds(double sec) { return {sec}; };

/* seconds -> "1.23ms" style */
static inline size_t fmt_seconds(char *dst, size_t cap, double t) {
    const char *format;
    double scale;
    if (t < 1e-8) { format = "%.3fns"; scale = 1./1e-9; }
    else if (t < 1e-7) { format = "%.2fns"; scale = 1./1e-9; }
    else if (t < 1e-6) { format = "%.1fns"; scale = 1./1e-9; }
    else if (t < 1e-5) { format = "%.3f\u03bcs"; scale = 1./1e-6; }
    else if (t < 1e-4) { format = "%.2f\u03bcs"; scale = 1./1e-6; }
    else if (t < 1e-3) { format = "%.1f\u03bcs"; scale = 1./1e-6; }
    else if (t < 1e-2) { format = "%.3fms"; scale = 1./1e-3; }
    else if (t < 1e-1) { format = "%.2fms"; scale = 1./1e-3; }
    else if (t < 1) { format = "%.1fms"; scale = 1./1e-3; }
    else if (t < 10) { format = "%.2fs"; scale = 1.; }
    else if (t < 60) { format = "%.1fs"; scale = 1.; }
    else if (t < 60*60) { format = "%.2fm"; scale = 1./60; }
    else if (t < 24*60*60) { format = "%.2fh"; scale = 1./(60*60); }
    else if (t < 7*24*60*60) { format = "%.2fd"; scale = 1./(24*60*60); }
    else if (t < 365*24*60*60) { format = "%.2fw"; scale = 1./(7*24*60*60); }
    else { format = "%.3fy"; scale = 1./(365*24*60*60); }
    return (size_t)snprintf(dst, cap, format, t*scale);
}

static inline void bprint(Buf &b, const BFMT_SEC x) {
    char s[32];
    b.write(s, fmt_seconds(s, sizeof(s), x.seconds));
}

struct BFMT_B { const double val; };
static inline BFMT_B bfmt_bytes(double val) { return {val}; };
static inline void bprint(Buf &b, const BFMT_B x) {
    const char *unit;
    double scale;
    if (x.val < (1ull<<10)) { unit = "B"; scale = 1.; }
    else if (x.val < (1ull<<20)) { unit = "kB"; scale = 1./(1ull<<10); }
    else if (x.val < (1ull<<30)) { unit = "MB"; scale = 1./(1ull<<20); }
    else if (x.val < (1ull<<40)) { unit = "GB"; scale = 1./(1ull<<30); }
    else if (x.val < (1ull<<50)) { unit = "TB"; scale = 1./(1ull<<40); }
    else { unit = "PB"; scale = 1./(1ull<<50); }
    b._printf("%.1f%s", x.val*scale, unit);
}

/* fprint.h interface
 */

static inline void
fprint(FILE *f, const Buf &b)
{
    fwrite(b.data, b.len, 1, f);
}
