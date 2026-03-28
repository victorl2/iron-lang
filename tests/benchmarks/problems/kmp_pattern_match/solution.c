#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/resource.h>

static int64_t kmp_search(int64_t* text, int tlen, int64_t* pattern, int plen) {
    if (plen == 0) return 0;

    int fail[300] = {0};
    int k = 0;
    for (int i = 1; i < plen; i++) {
        while (k > 0 && pattern[k] != pattern[i])
            k = fail[k - 1];
        if (pattern[k] == pattern[i])
            k++;
        fail[i] = k;
    }

    int64_t count = 0;
    int q = 0;
    for (int j = 0; j < tlen; j++) {
        while (q > 0 && pattern[q] != text[j])
            q = fail[q - 1];
        if (pattern[q] == text[j])
            q++;
        if (q == plen) {
            count++;
            q = fail[q - 1];
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
    int64_t t1[] = {1, 2, 3, 1, 2, 3, 1, 2, 3};
    int64_t p1[] = {1, 2, 3};
    printf("Test 1: %lld (expected 3)\n", kmp_search(t1, 9, p1, 3));

    int64_t t2[] = {1, 1, 1, 1, 1};
    int64_t p2[] = {1, 1};
    printf("Test 2: %lld (expected 4)\n", kmp_search(t2, 5, p2, 2));

    int64_t t3[] = {1, 2, 3, 4, 5};
    int64_t p3[] = {6, 7};
    printf("Test 3: %lld (expected 0)\n", kmp_search(t3, 5, p3, 2));

    int64_t text[200];
    for (int i = 0; i < 200; i++) text[i] = (i * 7 + 3) % 26 + 1;
    int64_t pat[] = {16, 23, 4, 11, 18, 25, 6, 13, 20, 1};
    printf("Test 4: %lld (expected 7)\n", kmp_search(text, 200, pat, 10));

    long mem_before = get_memory_kb();
    int iterations = 500000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = kmp_search(text, 200, pat, 10);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: KMP Pattern Match ===\n");
    printf("Text size: 200, Pattern size: 10\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
