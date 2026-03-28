#include <stdio.h>
#include <stdint.h>
#include <time.h>

int64_t house_robber(int64_t* nums, int64_t n) {
    if (n == 0) return 0;
    if (n == 1) return nums[0];
    int64_t prev2 = nums[0];
    int64_t prev1 = nums[1];
    if (prev2 > prev1) prev1 = prev2;
    for (int64_t i = 2; i < n; i++) {
        int64_t pick = prev2 + nums[i];
        int64_t curr = prev1;
        if (pick > curr) curr = pick;
        prev2 = prev1;
        prev1 = curr;
    }
    return prev1;
}

int main(void) {
    int64_t a1[] = {1, 2, 3, 1};
    printf("Test 1: %lld (expected 4)\n", (long long)house_robber(a1, 4));
    int64_t a2[] = {2, 7, 9, 3, 1};
    printf("Test 2: %lld (expected 12)\n", (long long)house_robber(a2, 5));

    int64_t bench[50];
    for (int i = 0; i < 50; i++) bench[i] = (i * 17 + 11) % 100;
    int64_t bench_result = house_robber(bench, 50);
    printf("Test 3: %lld (expected 1379)\n", (long long)bench_result);

    int iterations = 1000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = house_robber(bench, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: House Robber ===\n");
    printf("Array size: 50\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
