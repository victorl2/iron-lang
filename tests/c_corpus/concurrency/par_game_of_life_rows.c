/*
 * title: Parallel Game of Life with row bands
 * topic: concurrency
 * covers: toroidal grid, double buffering, glider periodicity, population history vs sequential
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

enum { W = 48, H = 32, GENS = 80, NT = 7 };

typedef struct {
    const unsigned char *cur;
    unsigned char *next;
} Life;

static int neighbors(const unsigned char *g, int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy)
                n += g[((y + dy + H) % H) * W + (x + dx + W) % W];
    return n;
}

static void row_range(const unsigned char *cur, unsigned char *next, int lo, int hi) {
    for (int y = lo; y < hi; y++)
        for (int x = 0; x < W; x++) {
            int n = neighbors(cur, x, y);
            int alive = cur[y * W + x];
            next[y * W + x] = (unsigned char)(n == 3 || (alive && n == 2));
        }
}

static void life_worker(void *ctx, int tid, int nt) {
    Life *l = ctx;
    int lo, hi;
    par_range(H, tid, nt, &lo, &hi);
    row_range(l->cur, l->next, lo, hi);
}

static int population(const unsigned char *g) {
    int p = 0;
    for (int i = 0; i < W * H; i++)
        p += g[i];
    return p;
}

int main(void) {
    static unsigned char pa[W * H], pb[W * H], sa[W * H], sb[W * H];
    uint64_t seed = 4711;
    for (int i = 0; i < W * H; i++)
        pa[i] = (unsigned char)((sm64(&seed) % 100u) < 30u);
    memcpy(sa, pa, sizeof pa);
    unsigned char *cur = pa, *nxt = pb, *scur = sa, *snxt = sb;
    int hist[GENS + 1];
    hist[0] = population(cur);
    printf("gen 0 pop %d\n", hist[0]);
    for (int g = 1; g <= GENS; g++) {
        Life l = {cur, nxt};
        par_run(NT, life_worker, &l);
        row_range(scur, snxt, 0, H);
        unsigned char *t = cur;
        cur = nxt;
        nxt = t;
        t = scur;
        scur = snxt;
        snxt = t;
        check(memcmp(cur, scur, W * H) == 0, "parallel equals sequential");
        hist[g] = population(cur);
        if (g % 20 == 0)
            printf("gen %d pop %d\n", g, hist[g]);
    }
    /* glider on an empty torus returns to its start after 4*W generations
     * when W == H; here W != H so check the period lcm-style: 4*lcm(W,H)/... */
    static unsigned char ga[W * H], gb[W * H];
    ga[0 * W + 1] = ga[1 * W + 2] = ga[2 * W + 0] = ga[2 * W + 1] = ga[2 * W + 2] = 1;
    unsigned char start[W * H];
    memcpy(start, ga, sizeof ga);
    unsigned char *c = ga, *n = gb;
    int period = 0;
    for (int g = 1; g <= 4 * W * H; g++) {
        Life l = {c, n};
        par_run(NT, life_worker, &l);
        unsigned char *t = c;
        c = n;
        n = t;
        check(population(c) == 5, "glider keeps 5 cells");
        if (memcmp(c, start, sizeof start) == 0) {
            period = g;
            break;
        }
    }
    check(period > 0, "glider returns");
    printf("glider period on %dx%d torus: %d\n", W, H, period);
    return 0;
}
