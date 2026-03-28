#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t jump_game_ii(int64_t* arr, int64_t n) {
    if (n <= 1) return 0;
    int64_t jumps = 0, current_end = 0, farthest = 0;
    for (int64_t i = 0; i < n - 1; i++) {
        int64_t reach = i + arr[i];
        if (reach > farthest) farthest = reach;
        if (i == current_end) {
            jumps++;
            current_end = farthest;
            if (current_end >= n - 1) return jumps;
        }
    }
    return jumps;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {2, 3, 1, 1, 4};
    printf("Test 1: %lld (expected 2)\n", jump_game_ii(a1, 5));

    int64_t a2[] = {2, 3, 0, 1, 4};
    printf("Test 2: %lld (expected 2)\n", jump_game_ii(a2, 5));

    int64_t a3[] = {1, 1, 1, 1, 1};
    printf("Test 3: %lld (expected 4)\n", jump_game_ii(a3, 5));

    int64_t n = 100;
    int64_t bench[100];
    for (int64_t i = 0; i < n; i++) {
        bench[i] = (i * 3 + 7) % 10 + 1;
    }
    printf("Bench check: %lld\n", jump_game_ii(bench, n));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = jump_game_ii(bench, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Jump Game II ===\n");
    printf("Array size: %lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
