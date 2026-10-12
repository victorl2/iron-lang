/*
 * title: Parallel red-black Gauss-Seidel relaxation in integers
 * topic: concurrency
 * covers: colored sweeps, in-place update safety, fixed boundary, convergence to fixed point, sequential same-order reference
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

/* Even split of [0,n) into nt contiguous ranges. */
static inline void par_range(int n, int tid, int nt, int *lo, int *hi) {
    *lo = (int)((long)n * tid / nt);
    *hi = (int)((long)n * (tid + 1) / nt);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

enum { W = 34, H = 26, NT = 6, SWEEPS = 5000 };

typedef struct {
    long *g;
    int color;
    long delta[8];
} Rb;

/* Points with (x+y)%2 == color depend only on the other color, so all points
 * of one color can be updated in place concurrently. Each thread takes rows. */
static long relax_rows(long *g, int color, int lo, int hi) {
    long d = 0;
    for (int y = lo; y < hi; y++)
        for (int x = 1; x < W - 1; x++)
            if (((x + y) & 1) == color) {
                long nv = (g[y * W + x - 1] + g[y * W + x + 1] + g[(y - 1) * W + x] +
                           g[(y + 1) * W + x]) / 4;
                d += nv > g[y * W + x] ? nv - g[y * W + x] : g[y * W + x] - nv;
                g[y * W + x] = nv;
            }
    return d;
}

static void rb_worker(void *ctx, int tid, int nt) {
    Rb *r = ctx;
    int lo, hi;
    par_range(H - 2, tid, nt, &lo, &hi);
    r->delta[tid] = relax_rows(r->g, r->color, lo + 1, hi + 1);
}

int main(void) {
    static long pg[W * H], sg[W * H];
    /* boundary: top edge hot, bottom edge medium, sides ramp */
    for (int x = 0; x < W; x++) {
        pg[x] = sg[x] = 100000;
        pg[(H - 1) * W + x] = sg[(H - 1) * W + x] = 20000;
    }
    for (int y = 1; y < H - 1; y++) {
        long v = 100000 - (long)(80000 * y) / (H - 1);
        pg[y * W] = sg[y * W] = v;
        pg[y * W + W - 1] = sg[y * W + W - 1] = v / 2;
    }
    int converged_at = -1;
    long lastdelta = 0;
    for (int s = 1; s <= SWEEPS; s++) {
        long dsum = 0, sd = 0;
        for (int color = 0; color < 2; color++) {
            Rb r;
            memset(&r, 0, sizeof r);
            r.g = pg;
            r.color = color;
            par_run(NT, rb_worker, &r);
            for (int t = 0; t < NT; t++)
                dsum += r.delta[t];
            sd += relax_rows(sg, color, 1, H - 1);
        }
        check(memcmp(pg, sg, sizeof pg) == 0, "parallel equals sequential red-black");
        check(dsum == sd, "same total change");
        lastdelta = dsum;
        if (dsum == 0) {
            converged_at = s;
            break;
        }
        if (s == 1 || s == 10 || s == 100)
            printf("sweep %3d: total change %ld center=%ld\n", s, dsum, pg[(H / 2) * W + W / 2]);
    }
    printf("converged at sweep %d, last change %ld\n", converged_at, lastdelta);
    check(converged_at > 0, "reached integer fixed point");
    for (int y = 0; y < H; y += 5)
        printf("row %2d: %6ld %6ld %6ld\n", y, pg[y * W + 1], pg[y * W + W / 2], pg[y * W + W - 2]);
    /* discrete maximum principle */
    for (int i = 0; i < W * H; i++)
        check(pg[i] >= 0 && pg[i] <= 100000, "max principle");
    return 0;
}
