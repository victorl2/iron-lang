#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t counting_sort_checksum(int64_t* orig, int64_t n, int64_t max_val) {
    int64_t arr[200];
    for (int64_t i = 0; i < n; i++) arr[i] = orig[i];

    int64_t count[100] = {0};
    for (int64_t i = 0; i < n; i++) {
        count[arr[i]]++;
    }

    int64_t idx = 0;
    for (int64_t v = 0; v < max_val; v++) {
        for (int64_t c = 0; c < count[v]; c++) {
            arr[idx] = v;
            idx++;
        }
    }

    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) sum += arr[i] * (i + 1);
    return sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {4,2,2,8,3,3,1};
    printf("Test 1: %lld\n", counting_sort_checksum(a1, 7, 10));

    int64_t a2[] = {0,0,1,1,2,2};
    printf("Test 2: %lld\n", counting_sort_checksum(a2, 6, 3));
    /* sorted same: 0*1+0*2+1*3+1*4+2*5+2*6 = 29 */

    /* Benchmark: 200 elements, arr[i] = (i*31+7) % 100 */
    int64_t bench[200];
    for (int i = 0; i < 200; i++) bench[i] = (i * 31 + 7) % 100;
    int64_t bench_result = counting_sort_checksum(bench, 200, 100);
    printf("Test 3: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 350000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = counting_sort_checksum(bench, 200, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Counting Sort ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
