#include "iron_runtime.h"

#include <stdio.h>
#include <stdlib.h>

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
        fprintf(stderr, "assertion failed: %s\n", iron_string_cstr(&msg));
        abort();
    }
}

/* ── range ──────────────────────────────────────────────────────────────── */
/* Iron_range is now static inline in iron_runtime.h */
