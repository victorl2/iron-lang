#include <stdio.h>
#include <stdint.h>
#include <time.h>

/* C doesn't have defer, so we manually place cleanup at each return point */

int64_t process_with_defer(int64_t *arr, int64_t idx, int64_t depth) {
    arr[0]++;
    if (depth <= 0) {
        int64_t ret = idx % 997;
        arr[0]--;
        return ret;
    }

    arr[1]++;
    int64_t v = (idx * 31 + 17) % 10007;
    if (v % 3 == 0) {
        arr[1]--;
        arr[0]--;
        return v;
    }

    arr[2]++;
    int64_t sub = process_with_defer(arr, v, depth - 1) % 1000;
    int64_t ret = v + sub;
    arr[2]--;
    arr[1]--;
    arr[0]--;
    return ret;
}

int64_t multi_exit(int64_t n, int64_t *tracker) {
    tracker[0]++;
    if (n < 0) {
        tracker[0]--;
        return 0;
    }

    tracker[1]++;
    if (n == 0) {
        tracker[1]--;
        tracker[0]--;
        return 1;
    }

    if (n % 2 == 0) {
        tracker[1]--;
        tracker[0]--;
        return n * 2;
    }

    tracker[2]++;
    int64_t ret = n * 3 + 1;
    tracker[2]--;
    tracker[1]--;
    tracker[0]--;
    return ret;
}

int64_t bench_defer(int64_t n) {
    int64_t tracker[10] = {0};
    int64_t checksum = 0;

    for (int64_t i = 0; i < n; i++) {
        int64_t v = process_with_defer(tracker, i * 73 + 29, 3);
        checksum += v % 10000;
    }

    int64_t tracker_sum = tracker[0] + tracker[1] + tracker[2];

    for (int64_t i2 = 0; i2 < n; i2++) {
        checksum += multi_exit(i2, tracker);
    }

    return checksum + tracker_sum;
}

int main(void) {
    int64_t tracker[10] = {0};
    int64_t v1 = process_with_defer(tracker, 100, 0);
    printf("Test 1 process(100,0): %lld (expected 100)\n", (long long)v1);
    printf("Test 2 tracker[0] after: %lld (expected 0)\n", (long long)tracker[0]);

    printf("Test 3 multi_exit(5): %lld (expected 16)\n", (long long)multi_exit(5, tracker));
    printf("Test 4 multi_exit(4): %lld (expected 8)\n", (long long)multi_exit(4, tracker));
    printf("Test 5 multi_exit(0): %lld (expected 1)\n", (long long)multi_exit(0, tracker));

    int64_t check = bench_defer(100);
    printf("Test 6 bench_defer(100): %lld\n", (long long)check);

    int iterations = 200000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = bench_defer(100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Defer Cleanup Chain ===\n");
    printf("Array size: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
