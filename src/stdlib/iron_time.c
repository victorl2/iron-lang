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

/* ── Timer ───────────────────────────────────────────────────────────────── */

Iron_Timer Iron_timer_create(void) {
    Iron_Timer t;
    t.start_ms = Iron_time_now_ms();
    return t;
}

int64_t Iron_timer_since(const Iron_Timer *t) {
    return Iron_time_now_ms() - t->start_ms;
}

void Iron_timer_reset(Iron_Timer *t) {
    t->start_ms = Iron_time_now_ms();
}
