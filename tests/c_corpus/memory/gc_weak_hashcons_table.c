/*
 * title: Hash-consing with a weak intern table
 * topic: memory
 * covers: maximal sharing of immutable pairs, intern table with chained buckets living inside the cells, weak table entries removed at sweep, pointer equality versus structural equality checked through independent serialization, temp-root stack while building, exact survivor oracle
 * deps: libc
 */
#define SEED 0x5EED0023ULL
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


static long g_checks;

#define CAP 260
#define NB 64
#define NR 12
#define LEAF(n) (-((n) + 1))
#define IS_LEAF(v) ((v) < 0)

typedef struct {
    int a, b;    /* leaf (negative) or cell index */
    int hnext;   /* bucket chain */
    unsigned char used, mark;
} Cell;

static Cell heap[CAP];
static int bucket[NB];
static int freelist;
static int tmp[128], ntmp;
static int rootv[NR];
static int ncoll, lookups_hit, lookups_miss, swept, table_removed, cells_max;

static unsigned hash2(int a, int b) {
    unsigned h = (unsigned)(a + 7) * 2654435761u;
    h ^= (unsigned)(b + 13) * 40503u + (h >> 15);
    return h % NB;
}

static void mark(int v) {
    while (v >= 0 && !heap[v].mark) {
        heap[v].mark = 1;
        mark(heap[v].a);
        v = heap[v].b;
    }
}

static void gc(void) {
    /* oracle: reachability by repeated passes over all cells */
    unsigned char reach[CAP];
    memset(reach, 0, sizeof reach);
    for (int i = 0; i < ntmp; i++)
        if (tmp[i] >= 0)
            reach[tmp[i]] = 1;
    for (int i = 0; i < NR; i++)
        if (rootv[i] >= 0)
            reach[rootv[i]] = 1;
    for (int ch = 1; ch;) {
        ch = 0;
        for (int c = 0; c < CAP; c++)
            if (reach[c] && heap[c].used) {
                if (heap[c].a >= 0 && !reach[heap[c].a]) {
                    reach[heap[c].a] = 1;
                    ch = 1;
                }
                if (heap[c].b >= 0 && !reach[heap[c].b]) {
                    reach[heap[c].b] = 1;
                    ch = 1;
                }
            }
    }
    for (int i = 0; i < ntmp; i++)
        mark(tmp[i]);
    for (int i = 0; i < NR; i++)
        mark(rootv[i]);
    /* weak table: unlink dead cells from their chains before they are freed */
    for (int b = 0; b < NB; b++) {
        int *link = &bucket[b];
        while (*link >= 0) {
            int c = *link;
            if (!heap[c].mark) {
                *link = heap[c].hnext;
                table_removed++;
            } else {
                link = &heap[c].hnext;
            }
        }
    }
    freelist = -1;
    int live = 0;
    for (int c = CAP - 1; c >= 0; c--) {
        if (heap[c].used && !heap[c].mark) {
            heap[c].used = 0;
            swept++;
        }
        CHECK((heap[c].used != 0) == (reach[c] != 0));
        live += heap[c].used;
        heap[c].mark = 0;
        if (!heap[c].used) {
            heap[c].a = freelist;
            freelist = c;
        }
    }
    if (live > cells_max)
        cells_max = live;
    ncoll++;
    g_checks++;
}

static int mk(int a, int b) {
    unsigned h = hash2(a, b);
    for (int c = bucket[h]; c >= 0; c = heap[c].hnext)
        if (heap[c].a == a && heap[c].b == b) {
            lookups_hit++;
            return c;
        }
    lookups_miss++;
    tmp[ntmp++] = a;
    tmp[ntmp++] = b;
    if (freelist < 0)
        gc();
    ntmp -= 2;
    CHECK(freelist >= 0);
    int c = freelist;
    freelist = heap[c].a;
    heap[c].used = 1;
    heap[c].a = a;
    heap[c].b = b;
    heap[c].hnext = bucket[h];
    bucket[h] = c;
    return c;
}

/* ---- a plain tree describing a term, independent of the heap ---- */
typedef struct {
    int leaf; /* >= 0 for a leaf */
    int l, r; /* child node indices */
} T;
static T tree[64];
static int ntree;

static int gen(uint64_t *st, int depth) {
    *st = *st * 6364136223846793005ULL + 1442695040888963407ULL;
    unsigned pick = (unsigned)(*st >> 33) % 10;
    int me = ntree++;
    if (depth == 0 || pick < 3) {
        tree[me].leaf = (int)((*st >> 40) % 3);
        return me;
    }
    tree[me].leaf = -1;
    int l = gen(st, depth - 1);
    int r = gen(st, depth - 1);
    tree[me].l = l;
    tree[me].r = r;
    return me;
}

static int build(int node) {
    if (tree[node].leaf >= 0)
        return LEAF(tree[node].leaf);
    int l = build(tree[node].l);
    tmp[ntmp++] = l;
    int r = build(tree[node].r);
    tmp[ntmp++] = r;
    int c = mk(l, r);
    ntmp -= 2;
    return c;
}

static int serialize(int node, char *out, int pos) {
    if (tree[node].leaf >= 0) {
        out[pos++] = (char)('0' + tree[node].leaf);
        return pos;
    }
    out[pos++] = '(';
    pos = serialize(tree[node].l, out, pos);
    out[pos++] = ' ';
    pos = serialize(tree[node].r, out, pos);
    out[pos++] = ')';
    return pos;
}

static int term_for(int seed, char *sig) {
    uint64_t st = 0x9E3779B97F4A7C15ULL * (uint64_t)(seed + 1);
    ntree = 0;
    int root = gen(&st, 5);
    int n = serialize(root, sig, 0);
    sig[n] = 0;
    return build(root);
}

int main(void) {
    for (int i = 0; i < NB; i++)
        bucket[i] = -1;
    for (int i = CAP - 1; i >= 0; i--) {
        heap[i].a = freelist;
        freelist = i;
    }
    for (int i = 0; i < NR; i++)
        rootv[i] = -1;
    static char sigs[NR][256];
    int builds = 0, identity_checks = 0, distinct_pairs_checked = 0, tree_nodes_total = 0;
    for (int step = 0; step < 900; step++) {
        int r = (int)rnd_n(NR);
        if (rnd_n(7) == 0) {
            rootv[r] = -1;
            continue;
        }
        int seed = (int)rnd_n(24);
        char sig[256];
        int t = term_for(seed, sig);
        tree_nodes_total += ntree;
        builds++;
        rootv[r] = t;
        strcpy(sigs[r], sig);
        for (int o = 0; o < NR; o++) {
            if (o == r || rootv[o] < 0 || IS_LEAF(t) || IS_LEAF(rootv[o]))
                continue;
            int same_text = strcmp(sigs[o], sig) == 0;
            CHECK(same_text == (rootv[o] == t)); /* pointer equality is exactly structural equality */
            identity_checks++;
            distinct_pairs_checked += !same_text;
        }
        if (step % 200 == 199)
            gc();
    }
    gc();
    int live = 0;
    for (int i = 0; i < CAP; i++)
        live += heap[i].used;
    int table_n = 0;
    for (int b = 0; b < NB; b++)
        for (int c = bucket[b]; c >= 0; c = heap[c].hnext)
            table_n++;
    CHECK(table_n == live);
    printf("terms built: %d, tree nodes visited: %d\n", builds, tree_nodes_total);
    printf("intern lookups: %d shared, %d created\n", lookups_hit, lookups_miss);
    printf("identity checks: %d equal-or-not comparisons (%d distinct pairs)\n", identity_checks, distinct_pairs_checked);
    printf("collections: %d, cells swept: %d, removed from weak table: %d\n", ncoll, swept, table_removed);
    printf("peak live cells: %d, live at end: %d, table entries: %d\n", cells_max, live, table_n);
    printf("oracle checks: %ld\n", g_checks);
    return 0;
}
