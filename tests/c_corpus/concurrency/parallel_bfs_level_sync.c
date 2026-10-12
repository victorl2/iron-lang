/*
 * title: Level-synchronous parallel BFS with atomic claiming
 * topic: concurrency
 * covers: frontier partitioning, atomic compare-exchange visited claim, thread-local next buffers, level barriers by join
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

enum { V = 4000, T = 4, MAXE = V * 6 };

static int off[V + 1], adj[MAXE];
static atomic_int dist[V];
static int parent[V];

typedef struct {
    int *front;
    int lo, hi;
    int level;
    int *out;
    int nout;
} Job;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void *expand(void *arg) {
    Job *j = arg;
    for (int i = j->lo; i < j->hi; i++) {
        int u = j->front[i];
        for (int e = off[u]; e < off[u + 1]; e++) {
            int v = adj[e];
            int expected = -1;
            if (atomic_compare_exchange_strong(&dist[v], &expected, j->level + 1)) {
                parent[v] = u; /* only the claiming thread writes parent[v] */
                j->out[j->nout++] = v;
            }
        }
    }
    return NULL;
}

int main(void) {
    unsigned s = 271828u;
    int ne = 0;
    for (int u = 0; u < V; u++) {
        off[u] = ne;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        int deg = (u != 0 && u % 11 == 0) ? 0 : (int)(s % 5u) + 1;
        for (int k = 0; k < deg; k++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            /* mostly local edges with occasional long jumps, so the level structure is interesting */
            int v = (s % 7u == 0) ? (int)(s / 7u % V) : (u + 1 + (int)(s % 40u)) % V;
            adj[ne++] = v;
        }
    }
    off[V] = ne;
    check(ne <= MAXE, "edge capacity");

    /* sequential reference */
    static int rdist[V], queue[V];
    for (int i = 0; i < V; i++)
        rdist[i] = -1;
    int qh = 0, qt = 0;
    rdist[0] = 0;
    queue[qt++] = 0;
    while (qh < qt) {
        int u = queue[qh++];
        for (int e = off[u]; e < off[u + 1]; e++)
            if (rdist[adj[e]] < 0) {
                rdist[adj[e]] = rdist[u] + 1;
                queue[qt++] = adj[e];
            }
    }

    for (int i = 0; i < V; i++)
        atomic_init(&dist[i], -1);
    atomic_store(&dist[0], 0);
    parent[0] = -1;
    static int frontier[V], next[V];
    static int bufs[T][V];
    int fn = 1, level = 0;
    frontier[0] = 0;
    int level_sizes[V], nlevels = 0;
    while (fn > 0) {
        level_sizes[nlevels++] = fn;
        Job jobs[T];
        pthread_t th[T];
        for (int t = 0; t < T; t++) {
            jobs[t].front = frontier;
            jobs[t].lo = (int)((long)fn * t / T);
            jobs[t].hi = (int)((long)fn * (t + 1) / T);
            jobs[t].level = level;
            jobs[t].out = bufs[t];
            jobs[t].nout = 0;
            check(pthread_create(&th[t], NULL, expand, &jobs[t]) == 0, "create");
        }
        for (int t = 0; t < T; t++)
            pthread_join(th[t], NULL);
        int nn = 0;
        for (int t = 0; t < T; t++)
            for (int i = 0; i < jobs[t].nout; i++)
                next[nn++] = bufs[t][i];
        for (int i = 0; i < nn; i++)
            frontier[i] = next[i];
        fn = nn;
        level++;
    }

    int reach = 0, maxd = 0;
    long dsum = 0;
    for (int v = 0; v < V; v++) {
        int d = atomic_load(&dist[v]);
        check(d == rdist[v], "distance equals sequential BFS");
        if (d >= 0) {
            reach++;
            dsum += d;
            if (d > maxd)
                maxd = d;
            if (v != 0) {
                int p = parent[v], found = 0;
                check(atomic_load(&dist[p]) == d - 1, "parent one level up");
                for (int e = off[p]; e < off[p + 1]; e++)
                    if (adj[e] == v)
                        found = 1;
                check(found, "parent edge exists");
            }
        }
    }
    printf("vertices %d edges %d\n", V, ne);
    printf("reachable %d max distance %d distance sum %ld\n", reach, maxd, dsum);
    printf("level sizes:");
    for (int l = 0; l < nlevels && l < 14; l++)
        printf(" %d", level_sizes[l]);
    printf("%s\n", nlevels > 14 ? " ..." : "");
    printf("levels %d\n", nlevels);
    return 0;
}
