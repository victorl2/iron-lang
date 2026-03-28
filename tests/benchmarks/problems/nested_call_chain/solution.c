#include <stdio.h>
#include <stdint.h>
#include <time.h>

int64_t step_a(int64_t x) { return (x * 31 + 7) % 100003; }
int64_t step_b(int64_t x) { return (x * 37 + 13) % 100003; }
int64_t step_c(int64_t x) { return (x * 41 + 17) % 100003; }
int64_t step_d(int64_t x) { return (x * 43 + 19) % 100003; }
int64_t step_e(int64_t x) { return (x * 47 + 23) % 100003; }
int64_t step_f(int64_t x) { return (x * 53 + 29) % 100003; }

int64_t compose_6(int64_t x) {
    return step_f(step_e(step_d(step_c(step_b(step_a(x))))));
}

int64_t pipeline(int64_t x, int64_t rounds) {
    int64_t v = x;
    for (int64_t i = 0; i < rounds; i++) {
        v = compose_6(v);
    }
    return v;
}

int64_t multi_arg_chain(int64_t a, int64_t b, int64_t c) {
    return step_a(step_b(a) + step_c(b)) + step_d(step_e(b) + step_f(c));
}

int64_t bench_nested(int64_t n) {
    int64_t checksum = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t v1 = pipeline(i, 10);
        int64_t v2 = multi_arg_chain(i, i + 1, i + 2);
        checksum += (v1 + v2) % 100000;
    }
    return checksum;
}

int main(void) {
    printf("Test 1 step_a(0): %lld (expected 7)\n", (long long)step_a(0));
    printf("Test 2 compose_6(0): %lld\n", (long long)compose_6(0));
    printf("Test 3 compose_6(1): %lld\n", (long long)compose_6(1));
    printf("Test 4 pipeline(0,1): %lld\n", (long long)pipeline(0, 1));
    printf("Test 5 pipeline(0,10): %lld\n", (long long)pipeline(0, 10));
    printf("Test 6 multi_arg(1,2,3): %lld\n", (long long)multi_arg_chain(1, 2, 3));

    int64_t check = bench_nested(200);
    printf("Test 7 bench(200): %lld\n", (long long)check);

    int iterations = 200000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = bench_nested(200);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Nested Call Chain ===\n");
    printf("Elements: 200\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
