#include <stdio.h>
#include <stdint.h>
#include <time.h>

int64_t jump_game(int64_t* nums, int64_t n) {
    int64_t max_reach = 0;
    for (int64_t i = 0; i < n; i++) {
        if (i > max_reach) return 0;
        int64_t r = i + nums[i];
        if (r > max_reach) max_reach = r;
    }
    return 1;
}

int main(void) {
    int64_t a1[] = {2,3,1,1,4};
    printf("Test 1: %lld (expected 1)\n", (long long)jump_game(a1, 5));
    int64_t a2[] = {3,2,1,0,4};
    printf("Test 2: %lld (expected 0)\n", (long long)jump_game(a2, 5));

    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = (i*3+1)%5+1;
    printf("Test 3: %lld (expected 1)\n", (long long)jump_game(bench, 100));

    int iterations = 1000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = jump_game(bench, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Jump Game ===\n");
    printf("Array size: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
