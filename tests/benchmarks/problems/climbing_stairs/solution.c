#include <stdio.h>
#include <stdint.h>
#include <time.h>

int64_t climb_stairs(int64_t n) {
    if (n <= 2) return n;
    int64_t prev2 = 1, prev1 = 2;
    for (int64_t i = 3; i <= n; i++) {
        int64_t curr = prev1 + prev2;
        prev2 = prev1;
        prev1 = curr;
    }
    return prev1;
}

int main(void) {
    printf("Test 1: %lld (expected 8)\n", (long long)climb_stairs(5));
    printf("Test 2: %lld (expected 89)\n", (long long)climb_stairs(10));
    printf("Test 3: %lld (expected 14930352)\n", (long long)climb_stairs(35));

    int iterations = 10000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = climb_stairs(35);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Climbing Stairs ===\n");
    printf("n=35\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
