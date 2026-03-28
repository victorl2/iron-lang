#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t longest_no_repeat(int64_t* arr, int n) {
    int freq[27] = {0};
    int left = 0;
    int64_t best = 0;

    for (int right = 0; right < n; right++) {
        int c = (int)arr[right];
        freq[c]++;

        while (freq[c] > 1) {
            int lc = (int)arr[left];
            freq[lc]--;
            left++;
        }

        int window = right - left + 1;
        if (window > best) best = window;
    }
    return best;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t t1[] = {1, 2, 3, 1, 2, 3, 2, 2};
    printf("Test 1: %lld (expected 3)\n", longest_no_repeat(t1, 8));

    int64_t t2[] = {2, 2, 2, 2, 2};
    printf("Test 2: %lld (expected 1)\n", longest_no_repeat(t2, 5));

    int64_t t3[] = {16, 23, 23, 11, 5, 23};
    printf("Test 3: %lld (expected 3)\n", longest_no_repeat(t3, 6));

    int64_t bench[100];
    for (int i = 0; i < 100; i++) {
        bench[i] = (i * 7 + 3) % 26 + 1;
    }
    int64_t bench_result = longest_no_repeat(bench, 100);
    printf("Test 4: %lld\n", bench_result);

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = longest_no_repeat(bench, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Longest Substring No Repeat ===\n");
    printf("Array size: 100\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
