#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t maximal_square(int64_t* matrix, int64_t rows, int64_t cols) {
    int64_t dp[21 * 21];
    int64_t best = 0;
    for (int64_t i = 0; i < rows; i++) {
        for (int64_t j = 0; j < cols; j++) {
            if (matrix[i*cols+j] == 0) {
                dp[i*cols+j] = 0;
            } else if (i == 0 || j == 0) {
                dp[i*cols+j] = 1;
            } else {
                int64_t a = dp[(i-1)*cols+j];
                int64_t b = dp[i*cols+(j-1)];
                int64_t c = dp[(i-1)*cols+(j-1)];
                int64_t mn = a < b ? a : b;
                if (c < mn) mn = c;
                dp[i*cols+j] = mn + 1;
            }
            if (dp[i*cols+j] > best) best = dp[i*cols+j];
        }
    }
    return best * best;
}

int main(void) {
    int64_t m1[] = {1,0,1,0,0, 1,0,1,1,1, 1,1,1,1,1, 1,0,0,1,0};
    printf("Test 1: %lld (expected 4)\n", (long long)maximal_square(m1, 4, 5));

    int64_t bench[400];
    for (int i = 0; i < 20; i++)
        for (int j = 0; j < 20; j++)
            bench[i*20+j] = ((i*5+j*3)%11 == 0) ? 0 : 1;
    printf("Test 2: %lld (expected 16)\n", (long long)maximal_square(bench, 20, 20));

    int iterations = 500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = maximal_square(bench, 20, 20);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Maximal Square ===\n");
    printf("Matrix: 20x20\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
