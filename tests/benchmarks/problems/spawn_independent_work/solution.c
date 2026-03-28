#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

/* Each task computes sum of i^2 for i in [0, N) with a task-specific offset */
#define WORK_SIZE 5000000

static int64_t heavy_compute(int64_t offset) {
    int64_t sum = 0;
    for (int64_t i = 0; i < WORK_SIZE; i++) {
        int64_t v = (i + offset) % 100003;
        sum = sum + v * v;
        sum = sum % 1000000007;
    }
    return sum;
}

typedef struct {
    int64_t offset;
    int64_t result;
} task_arg_t;

static void *worker(void *arg) {
    task_arg_t *ta = (task_arg_t *)arg;
    ta->result = heavy_compute(ta->offset);
    return NULL;
}

int main(void) {
    /* 8 tasks with different offsets */
    task_arg_t tasks[8];
    pthread_t threads[8];
    int64_t offsets[8] = {0, 7, 13, 29, 37, 53, 67, 97};

    for (int t = 0; t < 8; t++) {
        tasks[t].offset = offsets[t];
        pthread_create(&threads[t], NULL, worker, &tasks[t]);
    }

    /* Main thread also does work */
    int64_t main_result = heavy_compute(101);

    for (int t = 0; t < 8; t++) {
        pthread_join(threads[t], NULL);
    }

    /* Sequential verification: compute checksum of all results */
    int64_t checksum = main_result;
    for (int t = 0; t < 8; t++) {
        checksum = (checksum + tasks[t].result) % 1000000007;
    }

    printf("task 0: %lld\n", (long long)tasks[0].result);
    printf("task 1: %lld\n", (long long)tasks[1].result);
    printf("task 2: %lld\n", (long long)tasks[2].result);
    printf("task 3: %lld\n", (long long)tasks[3].result);
    printf("task 4: %lld\n", (long long)tasks[4].result);
    printf("task 5: %lld\n", (long long)tasks[5].result);
    printf("task 6: %lld\n", (long long)tasks[6].result);
    printf("task 7: %lld\n", (long long)tasks[7].result);
    printf("main: %lld\n", (long long)main_result);
    printf("checksum: %lld\n", (long long)checksum);

    /* Benchmark timing */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Re-run all tasks once more for timing */
    for (int t = 0; t < 8; t++) {
        tasks[t].offset = offsets[t];
        pthread_create(&threads[t], NULL, worker, &tasks[t]);
    }
    volatile int64_t mr = heavy_compute(101);
    for (int t = 0; t < 8; t++) {
        pthread_join(threads[t], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Spawn Independent Work ===\n");
    printf("Tasks: 9 (8 spawned + main)\n");
    printf("Work per task: %d iterations\n", WORK_SIZE);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
