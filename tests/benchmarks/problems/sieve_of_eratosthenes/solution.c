#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

static int64_t sieve(int n) {
    int is_prime[10001];
    memset(is_prime, 1, sizeof(int) * (n + 1));
    is_prime[0] = 0;
    is_prime[1] = 0;

    for (int i = 2; i * i <= n; i++) {
        if (is_prime[i]) {
            for (int j = i * i; j <= n; j += i)
                is_prime[j] = 0;
        }
    }

    int64_t count = 0;
    for (int k = 0; k <= n; k++)
        count += is_prime[k];
    return count;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1: %lld (expected 4)\n", sieve(10));
    printf("Test 2: %lld (expected 25)\n", sieve(100));
    printf("Test 3: %lld (expected 168)\n", sieve(1000));
    printf("Test 4: %lld (expected 1229)\n", sieve(10000));

    long mem_before = get_memory_kb();
    int iterations = 10000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = sieve(10000);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Sieve of Eratosthenes ===\n");
    printf("n=10000\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
