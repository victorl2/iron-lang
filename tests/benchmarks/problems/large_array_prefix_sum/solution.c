#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

void build_prefix_sum(int64_t *arr, int64_t n, int64_t *prefix) {
    prefix[0] = arr[0];
    for (int64_t i = 1; i < n; i++) {
        prefix[i] = prefix[i - 1] + arr[i];
    }
}

int64_t range_sum(int64_t *prefix, int64_t l, int64_t r) {
    if (l == 0) return prefix[r];
    return prefix[r] - prefix[l - 1];
}

void bulk_transform(int64_t *arr, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        arr[i] = (arr[i] * 31 + 17) % 10007;
    }
}

int64_t checksum_array(int64_t *arr, int64_t n) {
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        sum += arr[i] * ((i % 100) + 1);
    }
    return sum % 1000000007;
}

int64_t bench_large_array(int64_t n) {
    int64_t *arr = (int64_t *)calloc(n, sizeof(int64_t));
    for (int64_t i = 0; i < n; i++) {
        arr[i] = (i * 73 + 29) % 10007;
    }

    int64_t *prefix = (int64_t *)calloc(n, sizeof(int64_t));
    build_prefix_sum(arr, n, prefix);

    int64_t query_sum = 0;
    for (int64_t q = 0; q < 100; q++) {
        int64_t l = (q * 37) % (n / 2);
        int64_t r = l + (q * 13 + 50) % (n / 2);
        query_sum += range_sum(prefix, l, r) % 100000;
    }

    bulk_transform(arr, n);
    int64_t cs = checksum_array(arr, n);

    free(arr);
    free(prefix);
    return query_sum + cs;
}

int main(void) {
    int64_t small[] = {1, 2, 3, 4, 5};
    int64_t sp[5] = {0};
    build_prefix_sum(small, 5, sp);
    printf("Test 1 prefix[4]: %lld (expected 15)\n", (long long)sp[4]);
    printf("Test 2 range(1,3): %lld (expected 9)\n", (long long)range_sum(sp, 1, 3));
    printf("Test 3 range(0,0): %lld (expected 1)\n", (long long)range_sum(sp, 0, 0));

    int64_t check = bench_large_array(10000);
    printf("Test 4 bench(10000): %lld\n", (long long)check);

    int iterations = 10000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = bench_large_array(10000);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Large Array Prefix Sum ===\n");
    printf("Array size: 10000\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
