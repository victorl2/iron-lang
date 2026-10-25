/*
 * title: Deadlock detection with a wait-for graph
 * topic: concurrency
 * covers: wait-for graph, cycle detection by DFS colouring, trylock probes, graph reduction cross-check
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAXT = 8, MAXL = 8 };

/* --- a scripted scenario: thread t holds some locks, then probes one more with trylock --- */
typedef struct {
    const char *name;
    int nthreads;
    int held[MAXT][3]; /* lock ids, -1 terminated */
    int want[MAXT];    /* lock id or -1 */
} Scenario;

static const Scenario SCEN[] = {
    {"ring of four", 4, {{0, -1}, {1, -1}, {2, -1}, {3, -1}}, {1, 2, 3, 0}},
    {"chain, no cycle", 4, {{0, -1}, {1, -1}, {2, -1}, {3, -1}}, {1, 2, 3, -1}},
    {"two cycles", 6, {{0, -1}, {1, -1}, {2, -1}, {3, -1}, {4, -1}, {5, -1}}, {1, 0, 3, 2, 0, 4}},
    {"tail into cycle", 5, {{0, 1, -1}, {2, -1}, {3, -1}, {4, -1}, {5, -1}}, {2, 3, 2, 0, 1}},
};

static pthread_mutex_t lock_mu[MAXL];
static pthread_mutex_t st_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t st_cv = PTHREAD_COND_INITIALIZER;
static int owner[MAXL];      /* -1 free; written under st_mu */
static int holding_done;     /* threads that finished acquiring */
static int probing_done;     /* threads that finished probing */
static int wait_edge[MAXT];  /* thread -> owner thread, -1 none */
static const Scenario *cur;

typedef struct {
    int id;
} Arg;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static void *worker(void *p) {
    int t = ((Arg *)p)->id;
    for (int k = 0; cur->held[t][k] >= 0; k++) {
        int l = cur->held[t][k];
        pthread_mutex_lock(&lock_mu[l]);
        pthread_mutex_lock(&st_mu);
        owner[l] = t;
        pthread_mutex_unlock(&st_mu);
    }
    pthread_mutex_lock(&st_mu);
    holding_done++;
    pthread_cond_broadcast(&st_cv);
    while (holding_done < cur->nthreads)
        pthread_cond_wait(&st_cv, &st_mu);
    pthread_mutex_unlock(&st_mu);

    int w = cur->want[t];
    int got = 0;
    if (w >= 0) {
        got = pthread_mutex_trylock(&lock_mu[w]) == 0; /* would block forever in a real run */
    }
    pthread_mutex_lock(&st_mu);
    wait_edge[t] = -1;
    if (w >= 0 && !got)
        wait_edge[t] = owner[w];
    probing_done++;
    pthread_cond_broadcast(&st_cv);
    while (probing_done < cur->nthreads)
        pthread_cond_wait(&st_cv, &st_mu);
    pthread_mutex_unlock(&st_mu);

    if (got)
        pthread_mutex_unlock(&lock_mu[w]);
    for (int k = 0; cur->held[t][k] >= 0; k++)
        pthread_mutex_unlock(&lock_mu[cur->held[t][k]]);
    return NULL;
}

/* --- cycle detection on a general digraph (adjacency matrix) --- */
static int color[MAXT], parent_of[MAXT];
static int found_cycle[MAXT], found_len;

static int dfs(int n, int adj[MAXT][MAXT], int u) {
    color[u] = 1;
    for (int v = 0; v < n; v++) {
        if (!adj[u][v])
            continue;
        if (color[v] == 0) {
            parent_of[v] = u;
            if (dfs(n, adj, v))
                return 1;
        } else if (color[v] == 1) {
            int tmp[MAXT], len = 0;
            for (int x = u; x != v; x = parent_of[x])
                tmp[len++] = x;
            tmp[len++] = v;
            found_len = 0;
            for (int i = len - 1; i >= 0; i--)
                found_cycle[found_len++] = tmp[i];
            return 1;
        }
    }
    color[u] = 2;
    return 0;
}

static int has_cycle_dfs(int n, int adj[MAXT][MAXT]) {
    memset(color, 0, sizeof color);
    for (int u = 0; u < n; u++)
        if (color[u] == 0 && dfs(n, adj, u))
            return 1;
    return 0;
}

/* reduction: repeatedly delete nodes without outgoing edges; a cycle exists iff something remains */
static int has_cycle_reduce(int n, int adj[MAXT][MAXT], int *stuck_mask) {
    int alive[MAXT];
    for (int i = 0; i < n; i++)
        alive[i] = 1;
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int u = 0; u < n; u++) {
            if (!alive[u])
                continue;
            int out = 0;
            for (int v = 0; v < n; v++)
                if (alive[v] && adj[u][v])
                    out = 1;
            if (!out) {
                alive[u] = 0;
                changed = 1;
            }
        }
    }
    int mask = 0;
    for (int u = 0; u < n; u++)
        if (alive[u])
            mask |= 1 << u;
    *stuck_mask = mask;
    return mask != 0;
}

int main(void) {
    for (unsigned si = 0; si < sizeof SCEN / sizeof SCEN[0]; si++) {
        cur = &SCEN[si];
        for (int l = 0; l < MAXL; l++) {
            pthread_mutex_init(&lock_mu[l], NULL);
            owner[l] = -1;
        }
        holding_done = probing_done = 0;
        pthread_t th[MAXT];
        Arg args[MAXT];
        for (int t = 0; t < cur->nthreads; t++) {
            args[t].id = t;
            check(pthread_create(&th[t], NULL, worker, &args[t]) == 0, "create");
        }
        for (int t = 0; t < cur->nthreads; t++)
            pthread_join(th[t], NULL);
        for (int l = 0; l < MAXL; l++)
            pthread_mutex_destroy(&lock_mu[l]);

        int adj[MAXT][MAXT];
        memset(adj, 0, sizeof adj);
        printf("scenario '%s': edges", cur->name);
        for (int t = 0; t < cur->nthreads; t++)
            if (wait_edge[t] >= 0) {
                adj[t][wait_edge[t]] = 1;
                printf(" T%d->T%d", t, wait_edge[t]);
            }
        int mask;
        int c1 = has_cycle_dfs(cur->nthreads, adj);
        int c2 = has_cycle_reduce(cur->nthreads, adj, &mask);
        check(c1 == c2, "detectors agree");
        if (c1) {
            printf("\n  deadlock cycle:");
            for (int i = 0; i < found_len; i++)
                printf(" T%d", found_cycle[i]);
            printf(" (stuck or blocked set mask=0x%02x)\n", (unsigned)mask);
        } else {
            printf("\n  no deadlock\n");
        }
    }

    /* random digraphs: DFS colouring against reduction */
    unsigned s = 77u;
    int cyc = 0, acyc = 0;
    for (int g = 0; g < 400; g++) {
        int n = 3 + g % 6;
        int adj[MAXT][MAXT];
        memset(adj, 0, sizeof adj);
        int edges = g % 7 + 1;
        for (int e = 0; e < edges; e++) {
            s = s * 1664525u + 1013904223u;
            int u = (int)((s >> 8) % (unsigned)n);
            s = s * 1664525u + 1013904223u;
            int v = (int)((s >> 8) % (unsigned)n);
            if (u != v)
                adj[u][v] = 1;
        }
        int mask;
        int c1 = has_cycle_dfs(n, adj);
        int c2 = has_cycle_reduce(n, adj, &mask);
        check(c1 == c2, "random graph agreement");
        if (c1) {
            /* the reported cycle must really be a cycle */
            for (int i = 0; i < found_len; i++)
                check(adj[found_cycle[i]][found_cycle[(i + 1) % found_len]], "cycle edges exist");
            cyc++;
        } else {
            acyc++;
        }
    }
    printf("random graphs: %d with cycles, %d acyclic\n", cyc, acyc);
    return 0;
}
