#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

void sort_by_end(int64_t* starts, int64_t* ends, int64_t n) {
    for (int64_t i = 1; i < n; i++) {
        int64_t key_s = starts[i];
        int64_t key_e = ends[i];
        int64_t j = i - 1;
        while (j >= 0 && ends[j] > key_e) {
            starts[j + 1] = starts[j];
            ends[j + 1] = ends[j];
            j--;
        }
        starts[j + 1] = key_s;
        ends[j + 1] = key_e;
    }
}

int64_t activity_selection(int64_t* starts, int64_t* ends, int64_t n) {
    sort_by_end(starts, ends, n);
    int64_t count = 1;
    int64_t last_end = ends[0];
    for (int64_t i = 1; i < n; i++) {
        if (starts[i] >= last_end) {
            count++;
            last_end = ends[i];
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
    int64_t s1[] = {1, 3, 0, 5, 8, 5};
    int64_t e1[] = {2, 4, 6, 7, 9, 9};
    printf("Test 1: %lld (expected 4)\n", activity_selection(s1, e1, 6));

    int64_t s2[] = {0, 0, 0, 0};
    int64_t e2[] = {10, 10, 10, 10};
    printf("Test 2: %lld (expected 1)\n", activity_selection(s2, e2, 4));

    int64_t s3[] = {0, 2, 4, 6, 8};
    int64_t e3[] = {1, 3, 5, 7, 9};
    printf("Test 3: %lld (expected 5)\n", activity_selection(s3, e3, 5));

    int64_t n = 50;
    int64_t orig_starts[50], orig_ends[50];
    int64_t starts[50], ends[50];
    for (int64_t i = 0; i < n; i++) {
        orig_starts[i] = (i * 7 + 3) % 100;
        orig_ends[i] = orig_starts[i] + (i % 5) + 1;
    }

    memcpy(starts, orig_starts, sizeof(starts));
    memcpy(ends, orig_ends, sizeof(ends));
    int64_t check = activity_selection(starts, ends, n);
    printf("Bench check: %lld\n", check);

    long mem_before = get_memory_kb();
    int iterations = 200000;

    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        memcpy(starts, orig_starts, sizeof(starts));
        memcpy(ends, orig_ends, sizeof(ends));
        result = activity_selection(starts, ends, n);
    }
    clock_gettime(CLOCK_MONOTONIC, &t_end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                      + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Activity Selection ===\n");
    printf("Intervals: %lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
