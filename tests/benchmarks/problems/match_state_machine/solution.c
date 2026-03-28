#include <stdio.h>
#include <stdint.h>
#include <time.h>

int64_t transition(int64_t state, int64_t event) {
    switch (state) {
        case 0: /* IDLE */
            switch (event) {
                case 0: return 3;
                case 5: return 0;
                default: return 0;
            }
        case 1: /* RUNNING */
            switch (event) {
                case 1: return 2;
                case 4: return 4;
                default: return 1;
            }
        case 2: /* BLOCKED */
            switch (event) {
                case 2: return 3;
                case 4: return 4;
                default: return 2;
            }
        case 3: /* READY */
            switch (event) {
                case 3: return 1;
                case 4: return 4;
                default: return 3;
            }
        case 4: /* TERMINATED */
            switch (event) {
                case 5: return 0;
                default: return 4;
            }
        default: return 0;
    }
}

int64_t classify(int64_t val) {
    int64_t bucket = val % 10;
    switch (bucket) {
        case 0: return 100;
        case 1: return 200;
        case 2: return 300;
        case 3: return 400;
        case 4: return 500;
        case 5: return 600;
        case 6: return 700;
        case 7: return 800;
        case 8: return 900;
        case 9: return 1000;
        default: return 0;
    }
}

int64_t simulate(int64_t steps) {
    int64_t state = 0;
    int64_t checksum = 0;
    for (int64_t i = 0; i < steps; i++) {
        int64_t event = (i * 7 + 3) % 6;
        state = transition(state, event);
        checksum += state * (i + 1);
        checksum += classify(i);
    }
    return checksum;
}

int main(void) {
    printf("Test 1 transition(0,0): %lld (expected 3)\n", (long long)transition(0, 0));
    printf("Test 2 transition(3,3): %lld (expected 1)\n", (long long)transition(3, 3));
    printf("Test 3 transition(1,1): %lld (expected 2)\n", (long long)transition(1, 1));
    printf("Test 4 transition(2,2): %lld (expected 3)\n", (long long)transition(2, 2));
    printf("Test 5 transition(1,4): %lld (expected 4)\n", (long long)transition(1, 4));
    printf("Test 6 classify(7): %lld (expected 800)\n", (long long)classify(7));

    int64_t check = simulate(200);
    printf("Test 7 simulate(200): %lld\n", (long long)check);

    int iterations = 5000000;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    volatile int64_t result = 0;
    for (int it = 0; it < iterations; it++) {
        result = simulate(200);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Match State Machine ===\n");
    printf("Steps: 200\n");
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
