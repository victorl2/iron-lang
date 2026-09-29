/*
 * title: Parallel convex hull by merging per-chunk hulls
 * topic: concurrency
 * covers: monotone chain, chunked hulls, hull of hulls, collinear handling, integer cross products, brute force check
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

enum { N = 4000, NT = 6 };

typedef struct {
    long x, y;
} P;

static int cmp_p(const void *a, const void *b) {
    const P *p = a, *q = b;
    if (p->x != q->x)
        return p->x < q->x ? -1 : 1;
    return (p->y > q->y) - (p->y < q->y);
}

static long cross(P o, P a, P b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

/* Andrew's monotone chain on points sorted by (x,y); strict hull (no collinear
 * points). Output has room for 2n. Returns hull size, counter-clockwise. */
static int hull(P *pts, int n, P *out) {
    if (n < 3) {
        for (int i = 0; i < n; i++)
            out[i] = pts[i];
        return n;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2 && cross(out[k - 2], out[k - 1], pts[i]) <= 0)
            k--;
        out[k++] = pts[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && cross(out[k - 2], out[k - 1], pts[i]) <= 0)
            k--;
        out[k++] = pts[i];
    }
    return k - 1;
}

typedef struct {
    P *pts;
    P *out[8];
    int hn[8];
} Ch;

static void chunk_worker(void *ctx, int tid, int nt) {
    Ch *c = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    qsort(c->pts + lo, (size_t)(hi - lo), sizeof(P), cmp_p);
    c->out[tid] = malloc((size_t)(2 * (hi - lo) + 2) * sizeof(P));
    check(c->out[tid] != NULL, "alloc");
    c->hn[tid] = hull(c->pts + lo, hi - lo, c->out[tid]);
}

static long area2(const P *h, int n) {
    long a = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        a += h[i].x * h[j].y - h[j].x * h[i].y;
    }
    return a;
}

int main(void) {
    static P pts[N], orig[N], all[N], hh[2 * N + 2];
    uint64_t seed = 999;
    for (int shape = 0; shape < 3; shape++) {
        for (int i = 0; i < N; i++) {
            uint64_t r = sm64(&seed);
            long x = (long)(r % 2001u) - 1000, y = (long)((r >> 20) % 2001u) - 1000;
            if (shape == 1) { /* points inside a disc-ish diamond with many on the border */
                x = (long)(r % 41u) - 20;
                y = (long)((r >> 20) % 41u) - 20;
            }
            if (shape == 2) { /* nearly collinear */
                x = (long)(r % 1000u);
                y = x / 3 + (long)((r >> 30) % 3u);
            }
            pts[i].x = orig[i].x = x;
            pts[i].y = orig[i].y = y;
        }
        Ch c;
        memset(&c, 0, sizeof c);
        c.pts = pts;
        par_run(NT, chunk_worker, &c);
        int m = 0;
        for (int t = 0; t < NT; t++) {
            for (int i = 0; i < c.hn[t]; i++)
                all[m++] = c.out[t][i];
            free(c.out[t]);
        }
        qsort(all, (size_t)m, sizeof(P), cmp_p);
        int hn = hull(all, m, hh);
        /* reference: hull of all the original points at once */
        qsort(orig, N, sizeof(P), cmp_p);
        static P ref[2 * N + 2];
        int rn = hull(orig, N, ref);
        check(hn == rn, "hull size");
        check(memcmp(hh, ref, (size_t)hn * sizeof(P)) == 0, "hull vertices");
        /* every input point lies inside or on the hull */
        for (int i = 0; i < N; i++)
            for (int k = 0; k < hn && hn >= 3; k++)
                check(cross(hh[k], hh[(k + 1) % hn], orig[i]) >= 0, "point inside hull");
        printf("shape %d: merged %d chunk-hull points -> hull size %d, area*2=%ld, leftmost (%ld,%ld)\n",
               shape, m, hn, area2(hh, hn), hh[0].x, hh[0].y);
    }
    return 0;
}
