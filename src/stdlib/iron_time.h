#ifndef IRON_TIME_H
#define IRON_TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* ── Monotonic time ──────────────────────────────────────────────────────── */
double  Iron_time_now(void);         /* wall-clock seconds as double */
int64_t Iron_time_now_ms(void);      /* monotonic milliseconds */
void    Iron_time_sleep(int64_t ms); /* sleep for given milliseconds */
double  Iron_time_since(double start); /* elapsed seconds since start */

/* ── Timer (accumulator style) ───────────────────────────────────────────── */
/* IRON_TIMER_STRUCT_DEFINED is set by Iron codegen before including this
 * header.  In that case the struct body is emitted from the Iron 'object Timer'
 * declaration; we emit only the forward typedef to avoid redefinition. */
#ifndef IRON_TIMER_STRUCT_DEFINED
typedef struct Iron_Timer { int64_t elapsed_ms; int64_t duration_ms; } Iron_Timer;
#else
typedef struct Iron_Timer Iron_Timer;  /* codegen emits the struct body */
#endif

Iron_Timer Iron_time_Timer(double duration_s);  /* constructor: time.Timer(2.0) */
bool       Iron_timer_done(Iron_Timer t);
void       Iron_timer_update(Iron_Timer *t, double dt);
void       Iron_timer_reset(Iron_Timer *t);

#endif /* IRON_TIME_H */
