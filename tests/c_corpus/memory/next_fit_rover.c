/*
 * title: Next-fit allocator with roving pointer and boundary tags
 * topic: memory
 * covers: next fit, rover maintenance across coalescing, header and footer tags, wrap-around search, allocation clustering statistics
 * deps: libc
 */
#define SEED 0x2E70F17ULL
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

static _Alignas(16) unsigned char heap[HEAP];
static unsigned rover;
static unsigned long steps, wraps, allocs, frees, merges, splits;

static uint32_t rd(unsigned off) { uint32_t v; memcpy(&v, heap + off, 4); return v; }
static void wr(unsigned off, uint32_t v) { memcpy(heap + off, &v, 4); }
static unsigned bsize(unsigned p) { return rd(p) & ~7u; }
static int bused(unsigned p) { return (int)(rd(p) & USED); }
static void setblk(unsigned p, unsigned size, int used) {
    uint32_t t = size | (used ? USED : 0);
    wr(p, t);
    wr(p + size - TAG, t);
}

static void heap_init(void) { setblk(0, HEAP, 0); rover = 0; }

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = (unsigned)((n + 7) & ~(size_t)7) + 2 * TAG;
    if (need < MINBLK) need = MINBLK;
    unsigned p = rover;
    int wrapped = 0;
    for (;;) {
        steps++;
        unsigned sz = bsize(p);
        if (!bused(p) && sz >= need) {
            if (sz - need >= MINBLK) {
                setblk(p, need, 1);
                setblk(p + need, sz - need, 0);
                rover = p + need;           /* next search continues after the allocation */
                splits++;
            } else {
                setblk(p, sz, 1);
                rover = p + sz < HEAP ? p + sz : 0;
            }
            allocs++;
            if (wrapped) wraps++;
            return (long)(p + TAG);
        }
        p += sz;
        if (p >= HEAP) {
            p = 0;
            if (wrapped) return -1;         /* second wrap: nothing fits */
            wrapped = 1;
        }
        if (wrapped && p >= rover && p != 0) return -1;
    }
}

static void h_free(unsigned payload) {
    unsigned p = payload - TAG, sz = bsize(p);
    CHECK(bused(p) && rd(p + sz - TAG) == rd(p));
    frees++;
    if (p + sz < HEAP && !bused(p + sz)) {
        unsigned nx = p + sz;
        if (rover == nx) rover = p;
        sz += bsize(nx);
        merges++;
    }
    if (p > 0 && !(rd(p - TAG) & USED)) {
        unsigned psz = rd(p - TAG) & ~7u;
        unsigned pp = p - psz;
        CHECK(!bused(pp) && bsize(pp) == psz);
        if (rover == p) rover = pp;
        p = pp; sz += psz;
        merges++;
    }
    setblk(p, sz, 0);
    if (rover > p && rover < p + sz) rover = p;
}

static void heap_check(unsigned *nblocks, unsigned *nfree, unsigned *largest) {
    unsigned p = 0, nb = 0, nf = 0, big = 0, rover_seen = 0;
    int prev_free = 0;
    while (p < HEAP) {
        unsigned sz = bsize(p);
        CHECK(sz >= MINBLK && (sz & 7) == 0 && p + sz <= HEAP);
        CHECK(rd(p) == rd(p + sz - TAG));
        if (p == rover) rover_seen = 1;
        if (!bused(p)) { CHECK(!prev_free); prev_free = 1; nf++; if (sz > big) big = sz; }
        else prev_free = 0;
        nb++; p += sz;
    }
    CHECK(p == HEAP && rover_seen);
    *nblocks = nb; *nfree = nf; *largest = big;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[600];
    int nlive = 0, fails = 0;
    unsigned tag = 1, nb, nf, big;
    /* record how far apart consecutive allocations land: next fit tends to sweep through the heap */
    long last_off = -1;
    unsigned long forward = 0, backward = 0;
    for (int step = 0; step < 12000; step++) {
        if (nlive == 0 || (rnd() % 100 < 51 && nlive < 600)) {
            size_t n = 1 + rnd() % 100;
            if (rnd() % 10 == 0) n += 200 + rnd() % 300;
            long off = h_alloc(n);
            if (off < 0) { fails++; continue; }
            if (last_off >= 0) { if (off > last_off) forward++; else backward++; }
            last_off = off;
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
            heap_check(&nb, &nf, &big);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    heap_check(&nb, &nf, &big);
    printf("allocs=%lu frees=%lu failed=%d live=%d\n", allocs, frees, fails, nlive);
    printf("search steps=%lu (%.2f per alloc) wrapped allocs=%lu\n", steps, (double)steps / (double)(allocs + (unsigned long)fails), wraps);
    printf("consecutive allocation direction: forward=%lu backward=%lu\n", forward, backward);
    printf("splits=%lu merges=%lu final blocks=%u free blocks=%u largest free=%u\n", splits, merges, nb, nf, big);
    for (int i = 0; i < nlive; i++) h_free(live[i].off);
    heap_check(&nb, &nf, &big);
    CHECK(nb == 1 && big == HEAP && rover == 0);
    printf("drained; rover back at %u\n", rover);
    return 0;
}
