/*
 * title: TLSF-style two-level segregated fit allocator
 * topic: memory
 * covers: two-level segregated fit, first/second level index mapping, bitmap search, round-up for good fit, physical neighbour coalescing, O(1) alloc and free
 * deps: libc
 */
#define SEED 0x71A5F17ULL
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

#define HEAP 16384u
#define HDR 8u
#define MINBLK 32u
#define FL_MIN 5              /* 32 bytes */
#define FL_COUNT 10           /* 32 .. 16383 */
#define SLI 2
#define SL_COUNT (1u << SLI)
#define F_FREE 1u
#define F_PREV_FREE 2u
#define NIL 0xFFFFFFFFu

static _Alignas(16) unsigned char heap[HEAP];
static uint32_t bins[FL_COUNT][SL_COUNT];
static unsigned fl_bitmap;
static unsigned sl_bitmap[FL_COUNT];
static unsigned long allocs, frees, splits, merges, bitmap_hops, bin_use[FL_COUNT][SL_COUNT];

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, heap + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(heap + o, &v, 4); }
static unsigned bsize(unsigned p) { return rd(p) & ~7u; }
static int bfree(unsigned p) { return (int)(rd(p) & F_FREE); }
static void set_prev_free(unsigned p, int v) { wr(p, v ? rd(p) | F_PREV_FREE : rd(p) & ~F_PREV_FREE); }

static int ilog2(unsigned x) { int r = 0; while (x >>= 1) r++; return r; }
static int lowest_bit(unsigned x) { int r = 0; while (!(x & 1u)) { x >>= 1; r++; } return r; }

static void mapping_insert(unsigned size, int *fl, int *sl) {
    int f = ilog2(size);
    *sl = (int)((size >> (f - SLI)) & (SL_COUNT - 1));
    *fl = f - FL_MIN;
}
static void mapping_search(unsigned size, int *fl, int *sl) {
    int f = ilog2(size);
    size += (1u << (f - SLI)) - 1;   /* round up to the next class boundary: any block in that class fits */
    mapping_insert(size, fl, sl);
}

/* free block layout: [hdr 4][pad 4][next 4][prev 4] ... [footer 4] */
static void bin_insert(unsigned p) {
    int fl, sl;
    mapping_insert(bsize(p), &fl, &sl);
    uint32_t h = bins[fl][sl];
    wr(p + 8, h); wr(p + 12, NIL);
    if (h != NIL) wr(h + 12, p);
    bins[fl][sl] = p;
    fl_bitmap |= 1u << fl; sl_bitmap[fl] |= 1u << sl;
}
static void bin_remove(unsigned p) {
    int fl, sl;
    mapping_insert(bsize(p), &fl, &sl);
    uint32_t nx = rd(p + 8), pv = rd(p + 12);
    if (pv != NIL) wr(pv + 8, nx); else bins[fl][sl] = nx;
    if (nx != NIL) wr(nx + 12, pv);
    if (bins[fl][sl] == NIL) {
        sl_bitmap[fl] &= ~(1u << sl);
        if (!sl_bitmap[fl]) fl_bitmap &= ~(1u << fl);
    }
}
static void make_free(unsigned p, unsigned size, unsigned prev_free_flag) {
    wr(p, size | F_FREE | prev_free_flag);
    wr(p + size - 4, size);
    if (p + size < HEAP) set_prev_free(p + size, 1);
    bin_insert(p);
}

static void tlsf_init(void) {
    for (int f = 0; f < FL_COUNT; f++) for (unsigned s = 0; s < SL_COUNT; s++) bins[f][s] = NIL;
    make_free(0, HEAP, 0);
}

static uint32_t find_suitable(int fl, int sl) {
    unsigned slm = sl_bitmap[fl] & (~0u << sl);
    if (!slm) {
        bitmap_hops++;
        unsigned flm = fl_bitmap & (~0u << (fl + 1));
        if (!flm) return NIL;
        fl = lowest_bit(flm);
        slm = sl_bitmap[fl];
    }
    sl = lowest_bit(slm);
    bin_use[fl][sl]++;
    return bins[fl][sl];
}

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = (unsigned)((n + 7) & ~(size_t)7) + HDR;
    if (need < MINBLK) need = MINBLK;
    if (need >= HEAP) return -1;
    int fl, sl;
    mapping_search(need, &fl, &sl);
    if (fl >= FL_COUNT) return -1;
    uint32_t p = find_suitable(fl, sl);
    if (p == NIL) return -1;
    unsigned sz = bsize(p);
    CHECK(sz >= need);
    bin_remove(p);
    unsigned pf = rd(p) & F_PREV_FREE;
    if (sz - need >= MINBLK) {
        wr(p, need | pf);
        make_free(p + need, sz - need, 0);
        set_prev_free(p + need, 0);
        wr(p + need, (rd(p + need) & ~F_PREV_FREE));
        splits++;
    } else {
        wr(p, sz | pf);
        if (p + sz < HEAP) set_prev_free(p + sz, 0);
    }
    allocs++;
    return (long)(p + HDR);
}

static void h_free(unsigned payload) {
    unsigned p = payload - HDR, sz = bsize(p);
    CHECK(!bfree(p));
    frees++;
    unsigned pf = rd(p) & F_PREV_FREE;
    if (p + sz < HEAP && bfree(p + sz)) {
        bin_remove(p + sz);
        sz += bsize(p + sz);
        merges++;
    }
    if (pf) {
        unsigned psz = rd(p - 4);
        unsigned pp = p - psz;
        CHECK(bfree(pp) && bsize(pp) == psz);
        bin_remove(pp);
        p = pp; sz += psz;
        pf = rd(pp) & F_PREV_FREE;
        merges++;
    }
    make_free(p, sz, pf);
}

static void heap_check(unsigned *nf, unsigned *fbytes, unsigned *largest) {
    unsigned p = 0, cnt = 0, fb = 0, big = 0;
    int prev_free = 0;
    while (p < HEAP) {
        unsigned sz = bsize(p);
        CHECK(sz >= MINBLK && (sz & 7) == 0 && p + sz <= HEAP);
        CHECK(((rd(p) & F_PREV_FREE) != 0) == prev_free);
        if (bfree(p)) {
            CHECK(!prev_free && rd(p + sz - 4) == sz);
            cnt++; fb += sz; if (sz > big) big = sz;
            prev_free = 1;
        } else prev_free = 0;
        p += sz;
    }
    CHECK(p == HEAP);
    unsigned listed = 0;
    for (int f = 0; f < FL_COUNT; f++) for (unsigned s = 0; s < SL_COUNT; s++) {
        int fbit = (fl_bitmap >> f) & 1, sbit = (sl_bitmap[f] >> s) & 1;
        CHECK(sbit == (bins[f][s] != NIL));
        if (!sl_bitmap[f]) CHECK(!fbit); else CHECK(fbit);
        uint32_t pv = NIL;
        for (uint32_t q = bins[f][s]; q != NIL; q = rd(q + 8)) {
            int f2, s2;
            mapping_insert(bsize(q), &f2, &s2);
            CHECK(f2 == f && (unsigned)s2 == s && bfree(q) && rd(q + 12) == pv);
            pv = q; listed++;
        }
    }
    CHECK(listed == cnt);
    *nf = cnt; *fbytes = fb; *largest = big;
}

/* good-fit guarantee: a request may fail only if no free block reaches the rounded-up class */
static int any_block_at_least(unsigned need) {
    for (unsigned p = 0; p < HEAP; p += bsize(p)) if (bfree(p) && bsize(p) >= need) return 1;
    return 0;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    tlsf_init();
    Rec live[800];
    int nlive = 0, fails = 0, legit_fails = 0;
    unsigned tag = 1, nf, fb, big;
    for (int step = 0; step < 12000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 800)) {
            size_t n = 1 + rnd() % 150;
            if (rnd() % 10 == 0) n += 200 + rnd() % 900;
            long off = h_alloc(n);
            if (off < 0) {
                fails++;
                unsigned need = (unsigned)((n + 7) & ~(size_t)7) + HDR;
                if (need < MINBLK) need = MINBLK;
                int fl, sl;
                mapping_search(need, &fl, &sl);
                /* rounded-up request: nothing >= the next class boundary exists, or it is over the top level */
                unsigned rounded = need + (1u << (ilog2(need) - SLI)) - 1;
                rounded &= ~((1u << (ilog2(need) - SLI)) - 1);
                CHECK(fl >= FL_COUNT || !any_block_at_least(rounded));
                legit_fails++;
                continue;
            }
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
    printf("allocs=%lu frees=%lu failed=%d (all justified: %d)\n", allocs, frees, fails, legit_fails);
    printf("splits=%lu merges=%lu first-level hops=%lu\n", splits, merges, bitmap_hops);
    printf("free blocks=%u free bytes=%u largest=%u fl_bitmap=%03x\n", nf, fb, big, fl_bitmap);
    printf("bin hits by first level (32<<fl): ");
    for (int f = 0; f < FL_COUNT; f++) {
        unsigned long t = 0;
        for (unsigned s = 0; s < SL_COUNT; s++) t += bin_use[f][s];
        printf("%lu%s", t, f + 1 < FL_COUNT ? " " : "\n");
    }
    for (int i = 0; i < nlive; i++) h_free(live[i].off);
    heap_check(&nf, &fb, &big);
    CHECK(nf == 1 && fb == HEAP);
    printf("drained: single block of %u bytes in bin fl=%d\n", big, ilog2(big) - FL_MIN);
    return 0;
}
