/*
 * title: Deferred coalescing versus eager coalescing with exact-size free lists
 * topic: memory
 * covers: deferred coalescing, exact-size free lists, coalesce pass rebuilding lists, retry after failure, work counters compared across modes, heap walk checker
 * deps: libc
 */
#define SEED 0xDEFE44EDULL
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

#define HEAP 8192u
#define HDR 8u
#define NCLASS 17           /* index 1..16: exact 16-byte multiples up to 256, index 0: larger */
#define NIL 0xFFFFFFFFu
#define USED 1u

static _Alignas(16) unsigned char heap[HEAP];
static uint32_t lists[NCLASS];
static int eager;
static unsigned long visits, passes, allocs, frees, fails, list_pops, splits, retries;

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, heap + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(heap + o, &v, 4); }
static unsigned bsize(unsigned p) { return rd(p) & ~7u; }
static int bused(unsigned p) { return (int)(rd(p) & USED); }
static int class_of(unsigned size) { return size <= 256 ? (int)(size / 16) : 0; }

static void push_free(unsigned p, unsigned size) {
    wr(p, size); wr(p + 4, 0xF4EE);
    int c = class_of(size);
    wr(p + 8, lists[c]);
    lists[c] = p;
}

static void heap_init(void) {
    for (int c = 0; c < NCLASS; c++) lists[c] = NIL;
    push_free(0, HEAP);
}

/* merge every run of adjacent free blocks and rebuild all lists from the heap image */
static void coalesce_pass(void) {
    passes++;
    for (int c = 0; c < NCLASS; c++) lists[c] = NIL;
    unsigned p = 0;
    while (p < HEAP) {
        visits++;
        if (bused(p)) { p += bsize(p); continue; }
        unsigned q = p, total = 0;
        while (q < HEAP && !bused(q)) { total += bsize(q); q += bsize(q); visits++; }
        push_free(p, total);
        p = q;
    }
}

static long take(unsigned p, unsigned need) {
    unsigned sz = bsize(p);
    if (sz - need >= 16) { wr(p, need | USED); push_free(p + need, sz - need); splits++; }
    else wr(p, sz | USED);
    wr(p + 4, 0xA110C);
    return (long)(p + HDR);
}

static long try_alloc(unsigned need) {
    int c = class_of(need);
    if (c != 0 && lists[c] != NIL) {
        uint32_t p = lists[c]; lists[c] = rd(p + 8); list_pops++;
        return take(p, need);
    }
    for (int k = c ? c + 1 : NCLASS; k < NCLASS; k++)      /* next larger exact class */
        if (lists[k] != NIL) { uint32_t p = lists[k]; lists[k] = rd(p + 8); list_pops++; return take(p, need); }
    uint32_t prev = NIL;
    for (uint32_t p = lists[0]; p != NIL; prev = p, p = rd(p + 8)) {
        visits++;
        if (bsize(p) < need) continue;
        if (prev == NIL) lists[0] = rd(p + 8); else wr(prev + 8, rd(p + 8));
        return take(p, need);
    }
    return -1;
}

static long h_alloc(size_t n) {
    unsigned need = (unsigned)((n + 15 + HDR) & ~(size_t)15);
    if (need < 16) need = 16;
    long r = try_alloc(need);
    if (r < 0 && !eager) {
        retries++;
        coalesce_pass();
        r = try_alloc(need);
    }
    if (r < 0) { fails++; return -1; }
    allocs++;
    return r;
}

static void h_free(unsigned payload) {
    unsigned p = payload - HDR;
    CHECK(bused(p));
    frees++;
    push_free(p, bsize(p));
    if (eager) coalesce_pass();
}

static void heap_check(unsigned *nfree, unsigned *largest_phys, unsigned *adjacent_pairs) {
    unsigned p = 0, cnt = 0, big = 0, adj = 0;
    int prev_free = 0;
    while (p < HEAP) {
        unsigned sz = bsize(p);
        CHECK(sz >= 16 && (sz & 15) == 0 && p + sz <= HEAP);
        if (bused(p)) prev_free = 0;
        else { cnt++; if (sz > big) big = sz; if (prev_free) adj++; prev_free = 1; }
        p += sz;
    }
    CHECK(p == HEAP);
    unsigned listed = 0;
    for (int c = 0; c < NCLASS; c++)
        for (uint32_t q = lists[c]; q != NIL; q = rd(q + 8)) {
            CHECK(!bused(q) && class_of(bsize(q)) == c);
            listed++; CHECK(listed <= cnt);
        }
    CHECK(listed == cnt);
    if (eager) CHECK(adj == 0);
    *nfree = cnt; *largest_phys = big; *adjacent_pairs = adj;
}

typedef struct { int is_alloc; unsigned size; int target; } Op;
#define NOPS 14000
static Op ops[NOPS];

static void run(int eager_mode) {
    eager = eager_mode;
    visits = passes = allocs = frees = fails = list_pops = splits = retries = 0;
    heap_init();
    static struct { long off; unsigned n; unsigned tag; int ok; } slot[NOPS];
    memset(slot, 0, sizeof slot);
    unsigned nf, big, adj;
    unsigned max_adj = 0;
    for (int i = 0; i < NOPS; i++) {
        if (ops[i].is_alloc) {
            long off = h_alloc(ops[i].size);
            if (off < 0) continue;
            slot[i].off = off; slot[i].n = ops[i].size; slot[i].tag = (unsigned)i + 1; slot[i].ok = 1;
            pat_fill(heap + off, slot[i].n, slot[i].tag);
        } else if (slot[ops[i].target].ok) {
            int t = ops[i].target;
            CHECK(pat_ok(heap + slot[t].off, slot[t].n, slot[t].tag));
            h_free((unsigned)slot[t].off);
            slot[t].ok = 0;
        }
        if (i % 200 == 0) {
            heap_check(&nf, &big, &adj);
            if (adj > max_adj) max_adj = adj;
            for (int j = 0; j <= i; j++) if (slot[j].ok) CHECK(pat_ok(heap + slot[j].off, slot[j].n, slot[j].tag));
        }
    }
    heap_check(&nf, &big, &adj);
    printf("%-8s allocs=%lu fails=%lu frees=%lu\n", eager ? "eager" : "deferred", allocs, fails, frees);
    printf("         coalesce passes=%lu block visits=%lu exact-list pops=%lu splits=%lu retries=%lu\n", passes, visits, list_pops, splits, retries);
    printf("         end: free blocks=%u adjacent free pairs=%u (max sampled %u)\n", nf, adj, max_adj);
    for (int i = 0; i < NOPS; i++) if (slot[i].ok) h_free((unsigned)slot[i].off);
    coalesce_pass();
    heap_check(&nf, &big, &adj);
    CHECK(nf == 1 && big == HEAP);
}

int main(void) {
    int live[110], nl = 0;
    for (int i = 0; i < NOPS; i++) {
        if (nl == 0 || (rnd() % 100 < 52 && nl < 110)) {
            unsigned k = rnd() % 10;
            ops[i].is_alloc = 1; ops[i].size = k < 7 ? 1 + rnd() % 60 : k < 9 ? 61 + rnd() % 100 : 161 + rnd() % 300;
            ops[i].target = -1; live[nl++] = i;
        } else {
            int j = (int)(rnd() % (unsigned)nl);
            ops[i].is_alloc = 0; ops[i].target = live[j]; live[j] = live[--nl];
        }
    }
    run(0);
    run(1);
    return 0;
}
