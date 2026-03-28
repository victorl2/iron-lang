#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t two_sum(int64_t* arr, int64_t n, int64_t target) {
    for (int64_t i = 0; i < n; i++) {
        int64_t vi = arr[i];
        for (int64_t j = i + 1; j < n; j++) {
            if (vi + arr[j] == target) {
                return i * 1000 + j;
            }
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
    int64_t a1[] = {2, 7, 11, 15};
    printf("Test 1: %lld (expected 1)\n", two_sum(a1, 4, 9));

    int64_t a2[] = {3, 2, 4};
    printf("Test 2: %lld (expected 1002)\n", two_sum(a2, 3, 6));

    int64_t a3[] = {1, 5, 3, 7, 2, 8, 4, 6, 9, 10};
    printf("Test 3: %lld (expected 8009)\n", two_sum(a3, 10, 19));

    int64_t bench_arr[200];
    for (int i = 0; i < 200; i++) bench_arr[i] = i;
    printf("Test 4: %lld (expected 198199)\n", two_sum(bench_arr, 200, 397));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = two_sum(bench_arr, 200, 397);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Two Sum ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
