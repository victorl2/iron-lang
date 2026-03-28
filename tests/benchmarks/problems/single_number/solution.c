#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t single_number(int64_t* arr, int64_t n) {
    int64_t result = 0;
    for (int64_t i = 0; i < n; i++) {
        result = result ^ arr[i];
    }
    return result;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {2,2,1};
    printf("Test 1: %lld (expected 1)\n", single_number(a1, 3));

    int64_t a2[] = {4,1,2,1,2};
    printf("Test 2: %lld (expected 4)\n", single_number(a2, 5));

    int64_t a3[] = {1};
    printf("Test 3: %lld (expected 1)\n", single_number(a3, 1));

    /* Benchmark: 201 elements. pairs of 1..100 plus single element 42 */
    int64_t bench[201];
    for (int i = 0; i < 100; i++) {
        bench[i * 2] = i + 1;
        bench[i * 2 + 1] = i + 1;
    }
    bench[200] = 42;
    int64_t bench_result = single_number(bench, 201);
    printf("Test 4: %lld (expected 42)\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = single_number(bench, 201);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Single Number ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
