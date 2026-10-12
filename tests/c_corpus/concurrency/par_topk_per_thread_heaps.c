/*
 * title: Parallel top-k selection with per-thread heaps and ordered merge
 * topic: concurrency
 * covers: bounded min-heaps, total-order tie-break by index, heap merge, k larger than chunk, duplicates
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

enum { N = 20000, NT = 7 };

typedef struct {
    int score;
    int id;
} It;

/* a is "better" than b: higher score, ties broken by lower id (total order) */
static int better(It a, It b) {
    return a.score > b.score || (a.score == b.score && a.id < b.id);
}

typedef struct {
    It h[64];
    int n, k;
} Heap; /* min-heap of the k best: root is the worst kept item */

static void sift_down(Heap *h, int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, w = i;
        if (l < h->n && better(h->h[w], h->h[l]))
            w = l;
        if (r < h->n && better(h->h[w], h->h[r]))
            w = r;
        if (w == i)
            return;
        It t = h->h[i];
        h->h[i] = h->h[w];
        h->h[w] = t;
        i = w;
    }
}

static void push(Heap *h, It x) {
    if (h->n < h->k) {
        int i = h->n++;
        h->h[i] = x;
        while (i > 0 && better(h->h[(i - 1) / 2], h->h[i])) {
            It t = h->h[i];
            h->h[i] = h->h[(i - 1) / 2];
            h->h[(i - 1) / 2] = t;
            i = (i - 1) / 2;
        }
    } else if (better(x, h->h[0])) {
        h->h[0] = x;
        sift_down(h, 0);
    }
}

typedef struct {
    const It *items;
    Heap heap[8];
    int k;
} Tk;

static void worker(void *ctx, int tid, int nt) {
    Tk *t = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    t->heap[tid].n = 0;
    t->heap[tid].k = t->k;
    for (int i = lo; i < hi; i++)
        push(&t->heap[tid], t->items[i]);
}

static int cmp_best_first(const void *x, const void *y) {
    const It *a = x, *b = y;
    if (better(*a, *b))
        return -1;
    if (better(*b, *a))
        return 1;
    return 0;
}

int main(void) {
    static It items[N], sorted[N];
    uint64_t seed = 60;
    for (int i = 0; i < N; i++) {
        items[i].score = (int)(sm64(&seed) % 1000u); /* many duplicate scores */
        items[i].id = i;
        sorted[i] = items[i];
    }
    qsort(sorted, N, sizeof(It), cmp_best_first);
    int ks[] = {1, 5, 10, 32, 64};
    for (int q = 0; q < 5; q++) {
        int k = ks[q];
        Tk t;
        memset(&t, 0, sizeof t);
        t.items = items;
        t.k = k;
        par_run(NT, worker, &t);
        Heap m;
        m.n = 0;
        m.k = k;
        for (int th = 0; th < NT; th++)
            for (int i = 0; i < t.heap[th].n; i++)
                push(&m, t.heap[th].h[i]);
        qsort(m.h, (size_t)m.n, sizeof(It), cmp_best_first);
        check(m.n == k, "k results");
        for (int i = 0; i < k; i++)
            check(m.h[i].score == sorted[i].score && m.h[i].id == sorted[i].id, "top-k equals full sort");
        printf("k=%2d: best (%d,#%d) kth (%d,#%d)\n", k, m.h[0].score, m.h[0].id, m.h[k - 1].score,
               m.h[k - 1].id);
    }
    /* k = 5 printed in full to show tie-breaking by lowest id */
    printf("top 5 of full sort:");
    for (int i = 0; i < 5; i++)
        printf(" (%d,#%d)", sorted[i].score, sorted[i].id);
    printf("\n");
    return 0;
}
