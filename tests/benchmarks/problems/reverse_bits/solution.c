#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t reverse_bits(int64_t n) {
    int64_t result = 0;
    int64_t x = n;
    for (int i = 0; i < 32; i++) {
        result = result * 2 + (x % 2);
        x /= 2;
    }
    return result;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1: %lld (expected 964176192)\n", reverse_bits(43261596));
    printf("Test 2: %lld (expected 2147483648)\n", reverse_bits(1));
    printf("Test 3: %lld (expected 0)\n", reverse_bits(0));

    long mem_before = get_memory_kb();

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t total = 0;
    int64_t sum = 0;
    for (int i = 0; i < 1000000; i++) {
        sum += reverse_bits(i);
    }
    total = sum;
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("Test 4: %lld (expected 2147474564972544)\n", (long long)total);

    printf("\n=== Benchmark: Reverse Bits ===\n");
    printf("Values: 1000000\n");
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / 1000000);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
