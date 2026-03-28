#include <stdio.h>
#include <time.h>
#include <sys/resource.h>

/* Sliding window. Characters encoded as ints 1-26.
   Returns min window length, or -1 if not found. */

typedef struct { int start; int len; } WinResult;

WinResult minWindow(int* s, int sn, int* t, int tn) {
    int need[27] = {0}, have[27] = {0};
    for (int i = 0; i < tn; i++) need[t[i]]++;

    int required = 0;
    for (int i = 1; i <= 26; i++) {
        if (need[i] > 0) required++;
    }

    int formed = 0;
    int left = 0;
    WinResult best = {-1, sn + 1};

    for (int right = 0; right < sn; right++) {
        int c = s[right];
        have[c]++;
        if (need[c] > 0 && have[c] == need[c]) formed++;

        while (formed == required) {
            int wlen = right - left + 1;
            if (wlen < best.len) {
                best.start = left;
                best.len = wlen;
            }
            int lc = s[left];
            have[lc]--;
            if (need[lc] > 0 && have[lc] < need[lc]) formed--;
            left++;
        }
    }
    if (best.start == -1) best.len = -1;
    return best;
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    /* "ADOBECODEBANC", "ABC" => "BANC" (len=4) */
    int s1[] = {1,4,15,2,5,3,15,4,5,2,1,14,3};
    int t1[] = {1,2,3};
    WinResult r1 = minWindow(s1, 13, t1, 3);
    printf("Test 1: start=%d len=%d (expected start=9 len=4)\n", r1.start, r1.len);

    /* "a", "a" => "a" */
    int s2[] = {1}; int t2[] = {1};
    WinResult r2 = minWindow(s2, 1, t2, 1);
    printf("Test 2: len=%d (expected 1)\n", r2.len);

    /* Benchmark */
    int s3[] = {1,2,3,4,5,1,2,3,4,5,1,2,3,4,5,1,2,3,4,5};
    int t3[] = {1,3,5};

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int it = 0; it < iterations; it++) {
        WinResult r = minWindow(s3, 20, t3, 3);
        result = r.len;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Minimum Window Substring ===\n");
    printf("String: 20, Target: 3\n");
    printf("Iterations: %d\n", iterations);
    printf("Result len: %d\n", (int)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
