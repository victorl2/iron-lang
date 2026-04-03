#include "iron_math.h"
#include <time.h>

/* ── Trig and math wrappers ──────────────────────────────────────────────── */

double Iron_math_sin(double x)                   { return sin(x); }
double Iron_math_cos(double x)                   { return cos(x); }
double Iron_math_tan(double x)                   { return tan(x); }
double Iron_math_sqrt(double x)                  { return sqrt(x); }
double Iron_math_pow(double base, double exp_)   { return pow(base, exp_); }
double Iron_math_floor(double x)                 { return floor(x); }
double Iron_math_ceil(double x)                  { return ceil(x); }
double Iron_math_round(double x)                 { return round(x); }

/* ── Lerp ────────────────────────────────────────────────────────────────── */

double Iron_math_lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

/* ── RNG — xorshift64 ────────────────────────────────────────────────────── */

Iron_RNG Iron_rng_create(uint64_t seed) {
    Iron_RNG rng;
    rng.state = seed != 0 ? seed : 0xDEADBEEFCAFEBABEULL;
    return rng;
}

uint64_t Iron_rng_next(Iron_RNG *rng) {
    uint64_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng->state = x;
    return x;
}

int64_t Iron_rng_next_int(Iron_RNG *rng, int64_t min, int64_t max) {
    if (max <= min) return min;
    uint64_t range = (uint64_t)(max - min);
    return min + (int64_t)(Iron_rng_next(rng) % range);
}

double Iron_rng_next_float(Iron_RNG *rng) {
    uint64_t bits = Iron_rng_next(rng) >> 11;
    return (double)bits / (double)(1ULL << 53);
}

/* ── File-scope thread-local RNG state (shared by random, random_int, seed) ─ */
/* WINDOWS-TODO: __thread is GCC/Clang. On Windows use __declspec(thread).
 * A portable IRON_THREAD_LOCAL macro should be defined in iron_runtime.h
 * and used here instead. */

static __thread Iron_RNG s_math_rng;
static __thread int      s_math_rng_init = 0;

static void s_math_rng_lazy_init(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t seed = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec;
    s_math_rng = Iron_rng_create(seed);
    s_math_rng_init = 1;
}

/* ── Global convenience (thread-local RNG seeded from time) ─────────────── */

void Iron_math_seed(int64_t n) {
    s_math_rng = Iron_rng_create((uint64_t)n);
    s_math_rng_init = 1;
}

double Iron_math_random(void) {
    if (!s_math_rng_init) s_math_rng_lazy_init();
    return Iron_rng_next_float(&s_math_rng);
}

int64_t Iron_math_random_int(int64_t min, int64_t max) {
    if (!s_math_rng_init) s_math_rng_lazy_init();
    return Iron_rng_next_int(&s_math_rng, min, max);
}

/* ── Phase 39: New math functions ────────────────────────────────────────── */

double  Iron_math_asin(double x)                  { return asin(x); }
double  Iron_math_acos(double x)                  { return acos(x); }
double  Iron_math_atan2(double y, double x)       { return atan2(y, x); }
double  Iron_math_log(double x)                   { return log(x); }
double  Iron_math_log2(double x)                  { return log2(x); }
double  Iron_math_exp(double x)                   { return exp(x); }
double  Iron_math_hypot(double a, double b)       { return hypot(a, b); }

double Iron_math_random_float(double min, double max) {
    return Iron_math_random() * (max - min) + min;
}

int64_t Iron_math_sign(double x) {
    if (x > 0.0) return  1;
    if (x < 0.0) return -1;
    return 0;
}
