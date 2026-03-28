#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t lcs(int64_t* a, int64_t na, int64_t* b, int64_t nb) {
    int64_t dp[21 * 21];
    memset(dp, 0, sizeof(dp));
    int64_t cols = nb + 1;
    for (int64_t i = 1; i <= na; i++) {
        for (int64_t j = 1; j <= nb; j++) {
            if (a[i-1] == b[j-1]) {
                dp[i*cols+j] = dp[(i-1)*cols+(j-1)] + 1;
            } else {
                int64_t up = dp[(i-1)*cols+j];
                int64_t left = dp[i*cols+(j-1)];
                dp[i*cols+j] = up > left ? up : left;
            }
        }
    }
    return dp[na*cols+nb];
}

int main(void) {
    int64_t a1[] = {1,3,4,1,2,3};
    int64_t b1[] = {3,1,4,2,3};
    printf("Test 1: %lld (expected 4)\n", (long long)lcs(a1,6,b1,5));

    int64_t a2[20], b2[20];
    for (int i = 0; i < 20; i++) { a2[i] = (i*7+3)%10; b2[i] = (i*11+5)%10; }
    printf("Test 2: %lld (expected 7)\n", (long long)lcs(a2,20,b2,20));

    int iterations = 500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = lcs(a2, 20, b2, 20);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Longest Common Subsequence ===\n");
    printf("Sequence lengths: 20, 20\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
