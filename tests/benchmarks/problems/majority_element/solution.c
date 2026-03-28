#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t majority_element(int64_t* arr, int64_t n) {
    int64_t candidate = arr[0];
    int64_t count = 1;
    for (int64_t i = 1; i < n; i++) {
        if (count == 0) {
            candidate = arr[i];
            count = 1;
        } else if (arr[i] == candidate) {
            count++;
        } else {
            count--;
        }
    }
    return candidate;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {3,2,3};
    printf("Test 1: %lld (expected 3)\n", majority_element(a1, 3));

    int64_t a2[] = {2,2,1,1,1,2,2};
    printf("Test 2: %lld (expected 2)\n", majority_element(a2, 7));

    int64_t a3[] = {1};
    printf("Test 3: %lld (expected 1)\n", majority_element(a3, 1));

    /* Benchmark: 200 elements, majority is 42 (appears 120 times) */
    int64_t bench[200];
    for (int i = 0; i < 200; i++) {
        bench[i] = (i < 120) ? 42 : (i % 41);
    }
    int64_t bench_result = majority_element(bench, 200);
    printf("Test 4: %lld (expected 42)\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = majority_element(bench, 200);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Majority Element ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
