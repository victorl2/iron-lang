/*
 * title: Parallel Bellman-Ford with negative edges and cycle detection
 * topic: concurrency
 * covers: edge relaxation rounds, per-thread candidate arrays merged in fixed order, negative cycle detection
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

enum { V = 80, E = 500, NT = 5 };
#define INF 0x3fffffff

typedef struct {
    const int *eu, *ev, *ew;
    const int *dist;
    int *cand; /* per-vertex candidate from each thread: cand[t][v] */
} Bf;

/* Jacobi-style round: threads read the frozen dist and write only their own
 * candidate row; the merge takes the minimum. Deterministic by design. */
static void relax_worker(void *ctx, int tid, int nt) {
    Bf *b = ctx;
    int lo, hi;
    par_range(E, tid, nt, &lo, &hi);
    int *row = b->cand + tid * V;
    for (int v = 0; v < V; v++)
        row[v] = b->dist[v];
    for (int e = lo; e < hi; e++) {
        int u = b->eu[e];
        if (b->dist[u] < INF && b->dist[u] + b->ew[e] < row[b->ev[e]])
            row[b->ev[e]] = b->dist[u] + b->ew[e];
    }
}

/* returns rounds used, or -1 on negative cycle */
static int par_bf(const int *eu, const int *ev, const int *ew, int src, int *dist) {
    static int cand[8 * V];
    for (int i = 0; i < V; i++)
        dist[i] = INF;
    dist[src] = 0;
    Bf b = {eu, ev, ew, dist, cand};
    for (int r = 1; r <= V; r++) {
        par_run(NT, relax_worker, &b);
        int changed = 0;
        for (int v = 0; v < V; v++) {
            int best = dist[v];
            for (int t = 0; t < NT; t++)
                if (cand[t * V + v] < best)
                    best = cand[t * V + v];
            if (best != dist[v]) {
                dist[v] = best;
                changed = 1;
            }
        }
        if (!changed)
            return r;
    }
    return -1;
}

static int seq_bf(const int *eu, const int *ev, const int *ew, int src, int *dist) {
    for (int i = 0; i < V; i++)
        dist[i] = INF;
    dist[src] = 0;
    for (int r = 0; r < V; r++) {
        int changed = 0;
        for (int e = 0; e < E; e++)
            if (dist[eu[e]] < INF && dist[eu[e]] + ew[e] < dist[ev[e]]) {
                dist[ev[e]] = dist[eu[e]] + ew[e];
                changed = 1;
            }
        if (!changed)
            return 0;
    }
    return -1;
}

int main(void) {
    int eu[E], ev[E], ew[E];
    uint64_t seed = 24680;
    /* potentials make negative edges safe: w' = w + p[u] - p[v] with w >= 0 */
    int pot[V];
    for (int i = 0; i < V; i++)
        pot[i] = (int)(sm64(&seed) % 40u);
    for (int e = 0; e < E; e++) {
        eu[e] = (int)(sm64(&seed) % V);
        ev[e] = (int)(sm64(&seed) % V);
        int w = (int)(sm64(&seed) % 30u);
        ew[e] = w + pot[eu[e]] - pot[ev[e]];
    }
    int dist[V], ref[V];
    int rounds = par_bf(eu, ev, ew, 0, dist);
    int rc = seq_bf(eu, ev, ew, 0, ref);
    check(rounds > 0 && rc == 0, "no negative cycle in potential graph");
    check(memcmp(dist, ref, sizeof dist) == 0, "parallel equals sequential");
    int reach = 0, neg = 0;
    long sum = 0;
    for (int i = 0; i < V; i++)
        if (dist[i] < INF) {
            reach++;
            sum += dist[i];
            if (dist[i] < 0)
                neg++;
        }
    printf("no cycle: rounds=%d reachable=%d negative=%d sum=%ld\n", rounds, reach, neg, sum);

    /* plant a negative cycle reachable from the source */
    eu[0] = 0;
    ev[0] = 1;
    ew[0] = 1;
    eu[1] = 1;
    ev[1] = 2;
    ew[1] = 1;
    eu[2] = 2;
    ev[2] = 0;
    ew[2] = -5;
    rounds = par_bf(eu, ev, ew, 0, dist);
    rc = seq_bf(eu, ev, ew, 0, ref);
    check(rounds == -1 && rc == -1, "negative cycle detected by both");
    printf("planted cycle 0->1->2->0 weight -3: detected\n");
    return 0;
}
