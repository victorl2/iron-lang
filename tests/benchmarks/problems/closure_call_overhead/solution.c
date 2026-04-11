/* solution.c — C reference for closure call overhead benchmark.
 * Uses function pointer (simulating Iron_Closure) to call mix(x,y) in a loop,
 * matching the Iron main.iron program. The body is deliberately more expensive
 * than plain add to prevent clang from vectorizing the sum-reduction — the
 * call overhead is still the dominant cost this benchmark measures. */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static int64_t mix_direct(int64_t x, int64_t y) { return (x * 31 + y * 17) % 10007; }

typedef struct { void *env; int64_t (*fn)(void*, int64_t, int64_t); } Closure;
static int64_t mix_via_closure(void *e, int64_t x, int64_t y) { (void)e; return (x * 31 + y * 17) % 10007; }

int main(void) {
    const int64_t iterations = 10000000;
    Closure cl = { NULL, mix_via_closure };

    /* Runtime-sampled seed defeats clang constant folding of pure-integer sums. */
    struct timespec seed_ts;
    clock_gettime(CLOCK_MONOTONIC, &seed_ts);
    int64_t seed = (seed_ts.tv_nsec % 7) + 1;

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    int64_t sum_direct = 0;
    for (int64_t i = 0; i < iterations; i++)
        sum_direct += mix_direct(i, seed);

    int64_t sum_closure = 0;
    for (int64_t i = 0; i < iterations; i++)
        sum_closure += cl.fn(cl.env, i, seed);

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    int64_t ms = (ts1.tv_sec - ts0.tv_sec) * 1000 + (ts1.tv_nsec - ts0.tv_nsec) / 1000000;

    if (sum_direct == sum_closure) printf("MATCH\n");
    printf("Total time: %lld ms\n", (long long)ms);
    printf("Sum: %lld\n", (long long)sum_direct);
    return 0;
}
