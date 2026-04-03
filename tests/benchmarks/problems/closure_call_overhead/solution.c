/* solution.c — C reference for closure call overhead benchmark.
 * Uses function pointer (simulating Iron_Closure) to call add(x,y)
 * in a loop, matching the Iron main.iron program. */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static int64_t add_direct(int64_t x, int64_t y) { return x + y; }

typedef struct { void *env; int64_t (*fn)(void*, int64_t, int64_t); } Closure;
static int64_t add_via_closure(void *e, int64_t x, int64_t y) { (void)e; return x + y; }

int main(void) {
    const int64_t iterations = 1000000;
    Closure cl = { NULL, add_via_closure };

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    int64_t sum_direct = 0;
    for (int64_t i = 0; i < iterations; i++)
        sum_direct += add_direct(i, 1);

    int64_t sum_closure = 0;
    for (int64_t i = 0; i < iterations; i++)
        sum_closure += cl.fn(cl.env, i, 1);

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    int64_t ms = (ts1.tv_sec - ts0.tv_sec) * 1000 + (ts1.tv_nsec - ts0.tv_nsec) / 1000000;

    printf("direct: %lld\n", (long long)sum_direct);
    printf("closure: %lld\n", (long long)sum_closure);
    if (sum_direct == sum_closure) printf("MATCH\n");
    printf("Total time: %lld ms\n", (long long)ms);
    return 0;
}
