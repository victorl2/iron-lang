#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t remove_duplicates(int64_t* arr, int64_t n) {
    if (n == 0) return 0;
    int64_t write = 1;
    for (int64_t i = 1; i < n; i++) {
        if (arr[i] != arr[i - 1]) {
            arr[write] = arr[i];
            write++;
        }
    }
    return write;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {1,1,2};
    printf("Test 1: %lld (expected 2)\n", remove_duplicates(a1, 3));

    int64_t a2[] = {0,0,1,1,1,2,2,3,3,4};
    printf("Test 2: %lld (expected 5)\n", remove_duplicates(a2, 10));

    int64_t a3[] = {1,2,3,4,5};
    printf("Test 3: %lld (expected 5)\n", remove_duplicates(a3, 5));

    /* Benchmark: sorted array of 100 elements with duplicates */
    /* arr[i] = i / 2, so 0,0,1,1,2,2,...,49,49 */
    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = i / 2;
    /* Need a copy since we modify in place */
    int64_t bench_copy[100];
    for (int i = 0; i < 100; i++) bench_copy[i] = bench[i];
    int64_t bench_result = remove_duplicates(bench_copy, 100);
    printf("Test 4: %lld (expected 50)\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        int64_t tmp[100];
        for (int i = 0; i < 100; i++) tmp[i] = bench[i];
        result = remove_duplicates(tmp, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Remove Duplicates Sorted ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
