#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

int64_t permutation_count(int64_t n) {
    int64_t arr[20], c[20];
    for (int64_t i = 0; i < n; i++) {
        arr[i] = i;
        c[i] = 0;
    }
    int64_t count = 1;
    int64_t i = 0;
    while (i < n) {
        if (c[i] < i) {
            if (i % 2 == 0) {
                int64_t temp = arr[0]; arr[0] = arr[i]; arr[i] = temp;
            } else {
                int64_t temp = arr[c[i]]; arr[c[i]] = arr[i]; arr[i] = temp;
            }
            count++;
            c[i]++;
            i = 0;
        } else {
            c[i] = 0;
            i++;
        }
    }
    return count;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1 n=3: %lld (expected 6)\n", permutation_count(3));
    printf("Test 2 n=5: %lld (expected 120)\n", permutation_count(5));
    printf("Test 3 n=8: %lld (expected 40320)\n", permutation_count(8));

    int64_t n = 10;
    printf("Bench check n=10: %lld\n", permutation_count(n));

    long mem_before = get_memory_kb();
    int iterations = 5000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = permutation_count(n);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Permutation Count ===\n");
    printf("n=%lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
