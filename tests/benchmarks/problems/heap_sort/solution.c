#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

void heap_down(int64_t* arr, int64_t size, int64_t idx) {
    int64_t i = idx;
    while (1) {
        int64_t largest = i;
        int64_t l = 2 * i + 1;
        int64_t r = 2 * i + 2;
        if (l < size && arr[l] > arr[largest]) largest = l;
        if (r < size && arr[r] > arr[largest]) largest = r;
        if (largest == i) break;
        int64_t temp = arr[i];
        arr[i] = arr[largest];
        arr[largest] = temp;
        i = largest;
    }
}

void heap_sort(int64_t* arr, int64_t n) {
    for (int64_t i = n / 2 - 1; i >= 0; i--) {
        heap_down(arr, n, i);
    }
    for (int64_t i = n - 1; i > 0; i--) {
        int64_t temp = arr[0];
        arr[0] = arr[i];
        arr[i] = temp;
        heap_down(arr, i, 0);
    }
}

int64_t checksum(int64_t* arr, int64_t n) {
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
    int64_t a1[] = {5, 3, 8, 1, 2, 7, 4, 6};
    heap_sort(a1, 8);
    printf("Test 1: %lld %lld %lld %lld %lld %lld %lld %lld (expected 1 2 3 4 5 6 7 8)\n",
           a1[0], a1[1], a1[2], a1[3], a1[4], a1[5], a1[6], a1[7]);

    int64_t a2[] = {1};
    heap_sort(a2, 1);
    printf("Test 2: %lld (expected 1)\n", a2[0]);

    int64_t a3[] = {3, 1, 2};
    heap_sort(a3, 3);
    printf("Test 3: %lld %lld %lld (expected 1 2 3)\n", a3[0], a3[1], a3[2]);

    int64_t n = 200;
    int64_t orig[200], bench[200];
    for (int64_t i = 0; i < n; i++) {
        orig[i] = (i * 17 + 31) % 1000;
    }
    memcpy(bench, orig, sizeof(bench));
    heap_sort(bench, n);
    printf("Bench check: %lld\n", checksum(bench, n));

    long mem_before = get_memory_kb();
    int iterations = 100000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        memcpy(bench, orig, sizeof(bench));
        heap_sort(bench, n);
        result = bench[0];
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Heap Sort ===\n");
    printf("Array size: %lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
