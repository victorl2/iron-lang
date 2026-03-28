#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t rle_encode(int64_t* arr, int n) {
    if (n == 0) return 0;
    int64_t checksum = 0;
    int i = 0;
    while (i < n) {
        int64_t v = arr[i];
        int count = 1;
        while (i + count < n && arr[i + count] == v) count++;
        checksum += v * 100 + count;
        i += count;
    }
    return checksum;
}

static int64_t rle_decode_len(int64_t* arr, int n) {
    int64_t total = 0;
    for (int i = 0; i < n; i += 2) {
        total += arr[i + 1];
    }
    return total;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {1, 1, 1, 2, 2, 3};
    printf("Test 1: %lld (expected 606)\n", rle_encode(t1, 6));

    int64_t t2[] = {5, 5, 5, 5, 5};
    printf("Test 2: %lld (expected 505)\n", rle_encode(t2, 5));

    int64_t t3[] = {1, 2, 3, 4, 5};
    printf("Test 3: %lld (expected 1505)\n", rle_encode(t3, 5));

    int64_t d1[] = {1, 3, 2, 2, 3, 1};
    printf("Test 4: %lld (expected 6)\n", rle_decode_len(d1, 6));

    int64_t bench[100];
    for (int i = 0; i < 100; i++) bench[i] = (i / 4) % 26 + 1;
    printf("Test 5: %lld (expected 32600)\n", rle_encode(bench, 100));

    long mem_before = get_memory_kb();
    int iterations = 5000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = rle_encode(bench, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Run Length Encoding ===\n");
    printf("Array size: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
