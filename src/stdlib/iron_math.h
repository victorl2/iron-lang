#ifndef IRON_MATH_H
#define IRON_MATH_H

#include <stdint.h>
#include <math.h>

/* ── Constants ───────────────────────────────────────────────────────────── */
#define IRON_PI  3.14159265358979323846
#define IRON_TAU 6.28318530717958647692
#define IRON_E   2.71828182845904523536

/* ── Math object constant fields (Iron_Math_PI etc.) ─────────────────────── */
/* These #defines map the compiler-emitted Iron_Math_XXX identifiers (produced
 * by Iron code `Math.PI`, `Math.TAU`, `Math.E`) to the raw #define values.
 * The Iron codegen emits type-name field accesses as Iron_TypeName_FieldName. */
#define Iron_Math_PI  IRON_PI
#define Iron_Math_TAU IRON_TAU
#define Iron_Math_E   IRON_E

/* ── Trig and math wrappers ──────────────────────────────────────────────── */
double Iron_math_sin(double x);
double Iron_math_cos(double x);
double Iron_math_tan(double x);
double Iron_math_sqrt(double x);
double Iron_math_pow(double base, double exp);
double Iron_math_floor(double x);
double Iron_math_ceil(double x);
double Iron_math_round(double x);

/* ── Lerp ────────────────────────────────────────────────────────────────── */
double Iron_math_lerp(double a, double b, double t);

/* ── RNG — xorshift64 ────────────────────────────────────────────────────── */
typedef struct { uint64_t state; } Iron_RNG;

Iron_RNG Iron_rng_create(uint64_t seed);
uint64_t Iron_rng_next(Iron_RNG *rng);
int64_t  Iron_rng_next_int(Iron_RNG *rng, int64_t min, int64_t max);
double   Iron_rng_next_float(Iron_RNG *rng);  /* [0.0, 1.0) */

/* ── Global convenience (thread-local RNG seeded from time) ─────────────── */
double  Iron_math_random(void);                        /* [0.0, 1.0) */
int64_t Iron_math_random_int(int64_t min, int64_t max);

/* ── Phase 39 additions ──────────────────────────────────────────────────── */
double  Iron_math_asin(double x);
double  Iron_math_acos(double x);
double  Iron_math_atan2(double y, double x);
int64_t Iron_math_sign(double x);
void    Iron_math_seed(int64_t n);
double  Iron_math_random_float(double min, double max);
double  Iron_math_log(double x);
double  Iron_math_log2(double x);
double  Iron_math_exp(double x);
double  Iron_math_hypot(double a, double b);

#endif /* IRON_MATH_H */
