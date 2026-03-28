#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t unique_paths(int64_t m, int64_t n) {
    int64_t dp[16 * 16];
    memset(dp, 0, sizeof(dp));
    for (int64_t i = 0; i < m; i++) {
        for (int64_t j = 0; j < n; j++) {
            if (i == 0 || j == 0) dp[i*n+j] = 1;
            else dp[i*n+j] = dp[(i-1)*n+j] + dp[i*n+(j-1)];
        }
    }
    return dp[(m-1)*n+(n-1)];
}

int main(void) {
    printf("Test 1: %lld (expected 28)\n", (long long)unique_paths(3, 7));
    printf("Test 2: %lld (expected 40116600)\n", (long long)unique_paths(15, 15));

    int iterations = 1000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = unique_paths(15, 15);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Unique Paths ===\n");
    printf("Grid: 15x15\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
