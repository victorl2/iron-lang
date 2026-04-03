/* solution.c — C reference for closure call overhead benchmark.
 * Equivalent computation using function pointer dispatch to simulate
 * Iron_Closure calling convention (fn pointer + env pointer). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Simulated Iron_Closure fat-pointer */
typedef struct {
    int64_t (*fn)(void *env, int64_t x);
    void *env;
} Closure;

/* Environment for capturing closure */
typedef struct { int64_t multiplier; } EnvCapturing;

/* Lifted functions */
static int64_t fn_direct(int64_t x)          { return x * 2; }
static int64_t fn_non_cap(void *e, int64_t x) { (void)e; return x * 2; }
static int64_t fn_cap(void *e, int64_t x)    {
    EnvCapturing *env = (EnvCapturing *)e;
    return x * env->multiplier;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

int main(void) {
    const int n = 500;
    const int iterations = 1000000;

    int64_t *arr = malloc((size_t)n * sizeof(int64_t));
    for (int i = 0; i < n; i++) arr[i] = i;

    /* Direct call baseline */
    int64_t t1 = now_ms();
    int64_t sum1 = 0;
    for (int it = 0; it < iterations; it++)
        for (int idx = 0; idx < n; idx++)
            sum1 += fn_direct(arr[idx]);
    int64_t e1 = now_ms() - t1;

    /* Non-capturing closure */
    Closure nc = { fn_non_cap, NULL };
    int64_t t2 = now_ms();
    int64_t sum2 = 0;
    for (int it = 0; it < iterations; it++)
        for (int idx = 0; idx < n; idx++)
            sum2 += nc.fn(nc.env, arr[idx]);
    int64_t e2 = now_ms() - t2;

    /* Capturing closure */
    EnvCapturing env = { 2 };
    Closure cap = { fn_cap, &env };
    int64_t t3 = now_ms();
    int64_t sum3 = 0;
    for (int it = 0; it < iterations; it++)
        for (int idx = 0; idx < n; idx++)
            sum3 += cap.fn(cap.env, arr[idx]);
    int64_t e3 = now_ms() - t3;

    printf("=== Closure Call Overhead Benchmark ===\n");
    printf("Array size: %d, Iterations: %d\n", n, iterations);
    printf("Direct call (ms):            %lld\n", (long long)e1);
    printf("Non-capturing closure (ms):  %lld\n", (long long)e2);
    printf("Capturing closure (ms):      %lld\n", (long long)e3);
    printf("All results equal: %s\n",
           (sum1 == sum2 && sum2 == sum3) ? "true" : "false");

    free(arr);
    return 0;
}
