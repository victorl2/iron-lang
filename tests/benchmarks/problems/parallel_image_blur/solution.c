#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define WIDTH 500
#define HEIGHT 500
#define NUM_THREADS 8
#define ITERATIONS 500

static int64_t compute_blur_row(int64_t row, int64_t width, int64_t height) {
    int64_t row_sum = 0;
    for (int64_t c = 0; c < width; c++) {
        int64_t acc = 0;
        int64_t count = 0;
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                int64_t nr = row + dr;
                int64_t nc = c + dc;
                if (nr >= 0 && nr < height && nc >= 0 && nc < width) {
                    int64_t pixel = (nr * width + nc) % 256;
                    acc += pixel;
                    count++;
                }
            }
        }
        int64_t blurred = acc / count;
        row_sum += blurred;
    }
    return row_sum;
}

typedef struct {
    int64_t lo;
    int64_t hi;
} chunk_t;

static void* worker(void* arg) {
    chunk_t* c = (chunk_t*)arg;
    for (int64_t r = c->lo; r < c->hi; r++) {
        volatile int64_t s = compute_blur_row(r, WIDTH, HEIGHT);
        (void)s;
    }
    return NULL;
}

int main(void) {
    /* Sequential: compute checksum */
    int64_t checksum = 0;
    for (int64_t r = 0; r < HEIGHT; r++) {
        checksum += compute_blur_row(r, WIDTH, HEIGHT);
    }
    printf("Image size: %dx%d\n", WIDTH, HEIGHT);
    printf("Blur checksum: %lld\n", checksum);

    /* Benchmark: parallel blur per row */
    struct timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    for (int it = 0; it < ITERATIONS; it++) {
        pthread_t threads[NUM_THREADS];
        chunk_t chunks[NUM_THREADS];
        int64_t chunk_size = HEIGHT / NUM_THREADS;

        for (int t = 0; t < NUM_THREADS; t++) {
            chunks[t].lo = t * chunk_size;
            chunks[t].hi = (t == NUM_THREADS - 1) ? HEIGHT : (t + 1) * chunk_size;
            pthread_create(&threads[t], NULL, worker, &chunks[t]);
        }
        for (int t = 0; t < NUM_THREADS; t++) {
            pthread_join(threads[t], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                      + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;

    printf("\n=== Benchmark: Parallel Image Blur ===\n");
    printf("Image: %dx%d\n", WIDTH, HEIGHT);
    printf("Iterations: %d\n", ITERATIONS);
    printf("Total time: %.3f ms\n", elapsed_ms);

    return 0;
}
