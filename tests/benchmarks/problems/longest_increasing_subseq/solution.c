#include <stdio.h>
#include <stdint.h>
#include <time.h>

int64_t lis(int64_t* arr, int64_t n) {
    int64_t dp[51];
    int64_t best = 1;
    for (int64_t i = 0; i < n; i++) {
        dp[i] = 1;
        for (int64_t j = 0; j < i; j++) {
            if (arr[j] < arr[i]) {
                int64_t v = dp[j] + 1;
                if (v > dp[i]) dp[i] = v;
            }
        }
        if (dp[i] > best) best = dp[i];
    }
    return best;
}

int main(void) {
    int64_t a1[] = {10,9,2,5,3,7,101,18};
    printf("Test 1: %lld (expected 4)\n", (long long)lis(a1, 8));

    int64_t bench[50];
    for (int i = 0; i < 50; i++) bench[i] = (i*17+5)%37;
    printf("Test 2: %lld (expected 6)\n", (long long)lis(bench, 50));

    int iterations = 500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = lis(bench, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Longest Increasing Subsequence ===\n");
    printf("Array size: 50\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
