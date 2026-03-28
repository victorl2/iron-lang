#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <sys/resource.h>

bool isMatch(const char* s, const char* p) {
    int slen = (int)strlen(s);
    int plen = (int)strlen(p);

    /* dp[i][j] = s[0..i-1] matches p[0..j-1] */
    bool dp[201][201] = {false};
    dp[0][0] = true;

    for (int j = 1; j <= plen; j++) {
        if (p[j - 1] == '*') {
            dp[0][j] = dp[0][j - 2];
        }
    }

    for (int i = 1; i <= slen; i++) {
        for (int j = 1; j <= plen; j++) {
            if (p[j - 1] == '*') {
                dp[i][j] = dp[i][j - 2]; /* zero occurrences */
                if (p[j - 2] == '.' || p[j - 2] == s[i - 1]) {
                    dp[i][j] = dp[i][j] || dp[i - 1][j]; /* one+ occurrences */
                }
            } else if (p[j - 1] == '.' || p[j - 1] == s[i - 1]) {
                dp[i][j] = dp[i - 1][j - 1];
            }
        }
    }
    return dp[slen][plen];
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    printf("Test 1: %d (expected 0)\n", isMatch("aa", "a"));
    printf("Test 2: %d (expected 1)\n", isMatch("aa", "a*"));
    printf("Test 3: %d (expected 1)\n", isMatch("ab", ".*"));
    printf("Test 4: %d (expected 1)\n", isMatch("aab", "c*a*b"));
    printf("Test 5: %d (expected 0)\n", isMatch("mississippi", "mis*is*p*."));

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int result = 0;
    for (int i = 0; i < iterations; i++) {
        result += isMatch("aaaaaaaaaaab", "a*a*a*a*a*a*a*a*a*a*b");
        result += isMatch("mississippi", "mis*is*s*ip*i.");
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Regular Expression Matching ===\n");
    printf("Patterns: worst-case backtracking scenarios\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / (iterations * 2));
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
