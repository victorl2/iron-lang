/*
 * title: Parallel connected components by min-label propagation
 * topic: concurrency
 * covers: label propagation rounds, double buffered labels, edge partitioning, union-find cross-check, convergence rounds
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

enum { V = 400, E = 420, NT = 5 };

typedef struct {
    const int *eu, *ev;
    const int *cur;
    int *next;
    int changed[8];
} Lp;

static void copy_worker(void *ctx, int tid, int nt) {
    Lp *l = ctx;
    int lo, hi;
    par_range(V, tid, nt, &lo, &hi);
    for (int i = lo; i < hi; i++)
        l->next[i] = l->cur[i];
}

/* Edges are partitioned across threads. Two-phase per round: (1) copy
 * labels, (2) apply min via a compare-and-swap-free scheme: each thread owns
 * a vertex range and scans the whole edge list for edges touching it. */
static void relax_worker(void *ctx, int tid, int nt) {
    Lp *l = ctx;
    int lo, hi;
    par_range(V, tid, nt, &lo, &hi);
    int ch = 0;
    for (int e = 0; e < E; e++) {
        int u = l->eu[e], v = l->ev[e];
        if (u >= lo && u < hi && l->cur[v] < l->next[u]) {
            l->next[u] = l->cur[v];
            ch++;
        }
        if (v >= lo && v < hi && l->cur[u] < l->next[v]) {
            l->next[v] = l->cur[u];
            ch++;
        }
    }
    l->changed[tid] = ch;
}

static int uf_find(int *p, int x) {
    while (p[x] != x) {
        p[x] = p[p[x]];
        x = p[x];
    }
    return x;
}

int main(void) {
    int eu[E], ev[E];
    uint64_t seed = 13579;
    /* sparse random graph: many components (E is close to V so a few big ones) */
    for (int e = 0; e < E; e++) {
        eu[e] = (int)(sm64(&seed) % V);
        ev[e] = (int)(sm64(&seed) % V);
    }
    int la[V], lb[V];
    for (int i = 0; i < V; i++)
        la[i] = i;
    int *cur = la, *nxt = lb;
    int rounds = 0;
    for (;;) {
        Lp l;
        l.eu = eu;
        l.ev = ev;
        l.cur = cur;
        l.next = nxt;
        par_run(NT, copy_worker, &l);
        par_run(NT, relax_worker, &l);
        int ch = 0;
        for (int t = 0; t < NT; t++)
            ch += l.changed[t];
        rounds++;
        int *t = cur;
        cur = nxt;
        nxt = t;
        if (!ch)
            break;
        check(rounds < V, "converges");
    }
    /* reference: union-find, label = smallest vertex in the component */
    int p[V], minv[V];
    for (int i = 0; i < V; i++)
        p[i] = i;
    for (int e = 0; e < E; e++) {
        int a = uf_find(p, eu[e]), b = uf_find(p, ev[e]);
        if (a != b)
            p[a] = b;
    }
    for (int i = 0; i < V; i++)
        minv[i] = V;
    for (int i = 0; i < V; i++) {
        int r = uf_find(p, i);
        if (i < minv[r])
            minv[r] = i;
    }
    int comps = 0, largest = 0, singles = 0;
    int size[V];
    memset(size, 0, sizeof size);
    for (int i = 0; i < V; i++) {
        check(cur[i] == minv[uf_find(p, i)], "label equals min vertex of component");
        size[cur[i]]++;
    }
    for (int i = 0; i < V; i++)
        if (size[i]) {
            comps++;
            if (size[i] > largest)
                largest = size[i];
            if (size[i] == 1)
                singles++;
        }
    printf("vertices=%d edges=%d rounds=%d components=%d largest=%d singletons=%d\n", V, E, rounds,
           comps, largest, singles);
    return 0;
}
