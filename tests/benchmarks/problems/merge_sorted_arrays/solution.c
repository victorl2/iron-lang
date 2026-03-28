#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

/* Merge two sorted arrays into out. Returns checksum (sum of out). */
int64_t merge_sorted(int64_t* a, int64_t na, int64_t* b, int64_t nb, int64_t* out) {
    int64_t i = 0, j = 0, k = 0;
    while (i < na && j < nb) {
        if (a[i] <= b[j]) {
            out[k] = a[i];
            i++;
        } else {
            out[k] = b[j];
            j++;
        }
        k++;
    }
    while (i < na) {
        out[k] = a[i];
        i++;
        k++;
    }
    while (j < nb) {
        out[k] = b[j];
        j++;
        k++;
    }
    int64_t sum = 0;
    for (int64_t x = 0; x < na + nb; x++) sum += out[x];
    return sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {1,3,5,7};
    int64_t b1[] = {2,4,6,8};
    int64_t o1[8];
    printf("Test 1: %lld (expected 36)\n", merge_sorted(a1, 4, b1, 4, o1));

    int64_t a2[] = {1};
    int64_t b2[] = {2,3,4};
    int64_t o2[4];
    printf("Test 2: %lld (expected 10)\n", merge_sorted(a2, 1, b2, 3, o2));

    int64_t a3[] = {1,2,3};
    int64_t b3[] = {1,2,3};
    int64_t o3[6];
    printf("Test 3: %lld (expected 12)\n", merge_sorted(a3, 3, b3, 3, o3));

    /* Benchmark: two sorted arrays of 50 each */
    int64_t bench_a[50], bench_b[50];
    for (int i = 0; i < 50; i++) {
        bench_a[i] = i * 2;       /* 0,2,4,...,98 */
        bench_b[i] = i * 2 + 1;   /* 1,3,5,...,99 */
    }
    int64_t obench[100];
    int64_t bench_result = merge_sorted(bench_a, 50, bench_b, 50, obench);
    printf("Test 4: %lld (expected 4950)\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = merge_sorted(bench_a, 50, bench_b, 50, obench);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Merge Sorted Arrays ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
