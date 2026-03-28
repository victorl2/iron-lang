#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t binary_search_insert(int64_t* arr, int64_t n, int64_t target) {
    int64_t lo = 0, hi = n;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (arr[mid] < target) {
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
    int64_t a1[] = {1, 3, 5, 6};
    printf("Test 1: %lld (expected 2)\n", binary_search_insert(a1, 4, 5));
    printf("Test 2: %lld (expected 1)\n", binary_search_insert(a1, 4, 2));
    printf("Test 3: %lld (expected 4)\n", binary_search_insert(a1, 4, 7));
    printf("Test 4: %lld (expected 0)\n", binary_search_insert(a1, 4, 0));

    int64_t n = 200;
    int64_t arr[200];
    for (int64_t i = 0; i < n; i++) {
        arr[i] = i * 3;
    }
    printf("Bench check: %lld\n", binary_search_insert(arr, n, 299));

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        int64_t target = (it * 7 + 13) % 600;
        result += binary_search_insert(arr, n, target);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("Bench result sum: %lld\n", result);

    printf("\n=== Benchmark: Binary Search Insert ===\n");
    printf("Array size: %lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
