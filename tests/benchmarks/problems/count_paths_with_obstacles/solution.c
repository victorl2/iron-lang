#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t count_paths_obstacles(int64_t* grid, int64_t rows, int64_t cols) {
    int64_t dp[16 * 16];
    memset(dp, 0, sizeof(dp));
    if (grid[0] == 1) return 0;
    dp[0] = 1;
    for (int64_t j = 1; j < cols; j++) {
        if (grid[j] == 0 && dp[j-1] == 1) dp[j] = 1; else dp[j] = 0;
    }
    for (int64_t i = 1; i < rows; i++) {
        if (grid[i*cols] == 0 && dp[(i-1)*cols] == 1) dp[i*cols] = 1; else dp[i*cols] = 0;
        for (int64_t j = 1; j < cols; j++) {
            if (grid[i*cols+j] == 1) {
                dp[i*cols+j] = 0;
            } else {
                dp[i*cols+j] = dp[(i-1)*cols+j] + dp[i*cols+(j-1)];
            }
        }
    }
    return dp[(rows-1)*cols+(cols-1)];
}

int main(void) {
    int64_t g1[] = {0,0,0, 0,1,0, 0,0,0};
    printf("Test 1: %lld (expected 2)\n", (long long)count_paths_obstacles(g1, 3, 3));

    int64_t bench[225];
    memset(bench, 0, sizeof(bench));
    for (int i = 0; i < 15; i++)
        for (int j = 0; j < 15; j++)
            bench[i*15+j] = ((i*3+j*7)%11 == 0 && i > 0 && j > 0) ? 1 : 0;
    bench[14*15+14] = 0;
    printf("Test 2: %lld (expected 1449957)\n", (long long)count_paths_obstacles(bench, 15, 15));

    int iterations = 1000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = count_paths_obstacles(bench, 15, 15);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Count Paths With Obstacles ===\n");
    printf("Grid: 15x15\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
