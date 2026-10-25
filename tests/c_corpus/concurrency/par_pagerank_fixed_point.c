/*
 * title: Parallel PageRank in fixed point with fixed iterations
 * topic: concurrency
 * covers: power iteration, Q16 fixed point, pull-style rows per thread, dangling node redistribution, rank ordering
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

enum { V = 120, MAXDEG = 6, ITERS = 25, NT = 6 };
#define ONE (1L << 16)

typedef struct {
    int deg[V];
    int adj[V][MAXDEG];
    int indeg[V];
    int in[V][V]; /* in-lists, at most V */
} G;

typedef struct {
    const G *g;
    const long *rank;
    long *next;
    long base; /* teleport + dangling share per vertex */
} It;

static void it_worker(void *ctx, int tid, int nt) {
    It *it = ctx;
    int lo, hi;
    par_range(V, tid, nt, &lo, &hi);
    for (int v = lo; v < hi; v++) {
        long acc = 0;
        for (int k = 0; k < it->g->indeg[v]; k++) {
            int u = it->g->in[v][k];
            acc += it->rank[u] / it->g->deg[u];
        }
        it->next[v] = it->base + (acc * 85) / 100;
    }
}

/* Same arithmetic, single thread, for the reference answer */
static void seq_iter(const G *g, const long *rank, long *next, long base) {
    for (int v = 0; v < V; v++) {
        long acc = 0;
        for (int k = 0; k < g->indeg[v]; k++) {
            int u = g->in[v][k];
            acc += rank[u] / g->deg[u];
        }
        next[v] = base + (acc * 85) / 100;
    }
}

int main(void) {
    static G g;
    uint64_t seed = 8675309;
    for (int u = 0; u < V; u++) {
        int d = (int)(sm64(&seed) % (MAXDEG + 1));
        if (u % 17 == 3)
            d = 0; /* dangling */
        g.deg[u] = d;
        for (int k = 0; k < d; k++) {
            /* preferential-ish: bias toward low ids */
            uint64_t r = sm64(&seed);
            int t = (int)(r % V);
            if (r >> 60 & 1u)
                t = (int)((r >> 8) % 12u);
            g.adj[u][k] = t;
            g.in[t][g.indeg[t]++] = u;
        }
    }
    long a[V], b[V], sa[V], sb[V];
    for (int i = 0; i < V; i++)
        a[i] = sa[i] = ONE / V;
    long *cur = a, *nxt = b, *scur = sa, *snxt = sb;
    for (int iter = 0; iter < ITERS; iter++) {
        long dangling = 0;
        for (int u = 0; u < V; u++)
            if (g.deg[u] == 0)
                dangling += cur[u];
        long base = (ONE * 15 / 100) / V + (dangling * 85 / 100) / V;
        It it = {&g, cur, nxt, base};
        par_run(NT, it_worker, &it);
        long sdangling = 0;
        for (int u = 0; u < V; u++)
            if (g.deg[u] == 0)
                sdangling += scur[u];
        long sbase = (ONE * 15 / 100) / V + (sdangling * 85 / 100) / V;
        seq_iter(&g, scur, snxt, sbase);
        long *t = cur;
        cur = nxt;
        nxt = t;
        t = scur;
        scur = snxt;
        snxt = t;
        check(memcmp(cur, scur, sizeof a) == 0, "parallel equals sequential");
    }
    long total = 0;
    for (int v = 0; v < V; v++)
        total += cur[v];
    /* fixed-point truncation loses a little mass; it must stay close to 1.0 */
    check(total > ONE * 97 / 100 && total <= ONE, "mass roughly conserved");
    /* top five ranks, ties broken by id */
    int order[V];
    for (int i = 0; i < V; i++)
        order[i] = i;
    for (int i = 1; i < V; i++) {
        int x = order[i], j = i - 1;
        while (j >= 0 && (cur[order[j]] < cur[x])) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = x;
    }
    printf("total mass (Q16)=%ld\n", total);
    for (int i = 0; i < 5; i++)
        printf("rank %d: vertex %3d score %ld indeg %d\n", i + 1, order[i], cur[order[i]],
               g.indeg[order[i]]);
    return 0;
}
