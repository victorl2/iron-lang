#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

/* Move zeroes to end, return checksum: sum of arr[i] * (i+1) */
int64_t move_zeroes(int64_t* arr, int64_t n) {
    int64_t write = 0;
    for (int64_t i = 0; i < n; i++) {
        if (arr[i] != 0) {
            arr[write] = arr[i];
            write++;
        }
    }
    for (int64_t i = write; i < n; i++) {
        arr[i] = 0;
    }
    /* Checksum */
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        sum = sum + arr[i] * (i + 1);
    }
    return sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {0,1,0,3,12};
    printf("Test 1: %lld\n", move_zeroes(a1, 5));
    /* [1,3,12,0,0] -> 1*1 + 3*2 + 12*3 + 0 + 0 = 43 */

    int64_t a2[] = {0};
    printf("Test 2: %lld (expected 0)\n", move_zeroes(a2, 1));

    int64_t a3[] = {1,0,2,0,3,0,4};
    printf("Test 3: %lld\n", move_zeroes(a3, 7));
    /* [1,2,3,4,0,0,0] -> 1+4+9+16 = 30 */

    /* Benchmark: 100 elements, arr[i] = i%3==0 ? 0 : i */
    int64_t bench_orig[100];
    for (int i = 0; i < 100; i++) {
        bench_orig[i] = (i % 3 == 0) ? 0 : i;
    }

    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = bench_orig[i];
    int64_t bench_result = move_zeroes(bench, 100);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        int64_t tmp[100];
        for (int i = 0; i < 100; i++) tmp[i] = bench_orig[i];
        result = move_zeroes(tmp, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Move Zeroes ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
