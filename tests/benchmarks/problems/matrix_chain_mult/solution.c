#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t matrix_chain(int64_t* dims, int64_t n) {
    int64_t dp[11 * 11];
    memset(dp, 0, sizeof(dp));
    for (int64_t cl = 2; cl <= n; cl++) {
        for (int64_t i = 0; i <= n - cl; i++) {
            int64_t j = i + cl - 1;
            dp[i*n+j] = 999999999;
            for (int64_t k = i; k < j; k++) {
                int64_t cost = dp[i*n+k] + dp[(k+1)*n+j] + dims[i]*dims[k+1]*dims[j+1];
                if (cost < dp[i*n+j]) dp[i*n+j] = cost;
            }
        }
    }
    return dp[0*n+(n-1)];
}

int main(void) {
    int64_t d1[] = {10,30,5,60};
    printf("Test 1: %lld (expected 4500)\n", (long long)matrix_chain(d1, 3));
    int64_t d2[] = {40,20,30,10,30};
    printf("Test 2: %lld (expected 26000)\n", (long long)matrix_chain(d2, 4));
    int64_t d3[] = {5,10,3,12,5,50,6,8,15,20,25};
    printf("Test 3: %lld (expected 5199)\n", (long long)matrix_chain(d3, 10));

    int iterations = 500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = matrix_chain(d3, 10);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Matrix Chain Multiplication ===\n");
    printf("Matrices: 10\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
