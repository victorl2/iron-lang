#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define WIDTH 1000
#define HEIGHT 1000
#define TOTAL_PIXELS (WIDTH * HEIGHT)
#define NUM_SPHERES 20
#define IMAGE_DEPTH 1000

static int64_t sphere_cx(int64_t s) { return ((s * 317 + 113) % 1000) - 500; }
static int64_t sphere_cy(int64_t s) { return ((s * 241 + 79) % 1000) - 500; }
static int64_t sphere_cz(int64_t s) { return 300 + s * 50 + ((s * 127) % 200); }
static int64_t sphere_r(int64_t s) { return 40 + (s * 17) % 60; }
static int64_t sphere_red(int64_t s) { return (s * 73 + 50) % 256; }
static int64_t sphere_green(int64_t s) { return (s * 127 + 100) % 256; }
static int64_t sphere_blue(int64_t s) { return (s * 191 + 150) % 256; }

static int64_t isqrt(int64_t n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    int64_t x = n;
    int64_t y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

static int64_t count_divisors_small(int64_t n) {
    int64_t count = 0;
    int64_t d = 1;
    while (d * d <= n) {
        if (n % d == 0) { count++; if (d != n / d) count++; }
        d++;
    }
    return count;
}

static int64_t ray_sphere_test(int64_t px, int64_t py, int64_t s) {
    int64_t dx = px - 500, dy = py - 500, dz = IMAGE_DEPTH;
    int64_t cx = sphere_cx(s), cy = sphere_cy(s), cz = sphere_cz(s), r = sphere_r(s);
    int64_t ocx = -cx, ocy = -cy, ocz = -cz;
    int64_t a = dx*dx + dy*dy + dz*dz;
    int64_t b = 2*(ocx*dx + ocy*dy + ocz*dz);
    int64_t c = ocx*ocx + ocy*ocy + ocz*ocz - r*r;
    int64_t disc = b*b - 4*a*c;
    if (disc < 0) return 0;
    int64_t sqrt_disc = isqrt(disc);
    int64_t t_num = (-b) - sqrt_disc;
    if (t_num < 0) return 0;
    int64_t t_scaled = t_num * 100 / (2*a + 1);
    int64_t hx = dx*t_scaled/100, hy = dy*t_scaled/100, hz = dz*t_scaled/100;
    int64_t nx = hx-cx, ny = hy-cy, nz = hz-cz;
    int64_t dot = nx + ny - nz;
    if (dot < 0) dot = 0;
    int64_t norm_len = isqrt(nx*nx + ny*ny + nz*nz);
    int64_t intensity = 50;
    if (norm_len > 0) intensity = 50 + dot*200/(norm_len+1);
    if (intensity > 255) intensity = 255;
    return intensity * 1000 + s;
}

static int64_t trace_pixel(int64_t pixel) {
    int64_t px = pixel % WIDTH, py = pixel / WIDTH;
    int64_t best_color = 0, best_depth = 999999999;
    for (int64_t s = 0; s < NUM_SPHERES; s++) {
        int64_t hit = ray_sphere_test(px, py, s);
        if (hit > 0) {
            int64_t intensity = hit / 1000;
            int64_t sphere_idx = hit - intensity * 1000;
            int64_t depth = sphere_cz(sphere_idx);
            if (depth < best_depth) {
                best_depth = depth;
                int64_t rv = (intensity * sphere_red(sphere_idx)) / 256;
                int64_t gv = (intensity * sphere_green(sphere_idx)) / 256;
                int64_t bv = (intensity * sphere_blue(sphere_idx)) / 256;
                best_color = rv + gv + bv;
            }
        }
    }
    int64_t extra = count_divisors_small(pixel % 50000 + 2);
    int64_t result = best_color + extra % 10;
    if (result == -1) printf("x\n");  /* prevent DCE */
    return result;
}

typedef struct { int start; int end; } thread_arg_t;

static void *worker(void *arg) {
    thread_arg_t *ta = (thread_arg_t *)arg;
    for (int i = ta->start; i < ta->end; i++) {
        volatile int64_t c = trace_pixel(i);
        (void)c;
    }
    return NULL;
}

int main(void) {
    int64_t checksum = 0;
    for (int i = 0; i < TOTAL_PIXELS; i++) checksum += trace_pixel(i);
    printf("Color checksum: %lld\n", checksum);
    printf("pixel(0): %lld\n", trace_pixel(0));
    printf("pixel(500500): %lld\n", trace_pixel(500500));
    printf("pixel(999999): %lld\n", trace_pixel(999999));

    int iterations = 3;
    int num_threads = 8;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int iter = 0; iter < iterations; iter++) {
        pthread_t threads[8];
        thread_arg_t args[8];
        int chunk = (TOTAL_PIXELS + num_threads - 1) / num_threads;
        for (int t = 0; t < num_threads; t++) {
            args[t].start = t * chunk;
            args[t].end = (t + 1) * chunk;
            if (args[t].end > TOTAL_PIXELS) args[t].end = TOTAL_PIXELS;
            pthread_create(&threads[t], NULL, worker, &args[t]);
        }
        for (int t = 0; t < num_threads; t++) pthread_join(threads[t], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed_ms = (end.tv_sec - start.tv_sec)*1000.0 + (end.tv_nsec - start.tv_nsec)/1e6;
    printf("\n=== Benchmark: Parallel Ray Trace ===\n");
    printf("Pixels: %d\n", TOTAL_PIXELS);
    printf("Iterations: %d\n", iterations);
    printf("Total time: %.3f ms\n", elapsed_ms);
    return 0;
}
