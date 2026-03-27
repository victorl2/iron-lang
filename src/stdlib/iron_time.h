#ifndef IRON_TIME_H
#define IRON_TIME_H

#include <stdint.h>
#include <time.h>

/* ── Monotonic time ──────────────────────────────────────────────────────── */
double  Iron_time_now(void);         /* wall-clock seconds as double */
int64_t Iron_time_now_ms(void);      /* monotonic milliseconds */
void    Iron_time_sleep(int64_t ms); /* sleep for given milliseconds */

/* ── Timer ───────────────────────────────────────────────────────────────── */
/* IRON_TIMER_STRUCT_DEFINED is set by the Iron codegen before including this
 * header.  In that case the struct body is emitted by the compiler from the
 * Iron 'object Timer' declaration; we emit only the forward typedef here to
 * avoid a struct redefinition conflict. */
#ifndef IRON_TIMER_STRUCT_DEFINED
typedef struct Iron_Timer { int64_t start_ms; } Iron_Timer;
#else
typedef struct Iron_Timer Iron_Timer;  /* codegen emits the struct body */
#endif

Iron_Timer Iron_timer_create(void);
int64_t    Iron_timer_since(Iron_Timer t);    /* ms elapsed since create/reset */
Iron_Timer Iron_timer_reset(Iron_Timer t);    /* returns new timer with reset start */

#endif /* IRON_TIME_H */
