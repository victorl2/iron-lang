/*
 * title: Bucketized cuckoo table with BFS displacement
 * topic: data_structures
 * covers: cuckoo hashing, 4-way buckets, breadth-first path search, load factor above 90 percent, alternate bucket
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNUSED __attribute__((unused))

static uint64_t rs = 0x9E3779B97F4A7C15ull;
static UNUSED uint64_t rnd(void) {
    uint64_t z = (rs += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static UNUSED void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}
static UNUSED uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static UNUSED uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Reference model: unordered array with linear scan. */
enum { REF_CAP = 1 << 14 };
static uint32_t ref_k[REF_CAP];
static int ref_v[REF_CAP];
static int ref_n;
static UNUSED int ref_find(uint32_t k) {
    for (int i = 0; i < ref_n; i++)
        if (ref_k[i] == k)
            return i;
    return -1;
}
static UNUSED int ref_put(uint32_t k, int v) { /* 1 if new */
    int i = ref_find(k);
    if (i >= 0) {
        ref_v[i] = v;
        return 0;
    }
    check(ref_n < REF_CAP, "ref capacity");
    ref_k[ref_n] = k;
    ref_v[ref_n++] = v;
    return 1;
}
static UNUSED int ref_del(uint32_t k) {
    int i = ref_find(k);
    if (i < 0)
        return 0;
    ref_k[i] = ref_k[ref_n - 1];
    ref_v[i] = ref_v[ref_n - 1];
    ref_n--;
    return 1;
}
enum { B = 4, MAXQ = 512 };
typedef struct {
    uint32_t key[B];
    int val[B];
    uint8_t used[B];
} Bucket;
typedef struct {
    Bucket *b;
    size_t nb, n;
    long moves, inserts, failed;
} Tab;

static size_t h1(const Tab *t, uint32_t k) { return mix32(k) & (t->nb - 1); }
static size_t h2(const Tab *t, uint32_t k) { return mix32(k * 0x9E3779B1u + 0x7F4A7C15u) & (t->nb - 1); }

static int find_slot(Tab *t, uint32_t k, size_t *bi, int *si) {
    size_t c[2] = {h1(t, k), h2(t, k)};
    for (int w = 0; w < 2; w++)
        for (int s = 0; s < B; s++)
            if (t->b[c[w]].used[s] && t->b[c[w]].key[s] == k) {
                *bi = c[w];
                *si = s;
                return 1;
            }
    return 0;
}

typedef struct {
    size_t bucket;
    int parent;
    int slot_in_parent; /* slot of parent bucket whose item moves into this bucket */
} QNode;

/* BFS for a chain of moves ending in a bucket with a free slot. Returns 1 on success. */
static int insert_bfs(Tab *t, uint32_t k, int v) {
    QNode q[MAXQ];
    int qh = 0, qt = 0;
    size_t start[2] = {h1(t, k), h2(t, k)};
    for (int w = 0; w < 2; w++) {
        q[qt].bucket = start[w];
        q[qt].parent = -1;
        q[qt].slot_in_parent = -1;
        qt++;
    }
    while (qh < qt) {
        QNode cur = q[qh];
        Bucket *bk = &t->b[cur.bucket];
        for (int s = 0; s < B; s++)
            if (!bk->used[s]) {
                /* unwind: shift items along the path */
                int node = qh, free_slot = s;
                while (q[node].parent >= 0) {
                    int p = q[node].parent, ps = q[node].slot_in_parent;
                    Bucket *pb = &t->b[q[p].bucket];
                    Bucket *db = &t->b[q[node].bucket];
                    db->used[free_slot] = 1;
                    db->key[free_slot] = pb->key[ps];
                    db->val[free_slot] = pb->val[ps];
                    pb->used[ps] = 0;
                    t->moves++;
                    free_slot = ps;
                    node = p;
                }
                Bucket *rb = &t->b[q[node].bucket];
                rb->used[free_slot] = 1;
                rb->key[free_slot] = k;
                rb->val[free_slot] = v;
                t->n++;
                return 1;
            }
        if (qt + B <= MAXQ)
            for (int s = 0; s < B; s++) {
                uint32_t kk = bk->key[s];
                size_t alt = h1(t, kk) == cur.bucket ? h2(t, kk) : h1(t, kk);
                q[qt].bucket = alt;
                q[qt].parent = qh;
                q[qt].slot_in_parent = s;
                qt++;
            }
        qh++;
    }
    return 0;
}

static void tab_init(Tab *t, size_t nb) {
    t->b = calloc(nb, sizeof(Bucket));
    t->nb = nb;
    t->n = 0;
}

static void tab_put(Tab *t, uint32_t k, int v) {
    size_t bi;
    int si;
    if (find_slot(t, k, &bi, &si)) {
        t->b[bi].val[si] = v;
        return;
    }
    t->inserts++;
    while (!insert_bfs(t, k, v)) {
        t->failed++;
        Tab n;
        tab_init(&n, t->nb * 2);
        n.moves = t->moves;
        n.inserts = t->inserts;
        n.failed = t->failed;
        for (size_t i = 0; i < t->nb; i++)
            for (int s = 0; s < B; s++)
                if (t->b[i].used[s])
                    check(insert_bfs(&n, t->b[i].key[s], t->b[i].val[s]), "rehash insert");
        free(t->b);
        *t = n;
    }
}

int main(void) {
    Tab t;
    memset(&t, 0, sizeof t);
    tab_init(&t, 4);
    for (int step = 0; step < 20000; step++) {
        uint32_t k = (uint32_t)(rnd() % 6000);
        int ri = ref_find(k);
        int op = (int)(rnd() % 10);
        if (op < 6) {
            int v = (int)(rnd() & 0xffff);
            tab_put(&t, k, v);
            ref_put(k, v);
        } else if (op < 7) {
            size_t bi;
            int si, f = find_slot(&t, k, &bi, &si);
            check(f == (ri >= 0), "del find");
            if (f) {
                t.b[bi].used[si] = 0;
                t.n--;
            }
            ref_del(k);
        } else {
            size_t bi;
            int si, f = find_slot(&t, k, &bi, &si);
            check(f == (ri >= 0), "find");
            if (f)
                check(t.b[bi].val[si] == ref_v[ri], "val");
        }
        check(t.n == (size_t)ref_n, "size");
    }
    size_t full = 0, occ[B + 1] = {0};
    for (size_t i = 0; i < t.nb; i++) {
        int c = 0;
        for (int s = 0; s < B; s++)
            c += t.b[i].used[s];
        occ[c]++;
        full += (size_t)c;
    }
    check(full == t.n, "occupancy");
    printf("n=%zu buckets=%zu slots=%zu load=%.4f\n", t.n, t.nb, t.nb * B, (double)t.n / (double)(t.nb * B));
    printf("inserts=%ld path moves=%ld failed placements (grow)=%ld\n", t.inserts, t.moves, t.failed);
    for (int c = 0; c <= B; c++)
        printf("buckets with %d items: %zu\n", c, occ[c]);
    free(t.b);
    return 0;
}
