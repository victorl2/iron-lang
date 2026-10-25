/*
 * title: Parallel triangle counting on sorted adjacency lists
 * topic: concurrency
 * covers: orientation by degree, sorted list intersection, per-thread counters, brute force cube check
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

static inline uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

enum { V = 150, E = 1400, NT = 6 };

typedef struct {
    int deg[V];
    int adj[V][V]; /* oriented adjacency, sorted */
    unsigned char m[V][V];
} G;

typedef struct {
    const G *g;
    long tri[8];
    long work[8];
} Tc;

/* Count common elements of two sorted lists */
static long intersect(const int *a, int na, const int *b, int nb, long *work) {
    long c = 0;
    int i = 0, j = 0;
    while (i < na && j < nb) {
        (*work)++;
        if (a[i] < b[j])
            i++;
        else if (a[i] > b[j])
            j++;
        else {
            c++;
            i++;
            j++;
        }
    }
    return c;
}

/* Vertices are assigned cyclically (hubs are spread out); every triangle is
 * counted once at its lowest-ranked vertex thanks to the orientation. */
static void tc_worker(void *ctx, int tid, int nt) {
    Tc *t = ctx;
    long tri = 0, work = 0;
    for (int u = tid; u < V; u += nt)
        for (int k = 0; k < t->g->deg[u]; k++) {
            int v = t->g->adj[u][k];
            tri += intersect(t->g->adj[u], t->g->deg[u], t->g->adj[v], t->g->deg[v], &work);
        }
    t->tri[tid] = tri;
    t->work[tid] = work;
}

int main(void) {
    static G g;
    static unsigned char und[V][V];
    static int udeg[V];
    uint64_t seed = 3141;
    int edges = 0;
    while (edges < E) {
        uint64_t r = sm64(&seed);
        int a = (int)(r % V), b = (int)((r >> 20) % V);
        /* hubs: bias one endpoint toward the first 10 vertices */
        if ((r >> 44 & 3u) == 0)
            a = (int)((r >> 32) % 10u);
        if (a == b || und[a][b])
            continue;
        und[a][b] = und[b][a] = 1;
        udeg[a]++;
        udeg[b]++;
        edges++;
    }
    /* rank: (degree, id); orient each edge from lower to higher rank */
    for (int u = 0; u < V; u++)
        for (int v = 0; v < V; v++)
            if (und[u][v] && (udeg[u] < udeg[v] || (udeg[u] == udeg[v] && u < v))) {
                g.adj[u][g.deg[u]++] = v;
            }
    /* sort each oriented list ascending by vertex id (insertion sort) */
    for (int u = 0; u < V; u++)
        for (int i = 1; i < g.deg[u]; i++) {
            int x = g.adj[u][i], j = i - 1;
            while (j >= 0 && g.adj[u][j] > x) {
                g.adj[u][j + 1] = g.adj[u][j];
                j--;
            }
            g.adj[u][j + 1] = x;
        }
    long ref = 0;
    for (int a = 0; a < V; a++)
        for (int b = a + 1; b < V; b++)
            if (und[a][b])
                for (int c = b + 1; c < V; c++)
                    if (und[a][c] && und[b][c])
                        ref++;
    int maxdeg = 0, maxv = 0;
    for (int u = 0; u < V; u++)
        if (udeg[u] > maxdeg) {
            maxdeg = udeg[u];
            maxv = u;
        }
    printf("vertices=%d edges=%d max degree %d at vertex %d\n", V, E, maxdeg, maxv);
    for (int nt = 1; nt <= 8; nt++) {
        Tc t;
        memset(&t, 0, sizeof t);
        t.g = &g;
        par_run(nt, tc_worker, &t);
        long tri = 0, work = 0;
        for (int k = 0; k < nt; k++) {
            tri += t.tri[k];
            work += t.work[k];
        }
        check(tri == ref, "parallel triangles equal brute force");
        if (nt == 1 || nt == 6)
            printf("threads=%d triangles=%ld merge steps=%ld\n", nt, tri, work);
    }
    return 0;
}
