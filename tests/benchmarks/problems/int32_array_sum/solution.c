#include <stdio.h>
#include <stdint.h>
#include <time.h>

static int32_t array_sum(const int32_t *arr, int n) {
    int32_t total = 0;
    for (int i = 0; i < n; i++) {
        total += arr[i];
    }
    return total;
}

int main(void) {
    const int n = 200;
    int32_t arr[200];
    for (int i = 0; i < n; i++) {
        arr[i] = (int32_t)(i % 100);
    }

    printf("Test 1: %d (expected 9900)\n", array_sum(arr, n));
    printf("Test 2: %d (expected 1225)\n", array_sum(arr, 50));

    int32_t single[1] = {42};
    printf("Test 3: %d (expected 42)\n", array_sum(single, 1));
    printf("Test 4: %d (expected 0)\n", array_sum(arr, 0));

    const int iterations = 10000000;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    int32_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = array_sum(arr, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    long elapsed = (ts_end.tv_sec - ts_start.tv_sec) * 1000 +
                   (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;

    printf("\n");
    printf("=== Benchmark: Int32 Array Sum ===\n");
    printf("Array size: %d\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %ld ms\n", elapsed);
    printf("Result: %d\n", result);
    return 0;
}
