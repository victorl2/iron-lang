#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int hamming(int64_t a, int64_t b) {
    int64_t x = a, y = b;
    int count = 0;
    while (x > 0 || y > 0) {
        if ((x % 2) != (y % 2))
            count++;
        x /= 2;
        y /= 2;
    }
    return count;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1: %d (expected 2)\n", hamming(1, 4));
    printf("Test 2: %d (expected 1)\n", hamming(3, 1));
    printf("Test 3: %d (expected 0)\n", hamming(0, 0));

    long mem_before = get_memory_kb();

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t total = 0;
    int64_t sum = 0;
    for (int i = 0; i < 1000000; i++) {
        int64_t a = (i * 31 + 17) % 100000;
        int64_t b = (i * 37 + 23) % 100000;
        sum += hamming(a, b);
    }
    total = sum;
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("Test 4: %lld (expected 8217960)\n", (long long)total);

    printf("\n=== Benchmark: Hamming Distance ===\n");
    printf("Pairs: 1000000\n");
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / 1000000);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
