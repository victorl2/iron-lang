#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t max_repeated_subarray(int64_t* a, int64_t na, int64_t* b, int64_t nb) {
    int64_t dp[31 * 31];
    memset(dp, 0, sizeof(dp));
    int64_t cols = nb + 1;
    int64_t best = 0;
    for (int64_t i = 1; i <= na; i++) {
        for (int64_t j = 1; j <= nb; j++) {
            if (a[i-1] == b[j-1]) {
                dp[i*cols+j] = dp[(i-1)*cols+(j-1)] + 1;
                if (dp[i*cols+j] > best) best = dp[i*cols+j];
            } else {
                dp[i*cols+j] = 0;
            }
        }
    }
    return best;
}

int main(void) {
    int64_t a1[] = {1,2,3,2,1};
    int64_t b1[] = {3,2,1,4,7};
    printf("Test 1: %lld (expected 3)\n", (long long)max_repeated_subarray(a1,5,b1,5));

    int64_t a2[30], b2[30];
    for (int i = 0; i < 30; i++) { a2[i] = i % 8; b2[i] = (i+3) % 8; }
    printf("Test 2: %lld (expected 27)\n", (long long)max_repeated_subarray(a2,30,b2,30));

    int iterations = 200000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = max_repeated_subarray(a2, 30, b2, 30);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Max Length Repeated Subarray ===\n");
    printf("Array sizes: 30, 30\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
