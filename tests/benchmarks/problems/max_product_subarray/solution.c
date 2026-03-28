#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t max_product(int64_t* nums, int64_t n) {
    int64_t max_so_far = nums[0];
    int64_t cur_max = nums[0];
    int64_t cur_min = nums[0];
    for (int64_t i = 1; i < n; i++) {
        int64_t a = cur_max * nums[i];
        int64_t b = cur_min * nums[i];
        int64_t c = nums[i];
        /* cur_max = max(a, b, c) */
        cur_max = a;
        if (b > cur_max) cur_max = b;
        if (c > cur_max) cur_max = c;
        /* cur_min = min(a, b, c) */
        cur_min = a;
        if (b < cur_min) cur_min = b;
        if (c < cur_min) cur_min = c;

        if (cur_max > max_so_far) max_so_far = cur_max;
    }
    return max_so_far;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {2,3,-2,4};
    printf("Test 1: %lld (expected 6)\n", max_product(a1, 4));

    int64_t a2[] = {-2,0,-1};
    printf("Test 2: %lld (expected 0)\n", max_product(a2, 3));

    int64_t a3[] = {-2,3,-4};
    printf("Test 3: %lld (expected 24)\n", max_product(a3, 3));

    /* Benchmark: 50 elements, nums[i] = ((i*13+7) % 11) - 5 -> range [-5, 5] */
    int64_t bench[50];
    for (int i = 0; i < 50; i++) bench[i] = ((i * 13 + 7) % 11) - 5;
    int64_t bench_result = max_product(bench, 50);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 10000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = max_product(bench, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Max Product Subarray ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
