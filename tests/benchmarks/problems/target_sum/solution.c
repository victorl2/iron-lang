#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t target_sum(int64_t* nums, int64_t n, int64_t target) {
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) total += nums[i];
    int64_t sum = total + target;
    if (sum % 2 != 0) return 0;
    if (sum < 0) return 0;
    int64_t s = sum / 2;
    int64_t dp[1001];
    memset(dp, 0, sizeof(dp));
    dp[0] = 1;
    for (int64_t i = 0; i < n; i++) {
        for (int64_t j = s; j >= nums[i]; j--) {
            dp[j] = dp[j] + dp[j - nums[i]];
        }
    }
    return dp[s];
}

int main(void) {
    int64_t a1[] = {1,1,1,1,1};
    printf("Test 1: %lld (expected 5)\n", (long long)target_sum(a1, 5, 3));

    int64_t bench[15];
    for (int i = 0; i < 15; i++) bench[i] = (i%5)+1;
    printf("Test 2: %lld (expected 1932)\n", (long long)target_sum(bench, 15, 3));

    int iterations = 500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = target_sum(bench, 15, 3);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Target Sum ===\n");
    printf("Array size: 15\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
