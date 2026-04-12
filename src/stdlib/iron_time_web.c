#include "iron_time.h"
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include <stdlib.h>

/*
 * iron_time_web.c — WebAssembly (Emscripten) implementation of iron_time.h.
 *
 * Native counterpart: src/stdlib/iron_time.c (DO NOT TOUCH — WEB-RUNTIME-06).
 *
 * This file is the sole web-build time source. Phase 7's build_web.c emcc
 * orchestration will link this translation unit INSTEAD OF iron_time.c when
 * --target=web. Phase 4 only produces the file and verifies it compiles
 * standalone under emcc -Wall -Werror (see .github/workflows/web.yml
 * portability probe + CMakeLists.txt test_iron_time_web_symbols gate).
 *
 * Design notes:
 *   - Time source:  emscripten_get_now() is monotonic ms-since-page-load;
 *                   emscripten_date_now() is wall-clock ms-since-epoch.
 *                   Both map onto W3C performance.now() / Date.now() per the
 *                   Emscripten API reference.
 *   - Precision:    W3C performance.now() caps at millisecond precision by
 *                   default (100us in some isolated contexts). Iron_time_now_ns
 *                   therefore returns millisecond precision multiplied into
 *                   nanosecond units. Phase 4 SC1 accepts this as "monotonic
 *                   millisecond-precision clock".
 *   - Sleep policy: Asyncify-based sleep (requires -sASYNCIFY=1) is BANNED
 *                   by the locked project decision (see .planning/STATE.md:
 *                   "Auto-transform while(!WindowShouldClose()) at LIR emit
 *                   time ... not Asyncify"). Iron_time_sleep is therefore a
 *                   spin loop on emscripten_get_now(), blocking the main
 *                   thread for the requested duration. Game code SHOULD use
 *                   emscripten_set_main_loop (wired in Phase 5/6), NOT
 *                   Iron_time_sleep, for frame pacing. The spin-loop exists
 *                   only so programs that call time.sleep() still link.
 *   - Guard:        The entire body is wrapped in #ifdef __EMSCRIPTEN__ so
 *                   that a mis-configured native build that accidentally
 *                   picks up this file fails loudly at compile time. The
 *                   __EMSCRIPTEN__ macro is the canonical web-guard
 *                   established in Phase 3 (see src/runtime/iron_threads.c).
 */

#ifdef __EMSCRIPTEN__

/* ── Wall-clock time (seconds) ───────────────────────────────────────────── */

double Iron_time_now(void) {
    /* emscripten_date_now() returns ms-since-epoch as double (Date.now). */
    return emscripten_date_now() / 1000.0;
}

/* ── Monotonic time (milliseconds) ──────────────────────────────────────── */

int64_t Iron_time_now_ms(void) {
    /* emscripten_get_now() returns monotonic ms-since-page-load as double
     * (performance.now). Truncating to int64 preserves monotonicity. */
    return (int64_t)emscripten_get_now();
}

/* ── Monotonic time (nanoseconds) ───────────────────────────────────────── */

int64_t Iron_time_now_ns(void) {
    /* Web: ms precision returned in ns units — W3C performance.now() spec
     * caps the underlying clock at millisecond precision by default. True
     * nanosecond precision is NOT available in the browser. Phase 4 SC1
     * accepts this trade-off. */
    return (int64_t)(emscripten_get_now() * 1.0e6);
}

/* ── Sleep ───────────────────────────────────────────────────────────────── */

void Iron_time_sleep(int64_t ms) {
    /* Web: blocks the main thread. Do NOT use for >10ms sleeps in a browser
     * context — use emscripten_set_main_loop instead for frame pacing.
     * This implementation exists so programs calling time.sleep() still
     * link; Asyncify-based sleep is rejected (-sASYNCIFY=1 is banned per
     * locked project decision). */
    if (ms <= 0) return;
    double deadline = emscripten_get_now() + (double)ms;
    while (emscripten_get_now() < deadline) {
        /* busy wait — no yield available without Asyncify */
    }
}

/* ── Elapsed time ────────────────────────────────────────────────────────── */

double Iron_time_since(double start) {
    /* Pure arithmetic — duplicated verbatim from iron_time.c so this
     * translation unit is self-contained (zero link-time dependency on
     * iron_time.c). */
    return Iron_time_now() - start;
}

/* ── Timer (accumulator style) ───────────────────────────────────────────── */
/* Timer helpers below are PURE ARITHMETIC and duplicated verbatim from
 * iron_time.c. They have no time source and no OS dependency; duplicating
 * rather than cross-linking keeps iron_time_web.c self-contained and lets
 * Phase 7's link step swap iron_time.c for iron_time_web.c without any
 * fallback shims. */

Iron_Timer Iron_time_Timer(double duration_s) {
    Iron_Timer t;
    t.elapsed_ms  = 0;
    t.duration_ms = (int64_t)(duration_s * 1000.0);
    return t;
}

bool Iron_timer_done(Iron_Timer t) {
    return t.elapsed_ms >= t.duration_ms;
}

void Iron_timer_update(Iron_Timer *t, double dt) {
    t->elapsed_ms += (int64_t)(dt * 1000.0);
}

void Iron_timer_reset(Iron_Timer *t) {
    t->elapsed_ms = 0;
}

#else
#  error "iron_time_web.c must only be compiled under Emscripten"
#endif /* __EMSCRIPTEN__ */
