#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

typedef struct { int64_t m[4]; } Mat2;

Mat2 mat_mul(Mat2 a, Mat2 b, int64_t mod) {
    Mat2 c;
    c.m[0] = (a.m[0] * b.m[0] + a.m[1] * b.m[2]) % mod;
    c.m[1] = (a.m[0] * b.m[1] + a.m[1] * b.m[3]) % mod;
    c.m[2] = (a.m[2] * b.m[0] + a.m[3] * b.m[2]) % mod;
    c.m[3] = (a.m[2] * b.m[1] + a.m[3] * b.m[3]) % mod;
    return c;
}

Mat2 mat_pow(Mat2 base, int64_t exp, int64_t mod) {
    Mat2 result = {{1, 0, 0, 1}}; // identity
    while (exp > 0) {
        if (exp % 2 == 1) {
            result = mat_mul(result, base, mod);
        }
        base = mat_mul(base, base, mod);
        exp /= 2;
    }
    return result;
}

int64_t fibonacci(int64_t n, int64_t mod) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    Mat2 m = {{1, 1, 1, 0}};
    Mat2 result = mat_pow(m, n, mod);
    return result.m[1];
}

long get_memory_kb(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;
}

int main(void) {
    int64_t mod = 1000000007;
    printf("Test 1 fib(10): %lld (expected 55)\n", fibonacci(10, mod));
    printf("Test 2 fib(20): %lld (expected 6765)\n", fibonacci(20, mod));
    printf("Test 3 fib(50): %lld (expected 586268941)\n", fibonacci(50, mod));

    int64_t n = 50;
    printf("Bench check: %lld\n", fibonacci(n, mod));

    long mem_before = get_memory_kb();
    int iterations = 1000000;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = fibonacci(n, mod);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    long mem_after = get_memory_kb();
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Fibonacci Matrix ===\n");
    printf("n=%lld\n", n);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    printf("Avg per call: %.6f ms\n", elapsed_ms / iterations);
    printf("Memory (peak RSS): %ld KB\n", mem_after > mem_before ? mem_after : mem_before);

    return 0;
}
