#ifndef IRON_TIME_H
#define IRON_TIME_H

#include <stdint.h>
#include <time.h>

/* ── Monotonic time ──────────────────────────────────────────────────────── */
double  Iron_time_now(void);         /* wall-clock seconds as double */
int64_t Iron_time_now_ms(void);      /* monotonic milliseconds */
void    Iron_time_sleep(int64_t ms); /* sleep for given milliseconds */

/* ── Timer ───────────────────────────────────────────────────────────────── */
typedef struct { int64_t start_ms; } Iron_Timer;

Iron_Timer Iron_timer_create(void);
int64_t    Iron_timer_since(const Iron_Timer *t); /* ms elapsed since create/reset */
void       Iron_timer_reset(Iron_Timer *t);

#endif /* IRON_TIME_H */
