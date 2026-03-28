#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t find_peak(const int64_t *arr, int64_t n) {
    int64_t lo = 0, hi = n - 1;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (arr[mid] < arr[mid + 1]) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t arr1[100];
    for (int i = 0; i < 100; i++) {
        arr1[i] = i < 70 ? i : 139 - i;
    }
    printf("Test 1: %lld (expected 69)\n", find_peak(arr1, 100));

    int64_t arr2[] = {1, 3, 5, 7, 6, 4, 2};
    printf("Test 2: %lld (expected 3)\n", find_peak(arr2, 7));

    int64_t arr3[] = {1, 2, 3, 4, 5};
    printf("Test 3: %lld (expected 4)\n", find_peak(arr3, 5));

    int64_t arr4[] = {5, 4, 3, 2, 1};
    printf("Test 4: %lld (expected 0)\n", find_peak(arr4, 5));

    int iterations = 1000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int i = 0; i < iterations; i++) {
        result = find_peak(arr1, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Find Peak Element ===\n");
    printf("Array size: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
