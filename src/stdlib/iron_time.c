#include "iron_time.h"
#include <stdlib.h>

/* ── Wall-clock time (seconds) ───────────────────────────────────────────── */

double Iron_time_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1.0e9;
}

/* ── Monotonic time (milliseconds) ──────────────────────────────────────── */

int64_t Iron_time_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
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
