#include "iron_time.h"
#include <stdint.h>
#include <stdlib.h>
#include "util/os.h"   /* clock_gettime / nanosleep on Windows */

/* ── Wall-clock time (seconds) ───────────────────────────────────────────── */

double Iron_time_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
}

/* ── Wall-clock time (milliseconds since the Unix epoch) ─────────────── */

int64_t Iron_time_unix_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

/* ── Monotonic time (milliseconds) ──────────────────────────────────────── */

int64_t Iron_time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

/* ── Monotonic time (nanoseconds) ───────────────────────────────────────── */

int64_t Iron_time_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

/* ── Sleep ───────────────────────────────────────────────────────────────── */

void Iron_time_sleep(int64_t ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&ts, NULL);
}

/* ── Elapsed time ────────────────────────────────────────────────────────── */

double Iron_time_since(double start) {
    return Iron_time_now() - start;
}

/* ── Timer (accumulator style) ───────────────────────────────────────────── */

/* Seconds to whole milliseconds like an Iron Float to Int conversion
 * (manual 2.9): truncating, saturating at the Int limits, NaN to 0. A
 * plain C cast is undefined for those values (#574). */
static int64_t timer_ms(double seconds) {
    double ms = seconds * 1000.0;
    if (ms != ms) return 0;
    if (ms >= 9223372036854775807.0) return INT64_MAX;
    if (ms <= -9223372036854775808.0) return INT64_MIN;
    return (int64_t)ms;
}

Iron_Timer Iron_time_Timer(double duration_s) {
    Iron_Timer t;
    t.elapsed_ms  = 0;
    t.duration_ms = timer_ms(duration_s);
    return t;
}

bool Iron_timer_done(Iron_Timer t) {
    return t.elapsed_ms >= t.duration_ms;
}

void Iron_timer_update(Iron_Timer *t, double dt) {
    int64_t d = timer_ms(dt);
    if (d > 0 && t->elapsed_ms > INT64_MAX - d) t->elapsed_ms = INT64_MAX;
    else if (d < 0 && t->elapsed_ms < INT64_MIN - d) t->elapsed_ms = INT64_MIN;
    else t->elapsed_ms += d;
}

void Iron_timer_reset(Iron_Timer *t) {
    t->elapsed_ms = 0;
}
