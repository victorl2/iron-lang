/*
 * title: Parallel Floyd-Warshall with row bands per pivot
 * topic: concurrency
 * covers: all-pairs shortest paths, pivot rounds, infinity handling, negative-free weights, Dijkstra cross-check
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

enum { N = 60, NT = 6 };
#define INF 1000000000

typedef struct {
    int *d;
    int k;
} Piv;

static void piv_worker(void *ctx, int tid, int nt) {
    Piv *p = ctx;
    int lo, hi;
    par_range(N, tid, nt, &lo, &hi);
    for (int i = lo; i < hi; i++) {
        int dik = p->d[i * N + p->k];
        if (dik >= INF)
            continue;
        for (int j = 0; j < N; j++) {
            int dkj = p->d[p->k * N + j];
            if (dkj < INF && dik + dkj < p->d[i * N + j])
                p->d[i * N + j] = dik + dkj;
        }
    }
}

static void dijkstra(const int *w, int src, int *dist) {
    int done[N];
    for (int i = 0; i < N; i++) {
        dist[i] = INF;
        done[i] = 0;
    }
    dist[src] = 0;
    for (int it = 0; it < N; it++) {
        int u = -1;
        for (int i = 0; i < N; i++)
            if (!done[i] && (u < 0 || dist[i] < dist[u]))
                u = i;
        if (u < 0 || dist[u] >= INF)
            break;
        done[u] = 1;
        for (int v = 0; v < N; v++)
            if (w[u * N + v] < INF && dist[u] + w[u * N + v] < dist[v])
                dist[v] = dist[u] + w[u * N + v];
    }
}

int main(void) {
    static int w[N * N], d[N * N];
    uint64_t seed = 60606;
    for (int density = 0; density < 3; density++) {
        int pct = density == 0 ? 4 : density == 1 ? 10 : 35;
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                uint64_t r = sm64(&seed);
                if (i == j)
                    w[i * N + j] = 0;
                else if ((int)(r % 100u) < pct)
                    w[i * N + j] = 1 + (int)((r >> 20) % 50u);
                else
                    w[i * N + j] = INF;
            }
        memcpy(d, w, sizeof d);
        for (int k = 0; k < N; k++) {
            Piv p = {d, k};
            par_run(NT, piv_worker, &p);
        }
        long sum = 0;
        int reach = 0, diam = 0;
        for (int s = 0; s < N; s++) {
            int dist[N];
            dijkstra(w, s, dist);
            for (int t = 0; t < N; t++) {
                check(dist[t] == d[s * N + t], "matches dijkstra");
                if (dist[t] < INF) {
                    reach++;
                    sum += dist[t];
                    if (dist[t] > diam)
                        diam = dist[t];
                }
            }
        }
        printf("density %d%%: reachable pairs=%d sum=%ld diameter=%d d[0][%d]=%d\n", pct, reach, sum,
               diam, N - 1, d[N - 1] >= INF ? -1 : d[N - 1]);
    }
    return 0;
}
