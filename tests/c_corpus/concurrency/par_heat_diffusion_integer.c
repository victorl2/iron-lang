/*
 * title: Parallel integer heat diffusion on a 2D grid
 * topic: concurrency
 * covers: double buffering, fixed boundary, integer stencil with remainder carry, energy conservation
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

static inline uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

enum { W = 40, H = 30, STEPS = 60, NT = 6 };

/* Integer diffusion that conserves total heat exactly: each interior cell
 * sends floor(v/8) to each of its four neighbours and keeps the rest.
 * The grid is a closed torus so the sum is invariant. */
typedef struct {
    const long *cur;
    long *next;
} Grid;

static int idx(int x, int y) {
    x = (x + W) % W;
    y = (y + H) % H;
    return y * W + x;
}

static void step_worker(void *ctx, int tid, int nt) {
    Grid *g = ctx;
    int lo, hi;
    par_range(H, tid, nt, &lo, &hi);
    for (int y = lo; y < hi; y++)
        for (int x = 0; x < W; x++) {
            long v = g->cur[idx(x, y)];
            long give = v / 8;
            long r = v - 4 * give;
            long in = g->cur[idx(x - 1, y)] / 8 + g->cur[idx(x + 1, y)] / 8 +
                      g->cur[idx(x, y - 1)] / 8 + g->cur[idx(x, y + 1)] / 8;
            g->next[idx(x, y)] = r + in;
        }
}

static void seq_step(const long *cur, long *next) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            long v = cur[idx(x, y)];
            long give = v / 8;
            long r = v - 4 * give;
            long in = cur[idx(x - 1, y)] / 8 + cur[idx(x + 1, y)] / 8 + cur[idx(x, y - 1)] / 8 +
                      cur[idx(x, y + 1)] / 8;
            next[idx(x, y)] = r + in;
        }
}

static long total(const long *g) {
    long s = 0;
    for (int i = 0; i < W * H; i++)
        s += g[i];
    return s;
}

int main(void) {
    static long pa[W * H], pb[W * H], sa[W * H], sb[W * H];
    uint64_t seed = 6060;
    for (int i = 0; i < 12; i++) {
        int x = (int)(sm64(&seed) % W);
        int y = (int)(sm64(&seed) % H);
        pa[idx(x, y)] += 100000 + (long)(sm64(&seed) % 50000u);
    }
    memcpy(sa, pa, sizeof pa);
    long t0 = total(pa);
    long *cur = pa, *nxt = pb, *scur = sa, *snxt = sb;
    for (int s = 1; s <= STEPS; s++) {
        Grid g = {cur, nxt};
        par_run(NT, step_worker, &g);
        seq_step(scur, snxt);
        long *t = cur;
        cur = nxt;
        nxt = t;
        t = scur;
        scur = snxt;
        snxt = t;
        check(memcmp(cur, scur, sizeof pa) == 0, "parallel equals sequential each step");
        check(total(cur) == t0, "heat conserved");
        if (s == 1 || s % 15 == 0) {
            long mx = 0, mn = cur[0];
            for (int i = 0; i < W * H; i++) {
                if (cur[i] > mx)
                    mx = cur[i];
                if (cur[i] < mn)
                    mn = cur[i];
            }
            printf("step %2d: total=%ld max=%ld min=%ld center=%ld\n", s, total(cur), mx, mn,
                   cur[idx(W / 2, H / 2)]);
        }
    }
    return 0;
}
