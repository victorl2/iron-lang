#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

void reverse_range(int64_t* arr, int64_t lo, int64_t hi) {
    while (lo < hi) {
        int64_t tmp = arr[lo];
        arr[lo] = arr[hi];
        arr[hi] = tmp;
        lo++;
        hi--;
    }
}

/* Rotate array right by k positions. Returns checksum: sum of arr[i]*(i+1). */
int64_t rotate_array(int64_t* arr, int64_t n, int64_t k) {
    int64_t kk = k % n;
    if (kk > 0) {
        reverse_range(arr, 0, n - 1);
        reverse_range(arr, 0, kk - 1);
        reverse_range(arr, kk, n - 1);
    }
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        sum += arr[i] * (i + 1);
    }
    return sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {1,2,3,4,5,6,7};
    printf("Test 1: %lld\n", rotate_array(a1, 7, 3));
    /* [5,6,7,1,2,3,4] -> 5+12+21+4+10+18+28 = 98 */

    int64_t a2[] = {10,20,30,40,50};
    printf("Test 2: %lld\n", rotate_array(a2, 5, 2));
    /* [40,50,10,20,30] -> 40+100+30+80+150 = 400 */

    /* Benchmark: 100 elements, arr[i] = i, rotate by 37 */
    int64_t bench_orig[100];
    for (int i = 0; i < 100; i++) bench_orig[i] = i;

    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = bench_orig[i];
    int64_t bench_result = rotate_array(bench, 100, 37);
    printf("Test 3: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        int64_t tmp[100];
        for (int i = 0; i < 100; i++) tmp[i] = bench_orig[i];
        result = rotate_array(tmp, 100, 37);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Rotate Array ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
