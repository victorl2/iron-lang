/*
 * title: Best-fit allocator with free blocks indexed by an intrusive treap
 * topic: memory
 * covers: treap stored inside free blocks, key by size then address, lower-bound best fit search, delete by merge, immediate coalescing with boundary tags, tree height statistics
 * deps: libc
 */
#define SEED 0x72EA9F17ULL
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

#define HEAP 10240u
#define TAG 4u
#define MINBLK 24u          /* hdr + left + right + prio + footer */
#define USED 1u
#define NIL 0xFFFFFFFFu

static _Alignas(16) unsigned char heap[HEAP];
static uint32_t root = NIL;
static unsigned nfree_nodes;
static unsigned long allocs, frees, searches, search_steps, exact_hits, rotations, splits, merges, max_height_seen;

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, heap + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(heap + o, &v, 4); }
static unsigned bsize(unsigned p) { return rd(p) & ~7u; }
static int bused(unsigned p) { return (int)(rd(p) & USED); }
static uint32_t lc(unsigned p) { return rd(p + 4); }
static uint32_t rc(unsigned p) { return rd(p + 8); }
static uint32_t pr(unsigned p) { return rd(p + 12); }
static void set_lc(unsigned p, uint32_t v) { wr(p + 4, v); }
static void set_rc(unsigned p, uint32_t v) { wr(p + 8, v); }
static void setblk(unsigned p, unsigned size, int used) { uint32_t t = size | (used ? USED : 0); wr(p, t); wr(p + size - TAG, t); }

static int key_less(unsigned a, unsigned b) { return bsize(a) < bsize(b) || (bsize(a) == bsize(b) && a < b); }

static uint32_t rot_right(uint32_t y) { uint32_t x = lc(y); set_lc(y, rc(x)); set_rc(x, y); rotations++; return x; }
static uint32_t rot_left(uint32_t x) { uint32_t y = rc(x); set_rc(x, lc(y)); set_lc(y, x); rotations++; return y; }

static uint32_t tinsert(uint32_t t, uint32_t n) {
    if (t == NIL) return n;
    if (key_less(n, t)) {
        set_lc(t, tinsert(lc(t), n));
        if (pr(lc(t)) > pr(t)) t = rot_right(t);
    } else {
        set_rc(t, tinsert(rc(t), n));
        if (pr(rc(t)) > pr(t)) t = rot_left(t);
    }
    return t;
}
static uint32_t tmerge(uint32_t a, uint32_t b) {   /* every key in a is less than every key in b */
    if (a == NIL) return b;
    if (b == NIL) return a;
    if (pr(a) > pr(b)) { set_rc(a, tmerge(rc(a), b)); return a; }
    set_lc(b, tmerge(a, lc(b)));
    return b;
}
static uint32_t tremove(uint32_t t, uint32_t n) {
    CHECK(t != NIL);
    if (t == n) return tmerge(lc(t), rc(t));
    if (key_less(n, t)) set_lc(t, tremove(lc(t), n)); else set_rc(t, tremove(rc(t), n));
    return t;
}

static void add_free(unsigned p, unsigned size) {
    setblk(p, size, 0);
    wr(p + 4, NIL); wr(p + 8, NIL); wr(p + 12, rnd());
    /* re-write footer after payload scribble: it lives at p+size-4, untouched by the three words above */
    root = tinsert(root, p);
    nfree_nodes++;
}
static void del_free(unsigned p) { root = tremove(root, p); nfree_nodes--; }

static uint32_t lower_bound(unsigned need) {      /* smallest (size, addr) with size >= need */
    uint32_t cur = root, best = NIL;
    searches++;
    while (cur != NIL) {
        search_steps++;
        if (bsize(cur) >= need) { best = cur; cur = lc(cur); }
        else cur = rc(cur);
    }
    return best;
}

static void heap_init(void) { add_free(0, HEAP); }

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = (unsigned)((n + 7) & ~(size_t)7) + 2 * TAG;
    if (need < MINBLK) need = MINBLK;
    uint32_t p = lower_bound(need);
    if (p == NIL) return -1;
    unsigned sz = bsize(p);
    if (sz == need) exact_hits++;
    del_free(p);
    if (sz - need >= MINBLK) { setblk(p, need, 1); add_free(p + need, sz - need); splits++; }
    else setblk(p, sz, 1);
    allocs++;
    return (long)(p + TAG);
}

static void h_free(unsigned payload) {
    unsigned p = payload - TAG, sz = bsize(p);
    CHECK(bused(p));
    frees++;
    if (p + sz < HEAP && !bused(p + sz)) { unsigned nx = p + sz; del_free(nx); sz += bsize(nx); merges++; }
    if (p > 0 && !(rd(p - TAG) & USED)) { unsigned ps = rd(p - TAG) & ~7u; unsigned pp = p - ps; del_free(pp); p = pp; sz += ps; merges++; }
    add_free(p, sz);
}

static unsigned tcheck(uint32_t t, unsigned depth, unsigned *maxd, unsigned *count, uint32_t *last, int *have_last) {
    if (t == NIL) return 0;
    CHECK(!bused(t) && rd(t + bsize(t) - TAG) == bsize(t));
    if (depth > *maxd) *maxd = depth;
    if (lc(t) != NIL) CHECK(pr(lc(t)) <= pr(t));
    if (rc(t) != NIL) CHECK(pr(rc(t)) <= pr(t));
    tcheck(lc(t), depth + 1, maxd, count, last, have_last);
    if (*have_last) CHECK(key_less(*last, t));
    *last = t; *have_last = 1; (*count)++;
    tcheck(rc(t), depth + 1, maxd, count, last, have_last);
    return 1;
}

static void heap_check(unsigned *nf, unsigned *fb, unsigned *big, unsigned *height) {
    unsigned p = 0, cnt = 0, bytes = 0, mx = 0;
    int prev_free = 0;
    while (p < HEAP) {
        unsigned sz = bsize(p);
        CHECK(sz >= MINBLK && (sz & 7) == 0 && p + sz <= HEAP && rd(p) == rd(p + sz - TAG));
        if (bused(p)) prev_free = 0;
        else { CHECK(!prev_free); prev_free = 1; cnt++; bytes += sz; if (sz > mx) mx = sz; }
        p += sz;
    }
    CHECK(p == HEAP);
    unsigned maxd = 0, tc = 0, h;
    uint32_t last = 0; int have = 0;
    tcheck(root, 1, &maxd, &tc, &last, &have);
    h = maxd;
    CHECK(tc == cnt && cnt == nfree_nodes);
    if (h > max_height_seen) max_height_seen = h;
    *nf = cnt; *fb = bytes; *big = mx; *height = h;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[700];
    int nlive = 0, fails = 0;
    unsigned tag = 1, nf, fb, big, ht;
    for (int step = 0; step < 16000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 700)) {
            unsigned k = rnd() % 20;
            size_t n = k < 12 ? 1 + rnd() % 60 : k < 18 ? 61 + rnd() % 160 : 221 + rnd() % 500;
            long off = h_alloc(n);
            if (off < 0) { fails++; continue; }
            pat_fill(heap + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            h_free(live[i].off);
            live[i] = live[--nlive];
        }
        if (step % 100 == 0) {
            heap_check(&nf, &fb, &big, &ht);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    heap_check(&nf, &fb, &big, &ht);
    printf("allocs=%lu frees=%lu failed=%d live=%d\n", allocs, frees, fails, nlive);
    printf("best-fit searches=%lu, avg steps=%.2f, exact fits=%lu\n", searches, (double)search_steps / (double)searches, exact_hits);
    printf("splits=%lu merges=%lu rotations=%lu\n", splits, merges, rotations);
    printf("free blocks=%u bytes=%u largest=%u height=%u (max sampled %lu)\n", nf, fb, big, ht, max_height_seen);
    for (int i = 0; i < nlive; i++) h_free(live[i].off);
    heap_check(&nf, &fb, &big, &ht);
    CHECK(nf == 1 && fb == HEAP && ht == 1);
    printf("drained: one free block, tree height %u\n", ht);
    return 0;
}
