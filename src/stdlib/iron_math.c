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
    /* Ensure seed is non-zero (xorshift64 produces 0 forever if state=0) */
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
    /* Map to [0.0, 1.0) using 53 mantissa bits for precision */
    uint64_t bits = Iron_rng_next(rng) >> 11;  /* 53-bit integer */
    return (double)bits / (double)(1ULL << 53);
}

/* ── Global convenience (thread-local RNG) ───────────────────────────────── */

static void s_global_rng_init(Iron_RNG *rng) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t seed = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec;
    *rng = Iron_rng_create(seed);
}

double Iron_math_random(void) {
    /* WINDOWS-TODO: __thread is GCC/Clang thread-local storage. On Windows use __declspec(thread).
     * A portable IRON_THREAD_LOCAL macro should be defined in iron_runtime.h (see existing #ifdef _WIN32
     * pattern there) and used here: static IRON_THREAD_LOCAL Iron_RNG s_global_rng; */
    static __thread Iron_RNG s_global_rng;
    static __thread int s_global_rng_initialized = 0;
    if (!s_global_rng_initialized) {
        s_global_rng_init(&s_global_rng);
        s_global_rng_initialized = 1;
    }
    return Iron_rng_next_float(&s_global_rng);
}

int64_t Iron_math_random_int(int64_t min, int64_t max) {
    /* WINDOWS-TODO: same __thread → IRON_THREAD_LOCAL as above */
    static __thread Iron_RNG s_global_rng;
    static __thread int s_global_rng_initialized = 0;
    if (!s_global_rng_initialized) {
        s_global_rng_init(&s_global_rng);
        s_global_rng_initialized = 1;
    }
    return Iron_rng_next_int(&s_global_rng, min, max);
}
