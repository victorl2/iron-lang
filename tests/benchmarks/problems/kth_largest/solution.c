#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t qs_partition(int64_t* arr, int64_t lo, int64_t hi) {
    int64_t pivot = arr[hi];
    int64_t i = lo - 1;
    for (int64_t j = lo; j < hi; j++) {
        if (arr[j] <= pivot) {
            i++;
            int64_t tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
        }
    }
    int64_t tmp = arr[i + 1];
    arr[i + 1] = arr[hi];
    arr[hi] = tmp;
    return i + 1;
}

int64_t quickselect(int64_t* arr, int64_t lo, int64_t hi, int64_t k) {
    if (lo == hi) return arr[lo];
    int64_t p = qs_partition(arr, lo, hi);
    if (k == p) return arr[p];
    if (k < p) return quickselect(arr, lo, p - 1, k);
    return quickselect(arr, p + 1, hi, k);
}

int64_t kth_largest(int64_t* orig, int64_t n, int64_t k) {
    int64_t arr[200];
    for (int64_t i = 0; i < n; i++) arr[i] = orig[i];
    /* kth largest = (n-k)th smallest (0-indexed) */
    return quickselect(arr, 0, n - 1, n - k);
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {3,2,1,5,6,4};
    printf("Test 1: %lld (expected 5)\n", kth_largest(a1, 6, 2));

    int64_t a2[] = {3,2,3,1,2,4,5,5,6};
    printf("Test 2: %lld (expected 4)\n", kth_largest(a2, 9, 4));

    int64_t a3[] = {1};
    printf("Test 3: %lld (expected 1)\n", kth_largest(a3, 1, 1));

    /* Benchmark: 200 elements, arr[i] = (i*37+13) % 1000, k=50 */
    int64_t bench[200];
    for (int i = 0; i < 200; i++) bench[i] = (i * 37 + 13) % 1000;
    int64_t bench_result = kth_largest(bench, 200, 50);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 300000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = kth_largest(bench, 200, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Kth Largest ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
