#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/resource.h>

/* Two-pass O(1) space approach. '(' = 1, ')' = 0 */
int longestValidParentheses(int* s, int n) {
    int left = 0, right = 0, maxLen = 0;

    for (int i = 0; i < n; i++) {
        if (s[i] == 1) left++; else right++;
        if (left == right) {
            if (left + right > maxLen) maxLen = left + right;
        }
        if (right > left) { left = 0; right = 0; }
    }

    left = 0; right = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (s[i] == 1) left++; else right++;
        if (left == right) {
            if (left + right > maxLen) maxLen = left + right;
        }
        if (left > right) { left = 0; right = 0; }
    }
    return maxLen;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int t1[] = {1, 0};
    int t2[] = {1, 0, 0, 1};
    printf("Test 1: %d (expected 2)\n", longestValidParentheses(t1, 2));
    printf("Test 2: %d (expected 2)\n", longestValidParentheses(t2, 4));

    int s[] = {1,1,1,1,1,1,1,1,1,1, 0,0,0,0,0,0,0,0,0,0};
    printf("Test 3: %d (expected 20)\n", longestValidParentheses(s, 20));

    long mem_before = get_memory_kb();
    int iterations = 10000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = longestValidParentheses(s, 20);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Longest Valid Parentheses ===\n");
    printf("String length: 20\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
