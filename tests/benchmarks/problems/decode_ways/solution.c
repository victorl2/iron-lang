#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t decode_ways(int64_t* digits, int64_t n) {
    if (n == 0) return 0;
    int64_t dp[22];
    memset(dp, 0, sizeof(dp));
    dp[0] = 1;
    dp[1] = digits[0] != 0 ? 1 : 0;
    for (int64_t i = 2; i <= n; i++) {
        if (digits[i-1] != 0) dp[i] = dp[i-1];
        int64_t two = digits[i-2] * 10 + digits[i-1];
        if (two >= 10 && two <= 26) dp[i] = dp[i] + dp[i-2];
    }
    return dp[n];
}

int main(void) {
    int64_t d1[] = {1, 2};
    printf("Test 1: %lld (expected 2)\n", (long long)decode_ways(d1, 2));
    int64_t d2[] = {2, 2, 6};
    printf("Test 2: %lld (expected 3)\n", (long long)decode_ways(d2, 3));

    int64_t bench[20];
    for (int i = 0; i < 20; i++) bench[i] = (i % 3) + 1;
    printf("Test 3: %lld (expected 1458)\n", (long long)decode_ways(bench, 20));

    int iterations = 4000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = decode_ways(bench, 20);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Decode Ways ===\n");
    printf("Digits: 20\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
