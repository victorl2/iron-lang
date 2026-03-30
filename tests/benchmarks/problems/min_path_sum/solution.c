#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t min_path_sum(int64_t* grid, int64_t rows, int64_t cols) {
    int64_t dp[21 * 21];
    dp[0] = grid[0];
    for (int64_t j = 1; j < cols; j++) dp[j] = dp[j-1] + grid[j];
    for (int64_t i = 1; i < rows; i++) {
        dp[i*cols] = dp[(i-1)*cols] + grid[i*cols];
        for (int64_t j = 1; j < cols; j++) {
            int64_t up = dp[(i-1)*cols+j];
            int64_t left = dp[i*cols+(j-1)];
            int64_t mn = up < left ? up : left;
            dp[i*cols+j] = mn + grid[i*cols+j];
        }
    }
    return dp[(rows-1)*cols+(cols-1)];
}

int main(void) {
    int64_t g1[] = {1,3,1, 1,5,1, 4,2,1};
    printf("Test 1: %lld (expected 7)\n", (long long)min_path_sum(g1, 3, 3));

    int64_t bench[400];
    for (int i = 0; i < 20; i++)
        for (int j = 0; j < 20; j++)
            bench[i*20+j] = (i*3+j*7+5)%10+1;
    printf("Test 2: %lld (expected 131)\n", (long long)min_path_sum(bench, 20, 20));

    int iterations = 1500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = min_path_sum(bench, 20, 20);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Min Path Sum ===\n");
    printf("Grid: 20x20\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
