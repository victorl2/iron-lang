/*
 * title: Quick-list lookaside cache in front of a coalescing heap
 * topic: memory
 * covers: fastbin-style quick lists, bounded LIFO caches per size, blocks stay marked used while cached, overflow flush, consolidation on allocation failure, hit-rate statistics
 * deps: libc
 */
#define SEED 0x9C1C1157ULL
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

#define HEAP 6144u
#define TAG 4u
#define MINBLK 16u
#define USED 1u
#define CACHED 2u
#define QMAX 128u            /* largest cached block size */
#define QDEPTH 6
#define NIL 0xFFFFFFFFu

static _Alignas(16) unsigned char heap[HEAP];
static uint32_t qhead[QMAX / 8 + 1];
static int qlen[QMAX / 8 + 1];
static unsigned long q_hits, q_miss, q_push, flushes, consolidations, base_allocs, base_frees, allocs, frees;

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, heap + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(heap + o, &v, 4); }
static unsigned bsize(unsigned p) { return rd(p) & ~7u; }
static int bused(unsigned p) { return (int)(rd(p) & USED); }
static void setblk(unsigned p, unsigned size, unsigned flags) { wr(p, size | flags); wr(p + size - TAG, size | flags); }
static unsigned need_for(size_t n) { unsigned s = (unsigned)((n + 7) & ~(size_t)7) + 2 * TAG; return s < MINBLK ? MINBLK : s; }

static void heap_init(void) {
    setblk(0, HEAP, 0);
    for (unsigned i = 0; i <= QMAX / 8; i++) { qhead[i] = NIL; qlen[i] = 0; }
}

static long base_alloc(unsigned need) {
    for (unsigned p = 0; p < HEAP; p += bsize(p)) {
        unsigned sz = bsize(p);
        if (bused(p) || sz < need) continue;
        if (sz - need >= MINBLK) { setblk(p, need, USED); setblk(p + need, sz - need, 0); }
        else setblk(p, sz, USED);
        base_allocs++;
        return (long)(p + TAG);
    }
    return -1;
}

static void base_free(unsigned p) {
    unsigned sz = bsize(p);
    base_frees++;
    if (p + sz < HEAP && !bused(p + sz)) sz += bsize(p + sz);
    if (p > 0 && !(rd(p - TAG) & USED)) { unsigned ps = rd(p - TAG) & ~7u; p -= ps; sz += ps; }
    setblk(p, sz, 0);
}

static void flush_class(unsigned c) {
    while (qhead[c] != NIL) {
        unsigned p = qhead[c];
        qhead[c] = rd(p + TAG);
        qlen[c]--;
        setblk(p, bsize(p), USED);          /* clear CACHED */
        base_free(p);
    }
    flushes++;
}
static void consolidate(void) {
    consolidations++;
    for (unsigned c = 0; c <= QMAX / 8; c++) if (qhead[c] != NIL) flush_class(c);
}

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = need_for(n);
    allocs++;
    if (need <= QMAX && qhead[need / 8] != NIL) {
        unsigned c = need / 8, p = qhead[c];
        qhead[c] = rd(p + TAG); qlen[c]--;
        setblk(p, bsize(p), USED);
        q_hits++;
        return (long)(p + TAG);
    }
    if (need <= QMAX) q_miss++;
    long r = base_alloc(need);
    if (r < 0) { consolidate(); r = base_alloc(need); }
    if (r < 0) allocs--;
    return r;
}

static void h_free(unsigned payload) {
    unsigned p = payload - TAG, sz = bsize(p);
    CHECK((rd(p) & (USED | CACHED)) == USED);
    frees++;
    if (sz <= QMAX) {
        unsigned c = sz / 8;
        if (qlen[c] >= QDEPTH) flush_class(c);
        setblk(p, sz, USED | CACHED);
        wr(p + TAG, qhead[c]);
        qhead[c] = p; qlen[c]++;
        q_push++;
        return;
    }
    base_free(p);
}

static void heap_check(unsigned *nf, unsigned *fb, unsigned *cached_blocks, unsigned *cached_bytes) {
    unsigned p = 0, cnt = 0, bytes = 0, cb = 0, cby = 0;
    int prev_free = 0;
    while (p < HEAP) {
        unsigned sz = bsize(p);
        CHECK(sz >= MINBLK && (sz & 7) == 0 && p + sz <= HEAP && rd(p) == rd(p + sz - TAG));
        if (bused(p)) { prev_free = 0; if (rd(p) & CACHED) { cb++; cby += sz; } }
        else { CHECK(!prev_free); prev_free = 1; cnt++; bytes += sz; }
        p += sz;
    }
    CHECK(p == HEAP);
    unsigned listed = 0;
    for (unsigned c = 0; c <= QMAX / 8; c++) {
        int n = 0;
        for (uint32_t q = qhead[c]; q != NIL; q = rd(q + TAG)) { CHECK((rd(q) & CACHED) && bsize(q) == c * 8); n++; }
        CHECK(n == qlen[c] && n <= QDEPTH);
        listed += (unsigned)n;
    }
    CHECK(listed == cb);
    *nf = cnt; *fb = bytes; *cached_blocks = cb; *cached_bytes = cby;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[400];
    int nlive = 0, fails = 0;
    unsigned tag = 1, nf, fb, cb, cby;
    for (int step = 0; step < 16000; step++) {
        if (nlive == 0 || (rnd() % 100 < 50 && nlive < 400)) {
            unsigned k = rnd() % 20;
            size_t n = k < 14 ? 1 + rnd() % 100 : k < 19 ? 101 + rnd() % 150 : 251 + rnd() % 400;
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
            heap_check(&nf, &fb, &cb, &cby);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    heap_check(&nf, &fb, &cb, &cby);
    printf("allocs=%lu frees=%lu failed=%d\n", allocs, frees, fails);
    printf("quick hits=%lu misses=%lu pushes=%lu (hit rate %lu%% of small requests)\n", q_hits, q_miss, q_push, 100 * q_hits / (q_hits + q_miss));
    printf("class flushes=%lu consolidations=%lu base allocs=%lu base frees=%lu\n", flushes, consolidations, base_allocs, base_frees);
    printf("at end: cached blocks=%u (%u bytes) free blocks=%u free bytes=%u\n", cb, cby, nf, fb);
    for (int i = 0; i < nlive; i++) h_free(live[i].off);
    consolidate();
    heap_check(&nf, &fb, &cb, &cby);
    CHECK(nf == 1 && fb == HEAP && cb == 0);
    printf("drained after consolidation: one block of %u bytes\n", fb);
    return 0;
}
