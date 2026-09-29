#include "iron_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/* ── print / println ─────────────────────────────────────────────────────── */

void Iron_print(Iron_String s) {
    const char *cstr = iron_string_cstr(&s);
    printf("%s", cstr);
}

void Iron_println(Iron_String s) {
    const char *cstr = iron_string_cstr(&s);
    printf("%s\n", cstr);
}

/* ── len ─────────────────────────────────────────────────────────────────── */

int64_t Iron_len(Iron_String s) {
    return (int64_t)iron_string_codepoint_count(&s);
}

/* ── Integer arithmetic builtins ─────────────────────────────────────────── */

int64_t Iron_min(int64_t a, int64_t b) {
    return a < b ? a : b;
}

int64_t Iron_max(int64_t a, int64_t b) {
    return a > b ? a : b;
}

int64_t Iron_clamp(int64_t val, int64_t lo, int64_t hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

int64_t Iron_abs(int64_t val) {
    return val < 0 ? -val : val;
}

/* ── assert ──────────────────────────────────────────────────────────────── */

void Iron_assert(bool cond, Iron_String msg) {
    if (!cond) {
        /* Buffered stdout dies with abort(); flush it first so a failing
         * program keeps every line it printed before the assertion, in
         * order (same discipline as the iron_panic_* helpers). */
        fflush(stdout);
        fprintf(stderr, "assertion failed: %s\n", iron_string_cstr(&msg));
        fflush(stderr);
        abort();
    }
}

/* ── read_file ──────────────────────────────────────────────────────────── */

/* The builtin read_file(path) outside comptime: the whole file as a String.
 * At compile time an unreadable file is a build error; at run time it is a
 * panic (IO.read_file returns a Result-style value for recoverable reads). */
Iron_String Iron_read_file(Iron_String path) {
    const char *p = iron_string_cstr(&path);
    FILE *f = fopen(p, "rb");
    if (!f) {
        fflush(stdout);
        fprintf(stderr, "panic: read_file: cannot open '%s'\n", p);
        fflush(stderr);
        abort();
    }
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) iron_oom_abort("read_file");
    size_t n;
    while ((n = fread(buf + len, 1, cap - len, f)) > 0) {
        len += n;
        if (len == cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); iron_oom_abort("read_file"); }
            buf = nb;
        }
    }
    bool failed = ferror(f) != 0;
    fclose(f);
    if (failed) {
        free(buf);
        fflush(stdout);
        fprintf(stderr, "panic: read_file: error reading '%s'\n", p);
        fflush(stderr);
        abort();
    }
    Iron_String s = iron_string_from_cstr(buf, len);
    free(buf);
    return s;
}

/* ── range ──────────────────────────────────────────────────────────────── */
/* Iron_range is now static inline in iron_runtime.h */
