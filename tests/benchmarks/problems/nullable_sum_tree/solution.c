#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

#define NULL_VAL INT64_MIN
#define IS_NULL(x) ((x) == NULL_VAL)

int64_t find_index(int64_t *arr, int64_t n, int64_t target) {
    for (int64_t i = 0; i < n; i++) {
        if (arr[i] == target) return i;
    }
    return NULL_VAL;
}

int64_t safe_divide(int64_t a, int64_t b) {
    if (b == 0) return NULL_VAL;
    return a / b;
}

int64_t nullable_chain(int64_t x) {
    int64_t result = NULL_VAL;
    if (x % 2 == 0) {
        result = x * 3;
    }
    if (!IS_NULL(result)) {
        int64_t v = result + 10;
        int64_t div = safe_divide(v, x % 7 + 1);
        if (!IS_NULL(div)) {
            return div;
        }
        return v;
    }
    return 0;
}

int64_t bench_nullable(int64_t n, int64_t *arr, int64_t arr_len) {
    int64_t checksum = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t idx = find_index(arr, arr_len, i % (arr_len + 5));
        if (!IS_NULL(idx)) {
            checksum += idx;
        } else {
            checksum += 999;
        }

        int64_t d = safe_divide(i * 17 + 3, i % 5);
        if (!IS_NULL(d)) {
            checksum += d % 1000;
        }

        checksum += nullable_chain(i);
    }
    return checksum;
}

int main(void) {
    int64_t arr[] = {10, 20, 30, 40, 50};
    int64_t found = find_index(arr, 5, 30);
    if (!IS_NULL(found)) {
        printf("Test 1 found: %lld (expected 2)\n", (long long)found);
    }
    int64_t not_found = find_index(arr, 5, 99);
    if (IS_NULL(not_found)) {
        printf("Test 2 not found: null (expected null)\n");
    }

    int64_t div1 = safe_divide(100, 3);
    if (!IS_NULL(div1)) {
        printf("Test 3 divide: %lld (expected 33)\n", (long long)div1);
    }
    int64_t div0 = safe_divide(100, 0);
    if (IS_NULL(div0)) {
        printf("Test 4 div-by-zero: null (expected null)\n");
    }

    printf("Test 5 chain(4): %lld (expected 4)\n", (long long)nullable_chain(4));
    printf("Test 6 chain(3): %lld (expected 0)\n", (long long)nullable_chain(3));

    int64_t bench_arr[50];
    for (int i = 0; i < 50; i++) bench_arr[i] = i;
    int64_t check = bench_nullable(200, bench_arr, 50);
    printf("Test 7 bench(200): %lld\n", (long long)check);

    int iterations = 500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = bench_nullable(200, bench_arr, 50);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Nullable Types ===\n");
    printf("Elements: 200\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
