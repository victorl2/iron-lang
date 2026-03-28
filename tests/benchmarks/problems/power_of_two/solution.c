#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t is_power_of_two(int64_t n) {
    if (n <= 0) return 0;
    int64_t x = n;
    while (x > 1) {
        if (x % 2 != 0) return 0;
        x /= 2;
    }
    return 1;
}

static int64_t count_powers_of_two(int64_t limit) {
    int64_t count = 0;
    for (int64_t n = 1; n <= limit; n++) {
        if (is_power_of_two(n))
            count++;
    }
    return count;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1: %lld (expected 1)\n", is_power_of_two(1));
    printf("Test 2: %lld (expected 1)\n", is_power_of_two(16));
    printf("Test 3: %lld (expected 0)\n", is_power_of_two(3));
    printf("Test 4: %lld (expected 0)\n", is_power_of_two(0));
    printf("Test 5: %lld (expected 20)\n", count_powers_of_two(1000000));

    long mem_before = get_memory_kb();
    int iterations = 1;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    result = count_powers_of_two(1000000);
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Power of Two ===\n");
    printf("Range: 1 to 1000000\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
