#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t subsets_bitmask(int64_t* arr, int64_t n) {
    int64_t total = 1;
    for (int64_t i = 0; i < n; i++) total *= 2;

    int64_t grand_sum = 0;
    for (int64_t mask = 0; mask < total; mask++) {
        int64_t subset_sum = 0;
        for (int64_t i = 0; i < n; i++) {
            if ((mask >> i) & 1) {
                subset_sum += arr[i];
            }
        }
        grand_sum += subset_sum;
    }
    return grand_sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {1, 2, 3};
    printf("Test 1: %lld (expected 24)\n", subsets_bitmask(a1, 3));

    int64_t a2[] = {1};
    printf("Test 2: %lld (expected 1)\n", subsets_bitmask(a2, 1));

    int64_t a3[] = {1, 2, 3, 4, 5};
    printf("Test 3: %lld (expected 240)\n", subsets_bitmask(a3, 5));

    int64_t n = 18;
    int64_t arr[18];
    for (int64_t i = 0; i < n; i++) arr[i] = i + 1;
    printf("Bench check: %lld\n", subsets_bitmask(arr, n));

    long mem_before = get_memory_kb();
    int iterations = 100;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = subsets_bitmask(arr, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Subsets Bitmask ===\n");
    printf("Set size: %lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
