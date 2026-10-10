/*
 * title: Parallel Mandelbrot with fixed-point escape counts
 * topic: concurrency
 * covers: fixed-point arithmetic, dynamic row claiming, escape-count histogram, symmetry check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*ParFn)(void *ctx, int tid, int nt);
typedef struct {
    ParFn fn;
    void *ctx;
    int tid;
    int nt;
} ParJob;

static void *par_tramp(void *p) {
    ParJob *j = p;
    j->fn(j->ctx, j->tid, j->nt);
    return NULL;
}

/* Run fn on nt threads (nt <= 8) and join them all. */
static inline void par_run(int nt, ParFn fn, void *ctx) {
    pthread_t th[8];
    ParJob jobs[8];
    for (int i = 0; i < nt; i++) {
        jobs[i].fn = fn;
        jobs[i].ctx = ctx;
        jobs[i].tid = i;
        jobs[i].nt = nt;
        if (pthread_create(&th[i], NULL, par_tramp, &jobs[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (int i = 0; i < nt; i++)
        pthread_join(th[i], NULL);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#include <stdatomic.h>

enum { W = 64, H = 40, MAXIT = 200, NT = 4, SHIFT = 24 };

/* Q8.24 fixed point in long. Real axis [-2.25, 0.75], imaginary axis [-1.25, 1.25].
 * Products use division (truncation toward zero) so the arithmetic is
 * exactly symmetric under z -> conj(z). */
typedef struct {
    int *iters;
    atomic_int next_row;
} Img;

static int escape(long cr, long ci) {
    long zr = 0, zi = 0;
    for (int i = 0; i < MAXIT; i++) {
        long zr2 = (zr * zr) / (1L << SHIFT);
        long zi2 = (zi * zi) / (1L << SHIFT);
        if (zr2 + zi2 > (4L << SHIFT))
            return i;
        long nz = (2 * zr * zi) / (1L << SHIFT);
        zi = nz + ci;
        zr = zr2 - zi2 + cr;
    }
    return MAXIT;
}

static long coord_x(int x) {
    return -9L * (1L << (SHIFT - 2)) + (3L << SHIFT) * x / (W - 1);
}

static long coord_y(int y) {
    return (long)(2 * y - (H - 1)) * (5L << (SHIFT - 2)) / (H - 1);
}

static void row_worker(void *ctx, int tid, int nt) {
    Img *im = ctx;
    (void)tid;
    (void)nt;
    for (;;) {
        int y = atomic_fetch_add(&im->next_row, 1);
        if (y >= H)
            break;
        for (int x = 0; x < W; x++)
            im->iters[y * W + x] = escape(coord_x(x), coord_y(y));
    }
}

int main(void) {
    static int par[W * H], seq[W * H];
    Img im;
    im.iters = par;
    atomic_init(&im.next_row, 0);
    par_run(NT, row_worker, &im);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            seq[y * W + x] = escape(coord_x(x), coord_y(y));
    check(memcmp(par, seq, sizeof par) == 0, "parallel equals sequential");

    int inside = 0;
    long total = 0;
    int hist[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < W * H; i++) {
        total += par[i];
        if (par[i] == MAXIT)
            inside++;
        int b = par[i] == MAXIT ? 4 : par[i] < 3 ? 0 : par[i] < 8 ? 1 : par[i] < 30 ? 2 : 3;
        hist[b]++;
    }
    printf("inside=%d total_iters=%ld\n", inside, total);
    printf("hist <3:%d <8:%d <30:%d <200:%d inside:%d\n", hist[0], hist[1], hist[2], hist[3],
           hist[4]);
    for (int y = 0; y < H / 2; y++)
        for (int x = 0; x < W; x++)
            check(par[y * W + x] == par[(H - 1 - y) * W + x], "mirror symmetry");
    for (int y = 0; y < H; y += 5) {
        for (int x = 0; x < W; x += 2) {
            int v = par[y * W + x];
            putchar(v == MAXIT ? '#' : v > 10 ? '+' : v > 4 ? '.' : ' ');
        }
        putchar('\n');
    }
    return 0;
}
