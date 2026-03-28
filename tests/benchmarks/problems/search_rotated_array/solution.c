#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

int search_rotated(int* arr, int n, int target) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (arr[mid] == target) return mid;
        if (arr[lo] <= arr[mid]) {
            if (arr[lo] <= target && target < arr[mid]) hi = mid - 1;
            else lo = mid + 1;
        } else {
            if (arr[mid] < target && target <= arr[hi]) lo = mid + 1;
            else hi = mid - 1;
        }
    }
    return -1;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int arr[100];
    for (int i = 0; i < 100; i++) arr[i] = (i + 50) % 100;

    printf("Test 1: %d (expected 25)\n", search_rotated(arr, 100, 75));
    printf("Test 2: %d (expected 60)\n", search_rotated(arr, 100, 10));
    printf("Test 3: %d (expected -1)\n", search_rotated(arr, 100, 200));
    printf("Test 4: %d (expected 0)\n", search_rotated(arr, 100, 50));

    long mem_before = get_memory_kb();
    int iterations = 10000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = search_rotated(arr, 100, 75);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Search Rotated Array ===\n");
    printf("Array size: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
