#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

/* Returns sum of output array as a checksum */
int64_t product_except_self(int64_t* nums, int64_t n, int64_t* out) {
    /* Left pass */
    out[0] = 1;
    for (int64_t i = 1; i < n; i++) {
        out[i] = out[i - 1] * nums[i - 1];
    }
    /* Right pass */
    int64_t right = 1;
    for (int64_t i = n - 2; i >= 0; i--) {
        right = right * nums[i + 1];
        out[i] = out[i] * right;
    }
    /* Checksum: sum of output */
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) sum += out[i];
    return sum;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t a1[] = {1,2,3,4};
    int64_t o1[4];
    int64_t s1 = product_except_self(a1, 4, o1);
    /* [24,12,8,6] -> sum = 50 */
    printf("Test 1: %lld (expected 50)\n", s1);

    int64_t a2[] = {-1,1,0,-3,3};
    int64_t o2[5];
    int64_t s2 = product_except_self(a2, 5, o2);
    /* [0,0,9,0,0] -> sum = 9 */
    printf("Test 2: %lld (expected 9)\n", s2);

    int64_t a3[] = {2,3,4,5};
    int64_t o3[4];
    int64_t s3 = product_except_self(a3, 4, o3);
    /* [60,40,30,24] -> sum = 154 */
    printf("Test 3: %lld (expected 154)\n", s3);

    /* Benchmark: 50 elements, mostly 1s with some 2s to avoid overflow */
    int64_t bench[50];
    for (int i = 0; i < 50; i++) bench[i] = (i % 10 == 0) ? 2 : 1;
    int64_t obench[50];
    int64_t bench_result = product_except_self(bench, 50, obench);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = product_except_self(bench, 50, obench);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Product Except Self ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
