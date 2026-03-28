#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t min_val(int64_t a, int64_t b) { return a < b ? a : b; }

int64_t max_area(int64_t* height, int64_t n) {
    int64_t left = 0, right = n - 1;
    int64_t best = 0;
    while (left < right) {
        int64_t w = right - left;
        int64_t h = min_val(height[left], height[right]);
        int64_t area = w * h;
        if (area > best) best = area;
        if (height[left] < height[right]) {
            left++;
        } else {
            right--;
        }
    }
    return best;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {1,8,6,2,5,4,8,3,7};
    printf("Test 1: %lld (expected 49)\n", max_area(a1, 9));

    int64_t a2[] = {1,1};
    printf("Test 2: %lld (expected 1)\n", max_area(a2, 2));

    int64_t a3[] = {4,3,2,1,4};
    printf("Test 3: %lld (expected 16)\n", max_area(a3, 5));

    /* Benchmark array: 100 elements, height[i] = (i*7 + 13) % 100 */
    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = (i * 7 + 13) % 100;
    int64_t bench_result = max_area(bench, 100);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = max_area(bench, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Container With Most Water ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
