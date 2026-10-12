/*
 * title: Binary buddy allocator over an implicit tree with allocation bitmap
 * topic: memory
 * covers: buddy system, implicit binary tree, longest-free-order array, allocation bit per node, recursive invariant check, no per-block links
 * deps: libc
 */
#define SEED 0x7EEB1DD1ULL
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = SEED;
static unsigned rnd(void) {
    rs += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = rs;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return (unsigned)(z ^ (z >> 31));
}
static void pat_fill(void *vp, size_t n, unsigned tag) {
    unsigned char *p = vp;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u);
}
static int pat_ok(const void *vp, size_t n, unsigned tag) {
    const unsigned char *p = vp;
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)(tag * 37u + (unsigned)i * 11u + 5u)) return 0;
    return 1;
}

#define LEVELS 9                 /* 256 units */
#define NUNITS (1u << (LEVELS - 1))
#define UNIT 16
#define NNODES (2u * NUNITS - 1)

static _Alignas(16) unsigned char heap[NUNITS * UNIT];
/* longest[n]: largest free block order+1 available in the subtree of node n (0 = none) */
static unsigned char longest[NNODES];
static unsigned char allocbit[(NNODES + 7) / 8];
static size_t used_units, peak_units, node_visits;

static int getbit(unsigned n) { return (allocbit[n >> 3] >> (n & 7)) & 1; }
static void setbit(unsigned n, int v) {
    if (v) allocbit[n >> 3] |= (unsigned char)(1u << (n & 7));
    else allocbit[n >> 3] &= (unsigned char)~(1u << (n & 7));
}

static void tree_init(void) {
    memset(allocbit, 0, sizeof allocbit);
    for (unsigned n = 0; n < NNODES; n++) {
        unsigned depth = 0;
        for (unsigned m = n + 1; m > 1; m >>= 1) depth++;
        longest[n] = (unsigned char)(LEVELS - depth);   /* order+1 of the block this node represents */
    }
    used_units = peak_units = 0;
}

static unsigned units_for(size_t n) {
    unsigned u = 1;
    while (u * UNIT < n) u <<= 1;
    return u;
}
static unsigned order_of(unsigned units) { unsigned o = 0; while ((1u << o) < units) o++; return o; }

static long tree_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned units = units_for(n);
    if (units > NUNITS) return -1;
    unsigned want = order_of(units) + 1;  /* order+1 */
    if (longest[0] < want) return -1;
    unsigned node = 0, level_order = LEVELS;  /* order+1 at node */
    while (level_order > want) {
        node_visits++;
        unsigned l = 2 * node + 1, r = 2 * node + 2;
        /* prefer the child with the smaller sufficient free run to keep big blocks intact */
        int lok = longest[l] >= want, rok = longest[r] >= want;
        if (lok && rok) node = longest[l] <= longest[r] ? l : r;
        else node = lok ? l : r;
        level_order--;
    }
    CHECK(longest[node] == want && !getbit(node));
    setbit(node, 1);
    longest[node] = 0;
    unsigned span = 1u << (want - 1);
    /* offset in units: index within the level times block span */
    unsigned first_at_level = 1u;
    for (unsigned k = 0; k < LEVELS - want; k++) first_at_level <<= 1;
    unsigned off_units = (node + 1 - first_at_level) * span;
    for (unsigned p = node; p > 0; ) {
        p = (p - 1) / 2;
        unsigned a = longest[2 * p + 1], b = longest[2 * p + 2];
        longest[p] = (unsigned char)(a > b ? a : b);
    }
    used_units += span;
    if (used_units > peak_units) peak_units = used_units;
    return (long)off_units * UNIT;
}

static void tree_free(unsigned off_bytes) {
    unsigned unit = off_bytes / UNIT;
    unsigned node = unit + NUNITS - 1;      /* leaf */
    unsigned order1 = 1;
    while (!getbit(node)) {
        CHECK(node > 0);
        node = (node - 1) / 2;
        order1++;
    }
    setbit(node, 0);
    longest[node] = (unsigned char)order1;
    used_units -= 1u << (order1 - 1);
    while (node > 0) {
        node = (node - 1) / 2;
        order1++;
        unsigned a = longest[2 * node + 1], b = longest[2 * node + 2];
        if (a == order1 - 1 && b == order1 - 1) longest[node] = (unsigned char)order1;  /* coalesce */
        else longest[node] = (unsigned char)(a > b ? a : b);
    }
}

/* returns free units in subtree, verifying longest[] against the allocation bits */
static unsigned verify_node(unsigned n, unsigned order1) {
    if (getbit(n)) {
        CHECK(longest[n] == 0);
        return 0;
    }
    if (order1 == 1) { CHECK(longest[n] == 1); return 1; }
    unsigned a = verify_node(2 * n + 1, order1 - 1), b = verify_node(2 * n + 2, order1 - 1);
    unsigned full = 1u << (order1 - 1);
    if (a + b == full) CHECK(longest[n] == order1);
    else {
        unsigned x = longest[2 * n + 1], y = longest[2 * n + 2];
        CHECK(longest[n] == (x > y ? x : y));
    }
    return a + b;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    tree_init();
    Rec live[NUNITS];
    int nlive = 0, fails = 0;
    unsigned tag = 1;
    size_t alloc_by_order[LEVELS] = {0};
    for (int step = 0; step < 8000; step++) {
        if (nlive == 0 || (rnd() % 100 < 53 && nlive < (int)NUNITS)) {
            unsigned k = rnd() % 10;
            size_t n = k < 6 ? 1 + rnd() % 48 : k < 9 ? 49 + rnd() % 200 : 250 + rnd() % 1000;
            long off = tree_alloc(n);
            if (off < 0) { fails++; continue; }
            alloc_by_order[order_of(units_for(n))]++;
            CHECK(off % UNIT == 0 && (size_t)off + n <= sizeof heap);
            pat_fill(heap + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            tree_free(live[i].off);
            live[i] = live[--nlive];
        }
        if (step % 100 == 0) {
            unsigned freeu = verify_node(0, LEVELS);
            CHECK(freeu + used_units == NUNITS);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            /* live blocks never overlap */
            for (int i = 0; i < nlive; i++) for (int j = i + 1; j < nlive; j++)
                CHECK(live[i].off + live[i].n <= live[j].off || live[j].off + live[j].n <= live[i].off);
        }
    }
    for (unsigned o = 0; o < LEVELS; o++) printf("order %u (%4u bytes): %zu allocations\n", o, (1u << o) * UNIT, alloc_by_order[o]);
    printf("failed=%d peak units=%zu of %u, tree steps=%zu\n", fails, peak_units, NUNITS, node_visits);
    printf("live now=%d used units=%zu longest order+1=%u\n", nlive, used_units, longest[0]);
    for (int i = 0; i < nlive; i++) tree_free(live[i].off);
    CHECK(used_units == 0 && longest[0] == LEVELS && verify_node(0, LEVELS) == NUNITS);
    for (size_t i = 0; i < sizeof allocbit; i++) CHECK(allocbit[i] == 0);
    printf("fully coalesced back to one %u-byte block\n", NUNITS * UNIT);
    return 0;
}
