#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t sliding_max(int64_t* arr, int n, int k) {
    int dq[200];
    int front = 0, back = 0;
    int64_t total = 0;

    for (int i = 0; i < n; i++) {
        while (front < back && dq[front] <= i - k)
            front++;
        while (front < back && arr[dq[back - 1]] <= arr[i])
            back--;
        dq[back++] = i;
        if (i >= k - 1)
            total += arr[dq[front]];
    }
    return total;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {1, 3, -1, -3, 5, 3, 6, 7};
    printf("Test 1: %lld (expected 29)\n", sliding_max(t1, 8, 3));

    int64_t t2[] = {1};
    printf("Test 2: %lld (expected 1)\n", sliding_max(t2, 1, 1));

    int64_t t3[] = {9, 8, 7, 6, 5, 4, 3, 2, 1};
    printf("Test 3: %lld (expected 42)\n", sliding_max(t3, 9, 3));

    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = (i * 17 + 23) % 100 + 1;
    printf("Test 4: %lld (expected 8515)\n", sliding_max(bench, 100, 10));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = sliding_max(bench, 100, 10);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Sliding Window Maximum ===\n");
    printf("Array size: 100, Window: 10\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
