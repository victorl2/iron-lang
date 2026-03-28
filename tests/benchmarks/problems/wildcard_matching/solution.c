#include <stdio.h>
#include <time.h>
#include <sys/resource.h>

/* Greedy approach with backtracking. Chars encoded as int: 'a'-'z' = 1-26, '?' = 100, '*' = 101 */
int isMatch(int* s, int sn, int* p, int pn) {
    int si = 0, pi = 0;
    int starIdx = -1, matchIdx = 0;

    while (si < sn) {
        if (pi < pn && (p[pi] == 100 || p[pi] == s[si])) {
            si++; pi++;
        } else if (pi < pn && p[pi] == 101) {
            starIdx = pi++;
            matchIdx = si;
        } else if (starIdx != -1) {
            pi = starIdx + 1;
            si = ++matchIdx;
        } else {
            return 0;
        }
    }
    while (pi < pn && p[pi] == 101) pi++;
    return pi == pn ? 1 : 0;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    /* "aa", "a" -> 0 */
    int s1[] = {1,1}; int p1[] = {1};
    /* "aa", "*" -> 1 */
    int s2[] = {1,1}; int p2[] = {101};
    /* "cb", "?a" -> 0 */
    int s3[] = {3,2}; int p3[] = {100,1};
    /* "adceb", "*a*b" -> 1 */
    int s4[] = {1,4,3,5,2}; int p4[] = {101,1,101,2};

    printf("Test 1: %d (expected 0)\n", isMatch(s1,2,p1,1));
    printf("Test 2: %d (expected 1)\n", isMatch(s2,2,p2,1));
    printf("Test 3: %d (expected 0)\n", isMatch(s3,2,p3,2));
    printf("Test 4: %d (expected 1)\n", isMatch(s4,5,p4,4));

    /* Worst case: long string, pattern "*a*b*c*" */
    int s5[] = {1,2,3,1,2,3,1,2,3,1,2,3,1,2,3,1,2,3,1,2};
    int p5[] = {101,1,101,2,101,3,101};

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        result = isMatch(s5, 20, p5, 7);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Wildcard Matching ===\n");
    printf("String: 20 chars, Pattern: 7 chars\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
