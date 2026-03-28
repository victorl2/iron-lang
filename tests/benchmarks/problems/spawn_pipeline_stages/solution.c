#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

#define DATA_SIZE 500000
#define NUM_THREADS 8

/* Stage 1: Generate data deterministically */
static void generate_data(int64_t *data, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
        data[i] = (i * 73 + 29) % 10007;
    }
}

/* Transform a single element (heavy computation: 40 iterations) */
static int64_t transform_element(int64_t val, int64_t idx) {
    int64_t result = val;
    for (int k = 0; k < 40; k++) {
        result = (result * 31 + idx + k) % 1000000007;
    }
    return result;
}

typedef struct {
    int64_t *data;
    int64_t *output;
    int64_t lo;
    int64_t hi;
} transform_chunk_t;

static void *transform_worker(void *arg) {
    transform_chunk_t *c = (transform_chunk_t *)arg;
    for (int64_t i = c->lo; i < c->hi; i++) {
        c->output[i] = transform_element(c->data[i], i);
    }
    return NULL;
}

/* Stage 3: Aggregate - sum all transformed values */
static int64_t aggregate(int64_t *data, int64_t n) {
    int64_t sum = 0;
    for (int64_t i = 0; i < n; i++) {
        sum = (sum + data[i]) % 1000000007;
    }
    return sum;
}

int main(void) {
    int64_t *data = (int64_t *)malloc(DATA_SIZE * sizeof(int64_t));
    int64_t *output = (int64_t *)malloc(DATA_SIZE * sizeof(int64_t));

    /* Stage 1: Generate */
    generate_data(data, DATA_SIZE);
    printf("stage 1: generated %d elements\n", DATA_SIZE);
    printf("data[0]: %lld\n", (long long)data[0]);
    printf("data[999]: %lld\n", (long long)data[999]);

    /* Stage 2: Parallel transform */
    transform_chunk_t chunks[NUM_THREADS];
    pthread_t threads[NUM_THREADS];
    int64_t chunk_size = DATA_SIZE / NUM_THREADS;

    for (int t = 0; t < NUM_THREADS; t++) {
        chunks[t].data = data;
        chunks[t].output = output;
        chunks[t].lo = t * chunk_size;
        chunks[t].hi = (t == NUM_THREADS - 1) ? DATA_SIZE : (t + 1) * chunk_size;
        pthread_create(&threads[t], NULL, transform_worker, &chunks[t]);
    }

    for (int t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }

    printf("stage 2: transformed %d elements\n", DATA_SIZE);
    printf("output[0]: %lld\n", (long long)output[0]);
    printf("output[999]: %lld\n", (long long)output[999]);

    /* Stage 3: Aggregate */
    int64_t result = aggregate(output, DATA_SIZE);
    printf("stage 3: aggregate = %lld\n", (long long)result);

    /* Verification: small sequential transform check */
    int64_t verify = 0;
    for (int64_t i = 0; i < 100; i++) {
        verify = (verify + transform_element(data[i], i)) % 1000000007;
    }
    printf("verify first 100: %lld\n", (long long)verify);

    /* Benchmark timing */
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    generate_data(data, DATA_SIZE);
    for (int t = 0; t < NUM_THREADS; t++) {
        chunks[t].data = data;
        chunks[t].output = output;
        chunks[t].lo = t * chunk_size;
        chunks[t].hi = (t == NUM_THREADS - 1) ? DATA_SIZE : (t + 1) * chunk_size;
        pthread_create(&threads[t], NULL, transform_worker, &chunks[t]);
    }
    for (int t = 0; t < NUM_THREADS; t++) {
        pthread_join(threads[t], NULL);
    }
    volatile int64_t r = aggregate(output, DATA_SIZE);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                      + (end.tv_nsec - start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Spawn Pipeline Stages ===\n");
    printf("Data size: %d\n", DATA_SIZE);
    printf("Threads: %d\n", NUM_THREADS);
    printf("Total time: %.3f ms\n", elapsed_ms);

    free(data);
    free(output);
    return 0;
}
