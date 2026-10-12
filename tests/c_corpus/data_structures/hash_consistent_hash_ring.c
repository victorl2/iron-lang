/*
 * title: Consistent hash ring with virtual nodes
 * topic: data_structures
 * covers: consistent hashing, virtual nodes, sorted ring with binary search, key movement on join/leave, load balance
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
enum { MAXPTS = 4096, KEYS = 20000 };
typedef struct {
    uint32_t pos;
    int node;
} Pt;
typedef struct {
    Pt pts[MAXPTS];
    int n;
} Ring;

static int cmp_pt(const void *a, const void *b) {
    const Pt *x = a, *y = b;
    if (x->pos != y->pos)
        return x->pos < y->pos ? -1 : 1;
    return x->node - y->node;
}

static void ring_add(Ring *r, int node, int vnodes) {
    for (int v = 0; v < vnodes; v++) {
        check(r->n < MAXPTS, "ring capacity");
        r->pts[r->n].pos = mix32((uint32_t)node * 0x9E3779B1u + (uint32_t)v * 0x85EBCA6Bu + 12345u);
        r->pts[r->n].node = node;
        r->n++;
    }
    qsort(r->pts, (size_t)r->n, sizeof(Pt), cmp_pt);
}

static void ring_remove(Ring *r, int node) {
    int w = 0;
    for (int i = 0; i < r->n; i++)
        if (r->pts[i].node != node)
            r->pts[w++] = r->pts[i];
    r->n = w;
}

/* first point clockwise from the key hash: binary search */
static int ring_owner(const Ring *r, uint32_t key) {
    uint32_t h = mix32(key ^ 0xABCDEF01u);
    int lo = 0, hi = r->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (r->pts[mid].pos < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    return r->pts[lo == r->n ? 0 : lo].node;
}

/* brute force reference: smallest point >= h, else the globally smallest */
static int owner_brute(const Ring *r, uint32_t key) {
    uint32_t h = mix32(key ^ 0xABCDEF01u);
    int best = -1, smallest = 0;
    for (int i = 0; i < r->n; i++) {
        if (r->pts[i].pos < r->pts[smallest].pos || (r->pts[i].pos == r->pts[smallest].pos && r->pts[i].node < r->pts[smallest].node))
            smallest = i;
        if (r->pts[i].pos >= h && (best < 0 || r->pts[i].pos < r->pts[best].pos ||
                                   (r->pts[i].pos == r->pts[best].pos && r->pts[i].node < r->pts[best].node)))
            best = i;
    }
    return r->pts[best >= 0 ? best : smallest].node;
}

static void distribution(const Ring *r, int maxnode, const char *label) {
    long load[16] = {0};
    for (uint32_t k = 0; k < KEYS; k++)
        load[ring_owner(r, k)]++;
    long mn = KEYS, mx = 0;
    int nodes = 0;
    for (int i = 0; i < maxnode; i++)
        if (load[i]) {
            nodes++;
            if (load[i] < mn)
                mn = load[i];
            if (load[i] > mx)
                mx = load[i];
        }
    printf("%-22s nodes=%d min load=%ld max load=%ld ratio=%.3f\n", label, nodes, mn, mx, (double)mx / (double)mn);
}

int main(void) {
    static int before[KEYS], after[KEYS];
    /* balance versus number of virtual nodes */
    static const int vn[4] = {1, 8, 64, 256};
    for (int c = 0; c < 4; c++) {
        Ring r;
        r.n = 0;
        for (int node = 0; node < 8; node++)
            ring_add(&r, node, vn[c]);
        char label[32];
        snprintf(label, sizeof label, "8 nodes, %d vnodes", vn[c]);
        distribution(&r, 16, label);
        for (uint32_t k = 0; k < 500; k++)
            check(ring_owner(&r, k) == owner_brute(&r, k), "binary search equals brute force");
    }
    Ring r;
    r.n = 0;
    for (int node = 0; node < 8; node++)
        ring_add(&r, node, 128);
    for (uint32_t k = 0; k < KEYS; k++)
        before[k] = ring_owner(&r, k);
    /* join: node 8 arrives; only keys that land on node 8 may move */
    ring_add(&r, 8, 128);
    long moved = 0;
    for (uint32_t k = 0; k < KEYS; k++) {
        after[k] = ring_owner(&r, k);
        if (after[k] != before[k]) {
            check(after[k] == 8, "keys only move to the new node");
            moved++;
        }
    }
    printf("join node 8: moved %ld of %d keys (%.4f, ideal 1/9=%.4f)\n", moved, KEYS, (double)moved / KEYS, 1.0 / 9);
    /* leave: node 3 departs; only its keys move */
    for (uint32_t k = 0; k < KEYS; k++)
        before[k] = after[k];
    long on3 = 0;
    ring_remove(&r, 3);
    moved = 0;
    for (uint32_t k = 0; k < KEYS; k++) {
        after[k] = ring_owner(&r, k);
        on3 += before[k] == 3;
        if (after[k] != before[k]) {
            check(before[k] == 3, "only keys of the leaver move");
            moved++;
        }
    }
    check(moved == on3, "all keys of the leaver moved");
    printf("leave node 3: moved %ld keys (all were on node 3: %ld)\n", moved, on3);
    distribution(&r, 16, "after join+leave");
    /* contrast: naive modulo placement reshuffles almost everything on resize */
    long naive_moved = 0;
    for (uint32_t k = 0; k < KEYS; k++)
        naive_moved += mix32(k) % 8 != mix32(k) % 9;
    printf("naive hash mod N, 8 to 9 nodes: moved %ld keys (%.4f)\n", naive_moved, (double)naive_moved / KEYS);
    return 0;
}
