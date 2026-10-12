/*
 * title: Warshall transitive closure on bit rows
 * topic: algorithms
 * covers: transitive closure, bitset rows, 64-bit words, DFS reachability cross-check, reduction of a chain
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

enum { N = 150, WORDS = (N + 63) / 64 };

static unsigned st = 1122334u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void check(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", m);
        exit(1);
    }
}

typedef struct {
    uint64_t w[WORDS];
} Row;

static Row rows[N];
static unsigned char adjm[N][N];

static int get(const Row *r, int j) { return (int)(r->w[j >> 6] >> (j & 63) & 1u); }
static void set(Row *r, int j) { r->w[j >> 6] |= (uint64_t)1 << (j & 63); }
static int popcount64(uint64_t x) {
    int c = 0;
    while (x) x &= x - 1, c++;
    return c;
}

static int seen[N];
static void dfs(int u) {
    seen[u] = 1;
    for (int v = 0; v < N; v++)
        if (adjm[u][v] && !seen[v]) dfs(v);
}

int main(void) {
    int edges = 0;
    for (int u = 0; u < N; u++)
        for (int v = 0; v < N; v++)
            if (rnd() % 1000 < 12) adjm[u][v] = 1, edges++;
    for (int u = 0; u < N; u++) {
        memset(&rows[u], 0, sizeof(Row));
        for (int v = 0; v < N; v++)
            if (adjm[u][v]) set(&rows[u], v);
    }
    long word_ops = 0;
    for (int k = 0; k < N; k++)
        for (int i = 0; i < N; i++)
            if (get(&rows[i], k))
                for (int x = 0; x < WORDS; x++) rows[i].w[x] |= rows[k].w[x], word_ops++;
    long pairs = 0;
    int maxout = 0, maxv = 0;
    for (int u = 0; u < N; u++) {
        memset(seen, 0, sizeof seen);
        /* reachable in >= 1 step: start from successors */
        for (int v = 0; v < N; v++)
            if (adjm[u][v] && !seen[v]) dfs(v);
        int cnt = 0;
        for (int v = 0; v < N; v++) {
            check(seen[v] == get(&rows[u], v), "closure matches dfs");
            cnt += seen[v];
        }
        int pc = 0;
        for (int x = 0; x < WORDS; x++) pc += popcount64(rows[u].w[x]);
        check(pc == cnt, "popcount matches");
        pairs += cnt;
        if (cnt > maxout) maxout = cnt, maxv = u;
    }
    int cyc = 0;
    for (int u = 0; u < N; u++) cyc += get(&rows[u], u);
    printf("edges %d, closure pairs %ld\n", edges, pairs);
    printf("vertices on cycles: %d\n", cyc);
    printf("largest reach: vertex %d reaches %d\n", maxv, maxout);
    printf("word operations: %ld\n", word_ops);
    /* a pure chain closes to a triangle */
    memset(rows, 0, sizeof rows);
    for (int i = 0; i + 1 < 70; i++) set(&rows[i], i + 1);
    for (int k = 0; k < 70; k++)
        for (int i = 0; i < 70; i++)
            if (get(&rows[i], k))
                for (int x = 0; x < WORDS; x++) rows[i].w[x] |= rows[k].w[x];
    long chain = 0;
    for (int i = 0; i < 70; i++)
        for (int j = 0; j < 70; j++) {
            check(get(&rows[i], j) == (j > i), "chain closure is strict order");
            chain += get(&rows[i], j);
        }
    printf("chain of 70 closes to %ld pairs\n", chain);
    return 0;
}
