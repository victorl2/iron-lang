#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define NUM_TASKS 16
#define ITERATIONS 5

static int64_t fib(int64_t n) {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}

typedef struct {
    int64_t index;
} task_t;

static void* worker(void* arg) {
    task_t* t = (task_t*)arg;
    int64_t idx = t->index % 4;
    volatile int64_t r = fib(30 + idx);
    (void)r;
    return NULL;
}

int main(void) {
    /* Sequential verification */
    int64_t expected_sum = 0;
    for (int64_t i = 0; i < NUM_TASKS; i++) {
        int64_t idx = i % 4;
        expected_sum += fib(30 + idx);
    }
    printf("Fib sum (16 tasks): %lld\n", expected_sum);

    printf("fib(30): %lld\n", fib(30));
    printf("fib(31): %lld\n", fib(31));
    printf("fib(32): %lld\n", fib(32));
    printf("fib(33): %lld\n", fib(33));

    /* Benchmark: parallel fib computation */
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int it = 0; it < ITERATIONS; it++) {
        pthread_t threads[NUM_TASKS];
        task_t tasks[NUM_TASKS];
        for (int i = 0; i < NUM_TASKS; i++) {
            tasks[i].index = i;
            pthread_create(&threads[i], NULL, worker, &tasks[i]);
        }
        for (int i = 0; i < NUM_TASKS; i++) {
            pthread_join(threads[i], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                      + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Fibonacci Tree ===\n");
    printf("Tasks: %d\n", NUM_TASKS);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
