/*
 * title: Segregated free lists with size-sorted bins and prologue/epilogue blocks
 * topic: memory
 * covers: segregated fits, power-of-two size classes, sorted insertion (best fit inside a bin), header and footer tags, prologue and epilogue sentinels, bin occupancy statistics
 * deps: libc
 */
#define SEED 0x5E61157ULL
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

#define HEAP 12288u
#define NBINS 8
#define NIL 0xFFFFFFFFu
#define MINBLK 16u
#define ALLOC 1u
#define EPI (HEAP - 8)   /* epilogue header offset; keeps block sizes multiples of 8 */

static _Alignas(16) unsigned char heap[HEAP];
static uint32_t bin_head[NBINS];
static unsigned bin_count[NBINS], bin_maxdepth[NBINS];
static unsigned long allocs, frees, scans, splits, merges, bin_alloc_hits[NBINS];

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, heap + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(heap + o, &v, 4); }
static unsigned bsize(unsigned p) { return rd(p) & ~7u; }
static int balloc(unsigned p) { return (int)(rd(p) & ALLOC); }
static unsigned nxt(unsigned p) { return rd(p + 4); }
static unsigned prv(unsigned p) { return rd(p + 8); }

static int bin_of(unsigned size) {
    int b = 0;
    unsigned lim = 32;
    while (b < NBINS - 1 && size > lim) { b++; lim <<= 1; }
    return b;
}

static void set_tags(unsigned p, unsigned size, unsigned a) { wr(p, size | a); wr(p + size - 4, size | a); }

static void bin_insert(unsigned p) {
    int b = bin_of(bsize(p));
    unsigned sz = bsize(p);
    uint32_t prev = NIL, cur = bin_head[b];
    unsigned depth = 0;
    while (cur != NIL && (bsize(cur) < sz || (bsize(cur) == sz && cur < p))) { prev = cur; cur = nxt(cur); depth++; }
    wr(p + 4, cur); wr(p + 8, prev);
    if (cur != NIL) wr(cur + 8, p);
    if (prev != NIL) wr(prev + 4, p); else bin_head[b] = p;
    bin_count[b]++;
    if (bin_count[b] > bin_maxdepth[b]) bin_maxdepth[b] = bin_count[b];
    (void)depth;
}
static void bin_remove(unsigned p) {
    int b = bin_of(bsize(p));
    uint32_t n = nxt(p), pv = prv(p);
    if (pv != NIL) wr(pv + 4, n); else bin_head[b] = n;
    if (n != NIL) wr(n + 8, pv);
    bin_count[b]--;
}

static void heap_init(void) {
    for (int b = 0; b < NBINS; b++) bin_head[b] = NIL;
    set_tags(0, 8, ALLOC);                  /* prologue */
    wr(EPI, ALLOC);                    /* epilogue header, size 0 */
    set_tags(8, EPI - 8, 0);
    bin_insert(8);
}

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = (unsigned)((n + 7) & ~(size_t)7) + 8;
    if (need < MINBLK) need = MINBLK;
    for (int b = bin_of(need); b < NBINS; b++) {
        for (uint32_t p = bin_head[b]; p != NIL; p = nxt(p)) {
            scans++;
            if (bsize(p) < need) continue;
            unsigned sz = bsize(p);
            bin_remove(p);
            if (sz - need >= MINBLK) {
                set_tags(p, need, ALLOC);
                set_tags(p + need, sz - need, 0);
                bin_insert(p + need);
                splits++;
            } else set_tags(p, sz, ALLOC);
            allocs++; bin_alloc_hits[b]++;
            return (long)(p + 4);
        }
    }
    return -1;
}

static void h_free(unsigned payload) {
    unsigned p = payload - 4, sz = bsize(p);
    CHECK(balloc(p) && rd(p + sz - 4) == rd(p));
    frees++;
    if (!balloc(p + sz)) {
        unsigned nx = p + sz;
        bin_remove(nx);
        sz += bsize(nx);
        merges++;
    }
    if (!(rd(p - 4) & ALLOC)) {
        unsigned psz = rd(p - 4) & ~7u;
        unsigned pp = p - psz;
        bin_remove(pp);
        sz += psz; p = pp;
        merges++;
    }
    set_tags(p, sz, 0);
    bin_insert(p);
}

static void heap_check(unsigned *nf, unsigned *fbytes, unsigned *largest) {
    CHECK(rd(0) == (8 | ALLOC) && rd(4) == (8 | ALLOC) && rd(EPI) == ALLOC);
    unsigned p = 8, cnt = 0, fb = 0, big = 0;
    int prev_free = 0;
    while (p < EPI) {
        unsigned sz = bsize(p);
        CHECK(sz >= MINBLK && (sz & 7) == 0 && p + sz <= EPI && rd(p) == rd(p + sz - 4));
        if (balloc(p)) prev_free = 0;
        else { CHECK(!prev_free); prev_free = 1; cnt++; fb += sz; if (sz > big) big = sz; }
        p += sz;
    }
    CHECK(p == EPI);
    unsigned listed = 0;
    for (int b = 0; b < NBINS; b++) {
        uint32_t pv = NIL;
        unsigned c = 0, last = 0;
        for (uint32_t q = bin_head[b]; q != NIL; q = nxt(q)) {
            CHECK(!balloc(q) && bin_of(bsize(q)) == b && prv(q) == pv && bsize(q) >= last);
            last = bsize(q); pv = q; c++;
        }
        CHECK(c == bin_count[b]);
        listed += c;
    }
    CHECK(listed == cnt);
    *nf = cnt; *fbytes = fb; *largest = big;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[900];
    int nlive = 0, fails = 0;
    unsigned tag = 1, nf, fb, big;
    for (int step = 0; step < 14000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 900)) {
            unsigned k = rnd() % 20;
            size_t n = k < 12 ? 1 + rnd() % 40 : k < 18 ? 41 + rnd() % 200 : 241 + rnd() % 900;
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
            heap_check(&nf, &fb, &big);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    heap_check(&nf, &fb, &big);
    printf("allocs=%lu frees=%lu failed=%d live=%d\n", allocs, frees, fails, nlive);
    printf("bin scans=%lu (%.2f per alloc) splits=%lu merges=%lu\n", scans, (double)scans / (double)(allocs + (unsigned long)fails), splits, merges);
    for (int b = 0; b < NBINS; b++)
        printf("bin %d: served=%5lu max depth=%3u now=%u\n", b, bin_alloc_hits[b], bin_maxdepth[b], bin_count[b]);
    printf("free blocks=%u bytes=%u largest=%u\n", nf, fb, big);
    for (int i = 0; i < nlive; i++) h_free(live[i].off);
    heap_check(&nf, &fb, &big);
    CHECK(nf == 1 && fb == EPI - 8);
    printf("drained: one block of %u bytes in bin %d\n", big, bin_of(big));
    return 0;
}
