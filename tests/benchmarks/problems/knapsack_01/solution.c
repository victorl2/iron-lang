#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t knapsack(int64_t* weights, int64_t* values, int64_t n, int64_t capacity) {
    int64_t dp[21 * 51];
    memset(dp, 0, sizeof(dp));
    int64_t cols = capacity + 1;
    for (int64_t i = 1; i <= n; i++) {
        for (int64_t w = 0; w <= capacity; w++) {
            dp[i*cols+w] = dp[(i-1)*cols+w];
            if (weights[i-1] <= w) {
                int64_t val = dp[(i-1)*cols+(w-weights[i-1])] + values[i-1];
                if (val > dp[i*cols+w]) dp[i*cols+w] = val;
            }
        }
    }
    return dp[n*cols+capacity];
}

int main(void) {
    int64_t w1[] = {2,3,4,5};
    int64_t v1[] = {3,4,5,6};
    printf("Test 1: %lld (expected 13)\n", (long long)knapsack(w1,v1,4,10));

    int64_t w2[20], v2[20];
    for (int i = 0; i < 20; i++) { w2[i] = (i%5)+1; v2[i] = (i*3+2)%20+1; }
    printf("Test 2: %lld (expected 202)\n", (long long)knapsack(w2,v2,20,50));

    int iterations = 500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = knapsack(w2, v2, 20, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: 0/1 Knapsack ===\n");
    printf("Items: 20, Capacity: 50\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
