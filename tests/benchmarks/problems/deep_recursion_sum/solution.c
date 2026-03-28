#include <stdio.h>
#include <stdint.h>
#include <time.h>

int64_t recursive_sum(int64_t n) {
    if (n <= 0) return 0;
    return n + recursive_sum(n - 1);
}

int64_t is_even(int64_t n);
int64_t is_odd(int64_t n);

int64_t is_even(int64_t n) {
    if (n == 0) return 1;
    return is_odd(n - 1);
}

int64_t is_odd(int64_t n) {
    if (n == 0) return 0;
    return is_even(n - 1);
}

int64_t rec_power(int64_t base, int64_t exp, int64_t mod) {
    if (exp == 0) return 1;
    if (exp % 2 == 0) {
        int64_t half = rec_power(base, exp / 2, mod);
        return (half * half) % mod;
    }
    return (base * rec_power(base, exp - 1, mod)) % mod;
}

int64_t bench_recursion(int64_t depth) {
    int64_t sum = recursive_sum(depth);
    int64_t even_check = is_even(depth);
    int64_t odd_check = is_odd(depth);
    int64_t pow_val = rec_power(3, depth % 30, 1000000007);
    return sum + even_check * 10000 + odd_check * 100 + pow_val;
}

int main(void) {
    printf("Test 1 sum(10): %lld (expected 55)\n", (long long)recursive_sum(10));
    printf("Test 2 sum(100): %lld (expected 5050)\n", (long long)recursive_sum(100));
    printf("Test 3 is_even(4): %lld (expected 1)\n", (long long)is_even(4));
    printf("Test 4 is_odd(7): %lld (expected 1)\n", (long long)is_odd(7));
    printf("Test 5 is_even(7): %lld (expected 0)\n", (long long)is_even(7));
    printf("Test 6 power(2,10): %lld (expected 1024)\n", (long long)rec_power(2, 10, 1000000007));

    int64_t check = bench_recursion(1000);
    printf("Test 7 bench(1000): %lld\n", (long long)check);

    int iterations = 100000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = bench_recursion(1000);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Deep Recursion Sum ===\n");
    printf("Depth: 1000\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
