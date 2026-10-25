/*
 * title: Parallel integer k-means with fixed-order reduction
 * topic: concurrency
 * covers: Lloyd iterations, per-thread accumulators, ordered merge, integer centroids, tie-breaking by lowest index
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

enum { N = 1200, K = 5, NT = 6, MAXIT = 50 };

typedef struct {
    int x, y;
} Pt;

typedef struct {
    const Pt *pts;
    const Pt *cent;
    int *assign;
    long sx[8][K], sy[8][K];
    long cnt[8][K];
    long cost[8];
    int moved[8];
} Km;

static long d2(Pt a, Pt b) {
    long dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

static int nearest(const Pt *cent, Pt p) {
    int best = 0;
    long bd = d2(p, cent[0]);
    for (int k = 1; k < K; k++) {
        long d = d2(p, cent[k]);
        if (d < bd) { /* strict: ties go to the lowest index */
            bd = d;
            best = k;
        }
    }
    return best;
}

static void assign_worker(void *ctx, int tid, int nt) {
    Km *m = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    memset(m->sx[tid], 0, sizeof m->sx[tid]);
    memset(m->sy[tid], 0, sizeof m->sy[tid]);
    memset(m->cnt[tid], 0, sizeof m->cnt[tid]);
    m->cost[tid] = 0;
    m->moved[tid] = 0;
    for (int i = lo; i < hi; i++) {
        int k = nearest(m->cent, m->pts[i]);
        if (m->assign[i] != k)
            m->moved[tid]++;
        m->assign[i] = k;
        m->sx[tid][k] += m->pts[i].x;
        m->sy[tid][k] += m->pts[i].y;
        m->cnt[tid][k]++;
        m->cost[tid] += d2(m->pts[i], m->cent[k]);
    }
}

/* floor division for possibly negative sums */
static int fdiv(long a, long b) {
    long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return (int)q;
}

static int run(int nt, const Pt *pts, Pt *cent, int *assign, long *cost_out) {
    static Km m;
    m.pts = pts;
    m.cent = cent;
    m.assign = assign;
    for (int i = 0; i < N; i++)
        assign[i] = -1;
    int it;
    for (it = 1; it <= MAXIT; it++) {
        par_run(nt, assign_worker, &m);
        long sx[K] = {0}, sy[K] = {0}, cn[K] = {0}, cost = 0;
        int moved = 0;
        for (int t = 0; t < nt; t++) { /* fixed thread order */
            for (int k = 0; k < K; k++) {
                sx[k] += m.sx[t][k];
                sy[k] += m.sy[t][k];
                cn[k] += m.cnt[t][k];
            }
            cost += m.cost[t];
            moved += m.moved[t];
        }
        *cost_out = cost;
        if (moved == 0)
            break;
        for (int k = 0; k < K; k++)
            if (cn[k]) {
                cent[k].x = fdiv(sx[k], cn[k]);
                cent[k].y = fdiv(sy[k], cn[k]);
            }
    }
    return it;
}

int main(void) {
    static Pt pts[N];
    uint64_t seed = 777;
    static const Pt centers[K] = {{-500, -400}, {450, -300}, {0, 500}, {600, 600}, {-450, 350}};
    for (int i = 0; i < N; i++) {
        uint64_t r = sm64(&seed);
        int c = (int)(r % K);
        pts[i].x = centers[c].x + (int)((r >> 8) % 201u) - 100 + (int)((r >> 20) % 101u) - 50;
        pts[i].y = centers[c].y + (int)((r >> 32) % 201u) - 100 + (int)((r >> 44) % 101u) - 50;
    }
    static int assign1[N], assignN[N];
    Pt c1[K], cN[K];
    for (int k = 0; k < K; k++) /* deterministic init: every (N/K)th point */
        c1[k] = cN[k] = pts[k * (N / K)];
    long cost1, costN;
    int it1 = run(1, pts, c1, assign1, &cost1);
    printf("1 thread: iterations=%d cost=%ld\n", it1, cost1);
    for (int nt = 2; nt <= 8; nt++) {
        for (int k = 0; k < K; k++)
            cN[k] = pts[k * (N / K)];
        int itn = run(nt, pts, cN, assignN, &costN);
        check(itn == it1 && costN == cost1, "same iterations and cost");
        check(memcmp(c1, cN, sizeof c1) == 0, "same centroids");
        check(memcmp(assign1, assignN, sizeof assign1) == 0, "same assignment");
    }
    long sizes[K] = {0};
    for (int i = 0; i < N; i++)
        sizes[assign1[i]]++;
    for (int k = 0; k < K; k++)
        printf("cluster %d: centroid (%d,%d) size %ld\n", k, c1[k].x, c1[k].y, sizes[k]);
    printf("identical for 2..8 threads\n");
    return 0;
}
