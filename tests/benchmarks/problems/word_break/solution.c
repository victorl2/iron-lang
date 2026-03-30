#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

int64_t word_break(int64_t* s, int64_t sn, int64_t* dict_flat, int64_t* dict_starts,
                   int64_t* dict_lens, int64_t nd) {
    int64_t dp[22];
    memset(dp, 0, sizeof(dp));
    dp[0] = 1;
    for (int64_t i = 1; i <= sn; i++) {
        for (int64_t w = 0; w < nd; w++) {
            int64_t wlen = dict_lens[w];
            if (wlen <= i && dp[i - wlen]) {
                int64_t match = 1;
                for (int64_t k = 0; k < wlen; k++) {
                    if (s[i - wlen + k] != dict_flat[dict_starts[w] + k]) {
                        match = 0;
                        break;
                    }
                }
                if (match) { dp[i] = 1; break; }
            }
        }
    }
    return dp[sn];
}

int main(void) {
    int64_t s1[] = {12,5,5,20,3,15,4,5};
    int64_t df1[] = {12,5,5,20, 3,15,4,5};
    int64_t ds1[] = {0, 4};
    int64_t dl1[] = {4, 4};
    printf("Test 1: %lld (expected 1)\n", (long long)word_break(s1,8,df1,ds1,dl1,2));

    int64_t s2[] = {1,16,16,12,5,16,5,14,1,16,16,12,5};
    int64_t df2[] = {1,16,16,12,5, 16,5,14};
    int64_t ds2[] = {0, 5};
    int64_t dl2[] = {5, 3};
    printf("Test 2: %lld (expected 1)\n", (long long)word_break(s2,13,df2,ds2,dl2,2));

    int64_t bench_s[] = {1,2,3,1,2,3,4,5,1,2,3,4,5,6,1,2,1,2,3,4};
    int64_t bench_flat[] = {1,2, 3, 4,5, 1,2,3, 4,5,6, 1, 2,3,4, 5,6,1, 2, 3,4};
    int64_t bench_starts[] = {0,2,3,5,8,11,12,15,18,19};
    int64_t bench_lens[] = {2,1,2,3,3,1,3,3,1,2};
    printf("Test 3: %lld (expected 1)\n", (long long)word_break(bench_s,20,bench_flat,bench_starts,bench_lens,10));

    int iterations = 5000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = word_break(bench_s,20,bench_flat,bench_starts,bench_lens,10);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Word Break ===\n");
    printf("String length: 20, Dictionary: 10 words\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
