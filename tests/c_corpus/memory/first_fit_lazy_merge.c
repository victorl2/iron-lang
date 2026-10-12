/*
 * title: First-fit implicit list with lazy forward coalescing
 * topic: memory
 * covers: implicit free list, first fit, splitting, coalescing during the scan, header magic, double free detection, heap walk checker
 * deps: libc
 */
#define SEED 0xF125F17ULL
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
#define MINBLK 16u
#define MAGIC 0xB10Cu
#define USED 1u

static _Alignas(16) unsigned char heap[HEAP];
static unsigned long scan_steps, merges, splits, allocs, frees, double_frees;

static uint32_t rd(unsigned off) { uint32_t v; memcpy(&v, heap + off, 4); return v; }
static void wr(unsigned off, uint32_t v) { memcpy(heap + off, &v, 4); }
static unsigned bsize(unsigned off) { return rd(off) & ~7u; }
static int bused(unsigned off) { return (int)(rd(off) & USED); }
static void setblk(unsigned off, unsigned size, int used) { wr(off, size | (used ? USED : 0)); wr(off + 4, MAGIC); }

static void heap_init(void) { setblk(0, HEAP, 0); }

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = (unsigned)((n + 7) & ~(size_t)7) + HDR;
    if (need < MINBLK) need = MINBLK;
    for (unsigned p = 0; p < HEAP; p += bsize(p)) {
        scan_steps++;
        if (bused(p)) continue;
        /* lazily merge every free neighbour that follows */
        while (p + bsize(p) < HEAP && !bused(p + bsize(p))) {
            setblk(p, bsize(p) + bsize(p + bsize(p)), 0);
            merges++;
        }
        unsigned sz = bsize(p);
        if (sz < need) continue;
        if (sz - need >= MINBLK) {
            setblk(p + need, sz - need, 0);
            sz = need; splits++;
        }
        setblk(p, sz, 1);
        allocs++;
        return (long)(p + HDR);
    }
    return -1;
}

/* returns 0 on success, -1 on detected misuse */
static int h_free(unsigned payload) {
    unsigned p = payload - HDR;
    if (rd(p + 4) != MAGIC) return -1;
    if (!bused(p)) { double_frees++; return -1; }
    wr(p, rd(p) & ~USED);
    frees++;
    return 0;
}

static void heap_check(unsigned *used_bytes, unsigned *phys_largest, unsigned *logical_largest, int *nblocks) {
    unsigned p = 0, ub = 0, big = 0, run = 0, lbig = 0;
    int nb = 0;
    while (p < HEAP) {
        unsigned sz = bsize(p);
        CHECK(sz >= MINBLK && (sz & 7) == 0 && p + sz <= HEAP && rd(p + 4) == MAGIC);
        if (bused(p)) { ub += sz; run = 0; }
        else { if (sz > big) big = sz; run += sz; if (run > lbig) lbig = run; }
        p += sz; nb++;
    }
    CHECK(p == HEAP);
    *used_bytes = ub; *phys_largest = big; *logical_largest = lbig; *nblocks = nb;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[600];
    int nlive = 0, fails = 0;
    unsigned tag = 1, ub, big, lbig;
    int nb;
    for (int step = 0; step < 10000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 600)) {
            size_t n = 1 + rnd() % 200;
            if (rnd() % 16 == 0) n += 300 + rnd() % 400;
            long off = h_alloc(n);
            if (off < 0) { fails++; continue; }
            CHECK(off % 8 == 0);
            pat_fill(heap + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            CHECK(h_free(live[i].off) == 0);
            if (rnd() % 8 == 0) CHECK(h_free(live[i].off) == -1);   /* double free is caught */
            live[i] = live[--nlive];
        }
        if (step % 128 == 0) {
            heap_check(&ub, &big, &lbig, &nb);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
        if (step == 4999) {
            heap_check(&ub, &big, &lbig, &nb);
            printf("mid-run: blocks=%d used=%u physical largest free=%u logical largest free=%u\n", nb, ub, big, lbig);
        }
    }
    printf("allocs=%lu frees=%lu failed=%d double frees caught=%lu\n", allocs, frees, fails, double_frees);
    printf("scan steps=%lu splits=%lu lazy merges=%lu\n", scan_steps, splits, merges);
    for (int i = 0; i < nlive; i++) CHECK(h_free(live[i].off) == 0);
    /* one full-size allocation merges everything lazily */
    long whole = h_alloc(HEAP - HDR);
    CHECK(whole == (long)HDR);
    heap_check(&ub, &big, &lbig, &nb);
    CHECK(nb == 1 && ub == HEAP);
    printf("whole-heap block obtained after drain: blocks=%d\n", nb);
    return 0;
}
