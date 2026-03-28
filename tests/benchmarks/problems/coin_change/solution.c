#include <stdio.h>
#include <stdint.h>
#include <time.h>

int64_t coin_change(int64_t* coins, int64_t nc, int64_t amount) {
    int64_t dp[101];
    for (int i = 0; i <= amount; i++) dp[i] = 999999;
    dp[0] = 0;
    for (int64_t c = 0; c < nc; c++) {
        for (int64_t a = coins[c]; a <= amount; a++) {
            int64_t val = dp[a - coins[c]] + 1;
            if (val < dp[a]) dp[a] = val;
        }
    }
    return dp[amount] >= 999999 ? -1 : dp[amount];
}

int main(void) {
    int64_t coins[] = {1, 5, 10, 25};
    printf("Test 1: %lld (expected 2)\n", (long long)coin_change(coins, 4, 30));
    printf("Test 2: %lld (expected 4)\n", (long long)coin_change(coins, 4, 100));
    printf("Test 3: %lld (expected 6)\n", (long long)coin_change(coins, 4, 63));

    int iterations = 500000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = coin_change(coins, 4, 100);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Coin Change ===\n");
    printf("Amount: 100, Coins: [1,5,10,25]\n");
    printf("Iterations: %d\n", iterations);
    printf("Result: %lld\n", (long long)result);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
