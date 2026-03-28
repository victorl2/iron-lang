#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

/* Dutch National Flag: sort array of 0s, 1s, 2s in-place.
   Returns checksum: sum of first 10 elements * 1000 + count of sorted checks. */
int64_t sort_colors(int64_t* arr, int64_t n) {
    int64_t lo = 0, mid = 0, hi = n - 1;
    while (mid <= hi) {
        if (arr[mid] == 0) {
            int64_t tmp = arr[lo];
            arr[lo] = arr[mid];
            arr[mid] = tmp;
            lo++;
            mid++;
        } else if (arr[mid] == 1) {
            mid++;
        } else {
            int64_t tmp = arr[hi];
            arr[hi] = arr[mid];
            arr[mid] = tmp;
            hi--;
        }
    }
    /* Checksum: count of 0s * 10000 + count of 1s * 100 + count of 2s */
    int64_t c0 = 0, c1 = 0, c2 = 0;
    for (int64_t i = 0; i < n; i++) {
        if (arr[i] == 0) c0++;
        else if (arr[i] == 1) c1++;
        else c2++;
    }
    return c0 * 10000 + c1 * 100 + c2;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {2,0,2,1,1,0};
    printf("Test 1: %lld (expected 20202)\n", sort_colors(a1, 6));

    int64_t a2[] = {2,0,1};
    printf("Test 2: %lld (expected 10101)\n", sort_colors(a2, 3));

    int64_t a3[] = {0,0,0,1,1,2,2,2};
    printf("Test 3: %lld (expected 30203)\n", sort_colors(a3, 8));

    /* Benchmark: 100 elements, arr[i] = (i*7+3)%3 */
    int64_t bench_orig[100];
    for (int i = 0; i < 100; i++) bench_orig[i] = (i * 7 + 3) % 3;

    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = bench_orig[i];
    int64_t bench_result = sort_colors(bench, 100);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        int64_t tmp[100];
        for (int i = 0; i < 100; i++) tmp[i] = bench_orig[i];
        result = sort_colors(tmp, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Sort Colors ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
