#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t partition_equal_subset(int64_t* nums, int64_t n) {
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) total += nums[i];
    if (total % 2 != 0) return 0;
    int64_t target = total / 2;
    int64_t dp[1001];
    memset(dp, 0, sizeof(dp));
    dp[0] = 1;
    for (int64_t i = 0; i < n; i++) {
        for (int64_t s = target; s >= nums[i]; s--) {
            if (dp[s - nums[i]]) dp[s] = 1;
        }
    }
    return dp[target];
}

int main(void) {
    int64_t a1[] = {1,5,11,5};
    printf("Test 1: %lld (expected 1)\n", (long long)partition_equal_subset(a1, 4));
    int64_t a2[] = {1,2,3,5};
    printf("Test 2: %lld (expected 0)\n", (long long)partition_equal_subset(a2, 4));

    int64_t bench[20];
    for (int i = 0; i < 20; i++) bench[i] = (i*3+1)%10+1;
    printf("Test 3: %lld (expected 1)\n", (long long)partition_equal_subset(bench, 20));

    int iterations = 500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = partition_equal_subset(bench, 20);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Partition Equal Subset Sum ===\n");
    printf("Array size: 20\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
