/*
 * title: Circular bucket queue (Dial) with intrusive lists
 * topic: data_structures
 * covers: bucket queue, Dial's algorithm, circular buckets, intrusive doubly linked lists, decrease-key, monotone window
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 2718281828ull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 29);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

enum { N = 400, C = 9, NB = C + 1, INF = 1 << 30 };

/* bucket queue over ids 0..N-1 with keys in [cur, cur + C] */
typedef struct {
    int head[NB], next[N], prev[N], key[N], in[N];
    int cur, count;
    long scans, decreases;
} BQ;

static void bq_init(BQ *q) {
    for (int i = 0; i < NB; i++)
        q->head[i] = -1;
    for (int i = 0; i < N; i++)
        q->in[i] = 0;
    q->cur = 0;
    q->count = 0;
    q->scans = q->decreases = 0;
}

static void bq_link(BQ *q, int id, int key) {
    check(key >= q->cur && key <= q->cur + C, "key inside the active window");
    int b = key % NB;
    q->key[id] = key;
    q->prev[id] = -1;
    q->next[id] = q->head[b];
    if (q->head[b] >= 0)
        q->prev[q->head[b]] = id;
    q->head[b] = id;
}

static void bq_unlink(BQ *q, int id) {
    int b = q->key[id] % NB;
    if (q->prev[id] >= 0)
        q->next[q->prev[id]] = q->next[id];
    else
        q->head[b] = q->next[id];
    if (q->next[id] >= 0)
        q->prev[q->next[id]] = q->prev[id];
}

static void bq_insert(BQ *q, int id, int key) {
    check(!q->in[id], "not already inside");
    bq_link(q, id, key);
    q->in[id] = 1;
    q->count++;
}

static void bq_decrease(BQ *q, int id, int key) {
    check(q->in[id] && key <= q->key[id], "decrease inside");
    bq_unlink(q, id);
    bq_link(q, id, key);
    q->decreases++;
}

static int bq_pop(BQ *q, int *key) {
    check(q->count > 0, "non-empty");
    while (q->head[q->cur % NB] < 0) {
        q->cur++;
        q->scans++;
    }
    int id = q->head[q->cur % NB];
    bq_unlink(q, id);
    q->in[id] = 0;
    q->count--;
    *key = q->key[id];
    return id;
}

typedef struct {
    int to, w;
} Edge;

int main(void) {
    static Edge adj[N][8];
    static int deg[N];
    for (int u = 0; u < N; u++) {
        int d = 2 + (int)(rng() % 5);
        for (int k = 0; k < d; k++) {
            adj[u][deg[u]].to = (int)(rng() % N);
            adj[u][deg[u]].w = 1 + (int)(rng() % C);
            deg[u]++;
        }
        /* a backbone edge keeps the graph mostly reachable */
        if (deg[u] < 8) {
            adj[u][deg[u]].to = (u + 1) % N;
            adj[u][deg[u]].w = C;
            deg[u]++;
        }
    }
    static int dist[N], ref[N];
    BQ q;
    bq_init(&q);
    for (int i = 0; i < N; i++)
        dist[i] = INF;
    dist[0] = 0;
    bq_insert(&q, 0, 0);
    int settled = 0;
    while (q.count > 0) {
        int d, u = bq_pop(&q, &d);
        check(d == dist[u], "popped key is the distance");
        settled++;
        for (int k = 0; k < deg[u]; k++) {
            int v = adj[u][k].to, nd = d + adj[u][k].w;
            if (nd < dist[v]) {
                dist[v] = nd;
                if (q.in[v])
                    bq_decrease(&q, v, nd);
                else
                    bq_insert(&q, v, nd);
            }
        }
    }
    /* Bellman-Ford reference */
    for (int i = 0; i < N; i++)
        ref[i] = INF;
    ref[0] = 0;
    for (int round = 0; round < N; round++) {
        int changed = 0;
        for (int u = 0; u < N; u++)
            if (ref[u] < INF)
                for (int k = 0; k < deg[u]; k++)
                    if (ref[u] + adj[u][k].w < ref[adj[u][k].to]) {
                        ref[adj[u][k].to] = ref[u] + adj[u][k].w;
                        changed = 1;
                    }
        if (!changed)
            break;
    }
    long sum = 0;
    int maxd = 0;
    for (int i = 0; i < N; i++) {
        check(dist[i] == ref[i], "Dial equals Bellman-Ford");
        if (dist[i] < INF) {
            sum += dist[i];
            if (dist[i] > maxd)
                maxd = dist[i];
        }
    }
    printf("settled=%d max_dist=%d dist_sum=%ld\n", settled, maxd, sum);
    printf("decreases=%ld empty_bucket_scans=%ld final_cur=%d\n", q.decreases, q.scans, q.cur);

    /* raw window workload against a model: keys always within [cur, cur+C] */
    bq_init(&q);
    int mk[N], mh[N] = {0};
    long popsum = 0;
    int pops = 0;
    for (int op = 0; op < 20000; op++) {
        int id = (int)(rng() % N);
        if (!mh[id]) {
            if (rng() % 3) {
                int k = q.cur + (int)(rng() % (C + 1));
                mk[id] = k;
                mh[id] = 1;
                bq_insert(&q, id, k);
            }
        } else if (rng() % 3 == 0 && mk[id] > q.cur) {
            int k = q.cur + (int)(rng() % (unsigned)(mk[id] - q.cur + 1));
            mk[id] = k;
            bq_decrease(&q, id, k);
        } else if (rng() % 4 == 0) {
            int mn = INF;
            for (int i = 0; i < N; i++)
                if (mh[i] && mk[i] < mn)
                    mn = mk[i];
            int k, top = bq_pop(&q, &k);
            check(k == mn && mh[top] && mk[top] == k, "window pop is a minimum");
            mh[top] = 0;
            popsum += k;
            pops++;
        }
    }
    printf("window pops=%d popsum=%ld remaining=%d cur=%d\n", pops, popsum, q.count, q.cur);
    return 0;
}
