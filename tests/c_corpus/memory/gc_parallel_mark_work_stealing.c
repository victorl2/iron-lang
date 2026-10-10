/*
 * title: Simulated parallel marking with work-stealing deques
 * topic: memory
 * covers: per-worker deques (owner LIFO, thief FIFO), test-and-set mark bits, steal-one versus steal-half, deterministic round-based scheduling, termination detection, load balance and speedup counters, agreement with a sequential mark
 * deps: libc
 */
#define SEED 0x5EED001FULL
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*@RNG*/
/* Deterministic splitmix64 generator so every platform sees the same run. */
static uint64_t rng_s = SEED;
static inline uint32_t rnd(void) {
    uint64_t z = (rng_s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return (uint32_t)(z >> 32);
}
static inline uint32_t rnd_n(uint32_t n) {
    uint32_t r = rnd();
    return r % n;
}

/*@ENDRNG*/
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__);    \
            exit(1);                                                          \
        }                                                                     \
    } while (0)



#define N 2600
#define MAXW 8
#define DQ 4096

static int f0[N], f1[N];
static int nnodes;
static int roots[6], nroots;

typedef struct {
    int items[DQ];
    int head, tail; /* thieves take from head, the owner pushes and pops at tail */
} Deque;

static Deque dq[MAXW];
static unsigned char markbit[N];
static int scanned[MAXW];
static int rounds, steals, failed_steals, cas_lost, idle_rounds, stolen_items;

static int new_node(void) {
    CHECK(nnodes < N);
    f0[nnodes] = f1[nnodes] = -1;
    return nnodes++;
}

static void build_graph(void) {
    nnodes = 0;
    nroots = 0;
    int tree0 = nnodes; /* complete binary tree */
    for (int i = 0; i < 500; i++)
        new_node();
    for (int i = 0; i < 500; i++) {
        if (2 * i + 1 < 500)
            f0[tree0 + i] = tree0 + 2 * i + 1;
        if (2 * i + 2 < 500)
            f1[tree0 + i] = tree0 + 2 * i + 2;
    }
    roots[nroots++] = tree0;
    int chain0 = nnodes; /* a long list */
    for (int i = 0; i < 300; i++) {
        int n = new_node();
        if (i > 0)
            f0[n - 1] = n;
    }
    roots[nroots++] = chain0;
    int rnd0 = nnodes; /* random graph with sharing and cycles */
    for (int i = 0; i < 600; i++)
        new_node();
    for (int i = 1; i < 600; i++) { /* attach node i below a random earlier node with a free slot */
        int p = (int)rnd_n((unsigned)i);
        while (f0[rnd0 + p] >= 0 && f1[rnd0 + p] >= 0)
            p = (p + 1) % i;
        if (f0[rnd0 + p] < 0)
            f0[rnd0 + p] = rnd0 + i;
        else
            f1[rnd0 + p] = rnd0 + i;
    }
    for (int i = 0; i < 600; i++) { /* leaves sometimes point back into the graph: sharing and cycles */
        if (f0[rnd0 + i] < 0 && rnd_n(2) == 0)
            f0[rnd0 + i] = rnd0 + (int)rnd_n(600);
        if (f1[rnd0 + i] < 0 && rnd_n(2) == 0)
            f1[rnd0 + i] = rnd0 + (int)rnd_n(600);
    }
    roots[nroots++] = rnd0;
    int broom0 = nnodes; /* a stem of 50 with a bushy head */
    for (int i = 0; i < 250; i++)
        new_node();
    for (int i = 0; i < 49; i++)
        f0[broom0 + i] = broom0 + i + 1;
    for (int i = 50; i < 250; i++) {
        int parent = 49 + (i - 50) / 2;
        if (f0[broom0 + parent] < 0 || parent == 49)
            (i % 2 ? f1 : f0)[broom0 + parent] = broom0 + i;
        else
            f1[broom0 + parent] = broom0 + i;
    }
    roots[nroots++] = broom0;
    int junk0 = nnodes; /* unreachable nodes pointing into live ones */
    for (int i = 0; i < 700; i++)
        new_node();
    for (int i = 0; i < 700; i++) {
        f0[junk0 + i] = rnd_n(3) == 0 ? (int)rnd_n((unsigned)junk0) : junk0 + (int)rnd_n(700);
        f1[junk0 + i] = junk0 + (int)rnd_n(700);
    }
}

static int sequential_mark(unsigned char *m) {
    int stack[N], sp = 0, n = 0;
    memset(m, 0, N);
    for (int i = 0; i < nroots; i++)
        if (!m[roots[i]]) {
            m[roots[i]] = 1;
            stack[sp++] = roots[i];
        }
    while (sp > 0) {
        int o = stack[--sp];
        n++;
        int c[2] = {f0[o], f1[o]};
        for (int k = 0; k < 2; k++)
            if (c[k] >= 0 && !m[c[k]]) {
                m[c[k]] = 1;
                stack[sp++] = c[k];
            }
    }
    return n;
}

static int dq_size(int w) { return dq[w].tail - dq[w].head; }
static void dq_push(int w, int x) {
    CHECK(dq_size(w) < DQ);
    dq[w].items[dq[w].tail % DQ] = x;
    dq[w].tail++;
}
static int dq_pop(int w) { return dq[w].items[--dq[w].tail % DQ]; }
static int dq_steal(int w) { return dq[w].items[dq[w].head++ % DQ]; }

/* Try to claim an object for marking; false when another marker got there first. */
static int claim(int o) {
    if (markbit[o]) {
        cas_lost++;
        return 0;
    }
    markbit[o] = 1;
    return 1;
}

static void parallel_mark(int workers, int steal_half) {
    memset(markbit, 0, sizeof markbit);
    memset(scanned, 0, sizeof scanned);
    for (int w = 0; w < workers; w++)
        dq[w].head = dq[w].tail = 0;
    rounds = steals = failed_steals = cas_lost = idle_rounds = stolen_items = 0;
    for (int i = 0; i < nroots; i++)
        if (claim(roots[i]))
            dq_push(0, roots[i]); /* every root starts on worker 0 */
    for (;;) {
        int busy = 0;
        for (int w = 0; w < workers; w++)
            busy += dq_size(w) > 0;
        if (!busy)
            break;
        rounds++;
        unsigned char fresh[MAXW] = {0}; /* work just stolen this round cannot be stolen again until it has run */
        for (int w = 0; w < workers; w++) {
            if (dq_size(w) > 0) {
                int o = dq_pop(w);
                scanned[w]++;
                int c[2] = {f0[o], f1[o]};
                for (int k = 0; k < 2; k++)
                    if (c[k] >= 0 && claim(c[k]))
                        dq_push(w, c[k]);
            } else if (workers > 1) {
                int v = (w + 1 + (int)rnd_n((unsigned)workers - 1)) % workers;
                int avail = dq_size(v);
                if (avail == 0 || fresh[v]) {
                    failed_steals++;
                    idle_rounds++;
                    continue;
                }
                int take = steal_half ? (avail + 1) / 2 : 1;
                for (int i = 0; i < take; i++)
                    dq_push(w, dq_steal(v));
                steals++;
                stolen_items += take;
                fresh[w] = 1;
            } else {
                idle_rounds++;
            }
        }
    }
}

int main(void) {
    build_graph();
    unsigned char seq[N];
    int live = sequential_mark(seq);
    printf("nodes: %d, reachable: %d, garbage: %d\n", nnodes, live, nnodes - live);
    printf("workers policy   rounds speedup steals failed stolen  lost-claims  min/max scanned\n");
    static const int wcount[] = {1, 2, 4, 8};
    for (int wi = 0; wi < 4; wi++)
        for (int half = 0; half < 2; half++) {
            int workers = wcount[wi];
            if (workers == 1 && half)
                continue;
            rng_s = SEED;
            parallel_mark(workers, half);
            int total = 0, mn = N, mx = 0;
            for (int w = 0; w < workers; w++) {
                total += scanned[w];
                if (scanned[w] < mn)
                    mn = scanned[w];
                if (scanned[w] > mx)
                    mx = scanned[w];
            }
            CHECK(total == live);
            for (int i = 0; i < nnodes; i++)
                CHECK(markbit[i] == seq[i]);
            printf("%7d %-6s %8d %6d.%02d %6d %6d %6d %12d %8d/%d\n", workers, half ? "half" : "one", rounds,
                   total / rounds, (total * 100 / rounds) % 100, steals, failed_steals, stolen_items, cas_lost, mn,
                   mx);
        }
    int swept = 0;
    for (int i = 0; i < nnodes; i++)
        swept += !markbit[i];
    printf("sweep would free %d nodes\n", swept);
    return 0;
}
