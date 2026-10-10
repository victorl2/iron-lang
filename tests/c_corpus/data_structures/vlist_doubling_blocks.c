/*
 * title: VList with geometrically growing blocks and shared tails
 * topic: data_structures
 * covers: Bagwell VList, doubling block sizes, (block, offset) list values, logarithmic indexing, persistence by forking blocks
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 6174u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef struct Block Block;
/* A list value points at one element inside a block. Elements sit at indexes 0..used-1 of the block;
 * the list is elements[off], elements[off-1], ..., elements[0], then whatever the block's link denotes. */
typedef struct { Block *b; int off; } VList;
struct Block {
    int cap, used;
    long base;        /* number of elements in the list this block links to */
    VList next;
    int elem[];
};

static Block **all;
static size_t all_n, all_cap;
static long walk_steps;

static Block *block_new(int cap, VList next) {
    Block *b = malloc(sizeof *b + (size_t)cap * sizeof(int));
    CHECK(b);
    b->cap = cap; b->used = 0; b->next = next;
    b->base = next.b ? (long)next.off + 1 + next.b->base : 0;
    if (all_n == all_cap) {
        all_cap = all_cap ? all_cap * 2 : 256;
        all = realloc(all, all_cap * sizeof *all);
        CHECK(all);
    }
    all[all_n++] = b;
    return b;
}
static VList v_nil(void) { VList v = { NULL, -1 }; return v; }
static long v_length(VList v) { return v.b ? v.b->base + v.off + 1 : 0; }

static VList v_cons(int x, VList v) {
    /* fast path: we point at the newest element of a block that still has room */
    if (v.b && v.off == v.b->used - 1 && v.b->used < v.b->cap) {
        v.b->elem[v.b->used++] = x;
        v.off++;
        return v;
    }
    int cap = v.b ? v.b->cap * 2 : 1;
    if (cap > 256) cap = 256;
    Block *b = block_new(cap, v);
    b->elem[0] = x;
    b->used = 1;
    VList r = { b, 0 };
    return r;
}
static int v_head(VList v) { CHECK(v.b); return v.b->elem[v.off]; }
static VList v_tail(VList v) {
    CHECK(v.b);
    if (v.off > 0) { v.off--; return v; }
    return v.b->next;
}
static int v_nth(VList v, long i) {
    while (v.b) {
        walk_steps++;
        if (i <= v.off) return v.b->elem[v.off - i];
        i -= (long)v.off + 1;
        v = v.b->next;
    }
    CHECK(0);
    return 0;
}
static int v_blocks(VList v) { int n = 0; while (v.b) { n++; v = v.b->next; } return n; }
static int v_equal_shape(VList a, VList b) { return a.b == b.b && a.off == b.off; }

#define NV 12
#define MAXL 1500
int main(void) {
    VList vs[NV];
    int model[NV][MAXL];
    long mlen[NV];
    for (int i = 0; i < NV; i++) { vs[i] = v_nil(); mlen[i] = 0; }
    long conses = 0, tails = 0, forks = 0, nth_queries = 0;
    for (int step = 0; step < 8000; step++) {
        int i = (int)(rnd() % NV), j = (int)(rnd() % NV);
        unsigned op = rnd() % 10;
        if (op < 5) {
            if (mlen[i] < MAXL) {
                int x = (int)(rnd() % 100000);
                /* cons onto a version that may share its block with other versions */
                VList nv = v_cons(x, vs[i]);
                if (j != i) { memcpy(model[j], model[i], (size_t)mlen[i] * sizeof(int)); mlen[j] = mlen[i]; }
                vs[j] = nv;
                model[j][mlen[j]++] = x;
                conses++;
            }
        } else if (op < 7) {
            if (mlen[i] > 0) {
                CHECK(v_head(vs[i]) == model[i][mlen[i] - 1]);
                vs[j] = v_tail(vs[i]);
                if (j != i) { memcpy(model[j], model[i], (size_t)mlen[i] * sizeof(int)); mlen[j] = mlen[i]; }
                mlen[j]--;
                tails++;
            }
        } else if (op < 8) {
            vs[j] = vs[i];
            memcpy(model[j], model[i], (size_t)mlen[i] * sizeof(int));
            mlen[j] = mlen[i];
            forks++;
        } else if (mlen[i] > 0) {
            long k = (long)(rnd() % (unsigned long)mlen[i]);
            CHECK(v_nth(vs[i], k) == model[i][mlen[i] - 1 - k]);
            nth_queries++;
        }
        CHECK(v_length(vs[j]) == mlen[j]);
    }
    /* full comparison of every version */
    long cells = 0;
    int max_blocks = 0;
    for (int i = 0; i < NV; i++) {
        VList v = vs[i];
        for (long k = 0; k < mlen[i]; k++) {
            CHECK(v.b && v_head(v) == model[i][mlen[i] - 1 - k]);
            v = v_tail(v);
            cells++;
        }
        CHECK(v.b == NULL);
        int nb = v_blocks(vs[i]);
        if (nb > max_blocks) max_blocks = nb;
        CHECK((long)nb <= mlen[i]);
    }
    /* sequential build: block sizes double, so n elements need about log2(n) blocks */
    VList big = v_nil();
    for (int i = 0; i < 1000; i++) big = v_cons(i, big);
    int blocks = v_blocks(big);
    walk_steps = 0;
    long sum = 0;
    for (long k = 0; k < 1000; k++) sum += v_nth(big, k);
    CHECK(sum == 999L * 1000 / 2);
    printf("conses=%ld tails=%ld forks=%ld nth queries=%ld, cells verified=%ld\n", conses, tails, forks, nth_queries, cells);
    printf("1000-element list: %d blocks, %ld block hops for 1000 index reads\n", blocks, walk_steps);
    printf("longest version chain: %d blocks, total blocks allocated: %zu\n", max_blocks, all_n);
    /* a fork at a mid-block position must not corrupt the older version */
    VList base = v_nil();
    for (int i = 0; i < 6; i++) base = v_cons(i, base);
    VList mid = v_tail(v_tail(base));
    VList fork = v_cons(100, mid);
    VList grow = v_cons(200, base);
    CHECK(v_head(fork) == 100 && v_head(v_tail(fork)) == 3 && v_length(fork) == 5);
    CHECK(v_head(grow) == 200 && v_head(v_tail(grow)) == 5 && v_length(grow) == 7);
    CHECK(v_head(base) == 5 && v_length(base) == 6 && !v_equal_shape(fork, mid));
    printf("fork checks: fork len %ld, grown len %ld, original len %ld\n", v_length(fork), v_length(grow), v_length(base));
    for (size_t i = 0; i < all_n; i++) free(all[i]);
    free(all);
    return 0;
}
