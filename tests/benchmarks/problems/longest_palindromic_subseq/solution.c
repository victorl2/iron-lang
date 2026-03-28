#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t lps(int64_t* seq, int64_t n) {
    int64_t dp[31 * 31];
    memset(dp, 0, sizeof(dp));
    for (int64_t i = 0; i < n; i++) dp[i*n+i] = 1;
    for (int64_t cl = 2; cl <= n; cl++) {
        for (int64_t i = 0; i <= n - cl; i++) {
            int64_t j = i + cl - 1;
            if (seq[i] == seq[j]) {
                dp[i*n+j] = dp[(i+1)*n+(j-1)] + 2;
            } else {
                int64_t a = dp[(i+1)*n+j];
                int64_t b = dp[i*n+(j-1)];
                dp[i*n+j] = a > b ? a : b;
            }
        }
    }
    return dp[0*n+(n-1)];
}

int main(void) {
    int64_t s1[] = {2,2,1,2,2,1,2};
    printf("Test 1: %lld (expected 6)\n", (long long)lps(s1, 7));

    int64_t bench[30];
    for (int i = 0; i < 30; i++) bench[i] = (i*7+3)%5;
    printf("Test 2: %lld (expected 11)\n", (long long)lps(bench, 30));

    int iterations = 200000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = lps(bench, 30);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Longest Palindromic Subsequence ===\n");
    printf("Sequence length: 30\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
