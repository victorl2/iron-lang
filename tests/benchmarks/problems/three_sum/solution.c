#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

void insertion_sort(int64_t* arr, int64_t n) {
    for (int64_t i = 1; i < n; i++) {
        int64_t key = arr[i];
        int64_t j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

int64_t skip_dup_lo(int64_t* arr, int64_t lo, int64_t hi, int64_t lv) {
    while (lo < hi && arr[lo] == lv) lo++;
    return lo;
}

int64_t skip_dup_hi(int64_t* arr, int64_t lo, int64_t hi, int64_t hv) {
    while (hi > lo && arr[hi] == hv) hi--;
    return hi;
}

int64_t three_sum_count(int64_t* orig, int64_t n) {
    int64_t arr[200];
    for (int64_t i = 0; i < n; i++) arr[i] = orig[i];
    insertion_sort(arr, n);

    int64_t count = 0;
    for (int64_t i = 0; i < n - 2; i++) {
        if (i > 0 && arr[i] == arr[i - 1]) continue;
        int64_t lo = i + 1;
        int64_t hi = n - 1;
        while (lo < hi) {
            int64_t s = arr[i] + arr[lo] + arr[hi];
            if (s == 0) {
                count++;
                int64_t lv = arr[lo];
                int64_t hv = arr[hi];
                lo++;
                hi--;
                lo = skip_dup_lo(arr, lo, hi, lv);
                hi = skip_dup_hi(arr, lo, hi, hv);
            } else if (s < 0) {
                lo++;
            } else {
                hi--;
            }
        }
    }
    return count;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {-1, 0, 1, 2, -1, -4};
    printf("Test 1: %lld (expected 2)\n", three_sum_count(a1, 6));

    int64_t a2[] = {0, 0, 0, 0};
    printf("Test 2: %lld (expected 1)\n", three_sum_count(a2, 4));

    int64_t a3[] = {-2, 0, 1, 1, 2};
    printf("Test 3: %lld (expected 2)\n", three_sum_count(a3, 5));

    int64_t bench[50];
    for (int i = 0; i < 50; i++) bench[i] = i - 25;
    int64_t bench_result = three_sum_count(bench, 50);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 100000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = three_sum_count(bench, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Three Sum ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
