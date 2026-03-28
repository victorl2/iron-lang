#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t max_subarray(int64_t* nums, int64_t n) {
    int64_t max_sum = nums[0];
    int64_t cur_sum = nums[0];
    for (int64_t i = 1; i < n; i++) {
        if (cur_sum < 0) {
            cur_sum = nums[i];
        } else {
            cur_sum = cur_sum + nums[i];
        }
        if (cur_sum > max_sum) {
            max_sum = cur_sum;
        }
    }
    return max_sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {-2,1,-3,4,-1,2,1,-5,4};
    printf("Test 1: %lld (expected 6)\n", max_subarray(a1, 9));

    int64_t a2[] = {1};
    printf("Test 2: %lld (expected 1)\n", max_subarray(a2, 1));

    int64_t a3[] = {5,4,-1,7,8};
    printf("Test 3: %lld (expected 23)\n", max_subarray(a3, 5));

    /* Benchmark: 100 elements, nums[i] = ((i*17+11) % 201) - 100 -> range [-100, 100] */
    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = ((i * 17 + 11) % 201) - 100;
    int64_t bench_result = max_subarray(bench, 100);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 10000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = max_subarray(bench, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Maximum Subarray ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
