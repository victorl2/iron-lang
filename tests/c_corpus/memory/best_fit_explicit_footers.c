/*
 * title: Best-fit with explicit free list and prev-in-use footers
 * topic: memory
 * covers: best fit, explicit doubly linked free list, footer only in free blocks, prev_inuse bit, immediate two-sided coalescing, exact-fit shortcut
 * deps: libc
 */
#define SEED 0xBE57F17ULL
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
#define MINBLK 24u          /* hdr + prev + next + footer */
#define F_USED 1u
#define F_PREV_USED 2u
#define NIL 0xFFFFFFFFu

static _Alignas(16) unsigned char heap[HEAP];
static uint32_t free_head = NIL;
static unsigned long search_steps, exact_hits, splits, merges_next, merges_prev, allocs, frees;
static unsigned free_blocks;

static uint32_t rd(unsigned off) { uint32_t v; memcpy(&v, heap + off, 4); return v; }
static void wr(unsigned off, uint32_t v) { memcpy(heap + off, &v, 4); }
static unsigned bsize(unsigned p) { return rd(p) & ~7u; }
static int bused(unsigned p) { return (int)(rd(p) & F_USED); }
static int bprev_used(unsigned p) { return (int)((rd(p) & F_PREV_USED) != 0); }
static void set_prev_used(unsigned p, int v) { wr(p, v ? rd(p) | F_PREV_USED : rd(p) & ~F_PREV_USED); }
static uint32_t lprev(unsigned p) { return rd(p + 8); }
static uint32_t lnext(unsigned p) { return rd(p + 12); }

static void list_insert(unsigned p) {
    wr(p + 8, NIL); wr(p + 12, free_head);
    if (free_head != NIL) wr(free_head + 8, p);
    free_head = p; free_blocks++;
}
static void list_remove(unsigned p) {
    uint32_t pv = lprev(p), nx = lnext(p);
    if (pv != NIL) wr(pv + 12, nx); else free_head = nx;
    if (nx != NIL) wr(nx + 8, pv);
    free_blocks--;
}
/* make p a free block of the given size: header keeps prev_used, footer written, next block sees prev free */
static void make_free(unsigned p, unsigned size) {
    unsigned pu = rd(p) & F_PREV_USED;
    wr(p, size | pu);
    wr(p + size - 4, size);
    if (p + size < HEAP) set_prev_used(p + size, 0);
    list_insert(p);
}

static void heap_init(void) { wr(0, 0); make_free(0, HEAP); wr(0, HEAP | F_PREV_USED); }

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = (unsigned)((n + 7) & ~(size_t)7) + HDR;
    if (need < MINBLK) need = MINBLK;
    uint32_t best = NIL;
    for (uint32_t p = free_head; p != NIL; p = lnext(p)) {
        search_steps++;
        unsigned sz = bsize(p);
        if (sz < need) continue;
        if (best == NIL || sz < bsize(best)) best = p;
        if (sz == need) { exact_hits++; break; }
    }
    if (best == NIL) return -1;
    unsigned sz = bsize(best);
    list_remove(best);
    if (sz - need >= MINBLK) {
        unsigned pu = rd(best) & F_PREV_USED;
        wr(best, need | F_USED | pu);
        wr(best + need, 0);      /* placeholder header for remainder, prev_used set by make_free */
        make_free(best + need, sz - need);
        set_prev_used(best + need, 1);
        splits++;
    } else {
        wr(best, rd(best) | F_USED);
        if (best + sz < HEAP) set_prev_used(best + sz, 1);
    }
    allocs++;
    return (long)(best + HDR);
}

static void h_free(unsigned payload) {
    unsigned p = payload - HDR, sz = bsize(p);
    CHECK(bused(p));
    frees++;
    unsigned pu = rd(p) & F_PREV_USED;
    if (p + sz < HEAP && !bused(p + sz)) {           /* merge with next */
        list_remove(p + sz);
        sz += bsize(p + sz);
        merges_next++;
    }
    if (!pu) {                                        /* merge with previous via its footer */
        unsigned psz = rd(p - 4);
        unsigned pp = p - psz;
        CHECK(!bused(pp) && bsize(pp) == psz);
        list_remove(pp);
        p = pp; sz += psz;
        pu = rd(pp) & F_PREV_USED;
        merges_prev++;
    }
    wr(p, sz | pu);
    make_free(p, sz);
}

static void heap_check(unsigned *nfree, unsigned *freebytes, unsigned *largest) {
    unsigned p = 0, nf = 0, fb = 0, big = 0;
    int prev_used = 1;
    while (p < HEAP) {
        unsigned sz = bsize(p);
        CHECK(sz >= MINBLK && (sz & 7) == 0 && p + sz <= HEAP);
        CHECK(bprev_used(p) == prev_used);
        if (bused(p)) prev_used = 1;
        else {
            CHECK(prev_used == 1 || p == 0);   /* no two adjacent free blocks */
            CHECK(rd(p + sz - 4) == sz);
            nf++; fb += sz; if (sz > big) big = sz;
            prev_used = 0;
        }
        p += sz;
    }
    CHECK(p == HEAP);
    unsigned ln = 0;
    uint32_t pv = NIL;
    for (uint32_t q = free_head; q != NIL; q = lnext(q)) {
        CHECK(!bused(q) && lprev(q) == pv);
        pv = q; ln++;
        CHECK(ln <= nf);
    }
    CHECK(ln == nf && nf == free_blocks);
    *nfree = nf; *freebytes = fb; *largest = big;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[700];
    int nlive = 0, fails = 0;
    unsigned tag = 1, nf, fb, big;
    for (int step = 0; step < 12000; step++) {
        if (nlive == 0 || (rnd() % 100 < 53 && nlive < 700)) {
            size_t n = 1 + rnd() % 120;
            if (rnd() % 12 == 0) n += 250 + rnd() % 500;
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
        if (step == 5999) {
            heap_check(&nf, &fb, &big);
            printf("mid-run: live=%d free blocks=%u free bytes=%u largest=%u\n", nlive, nf, fb, big);
            printf("mid-run external fragmentation=%u per mille\n", fb ? 1000 - 1000 * big / fb : 0);
        }
    }
    printf("allocs=%lu frees=%lu failed=%d\n", allocs, frees, fails);
    printf("search steps=%lu exact hits=%lu splits=%lu merges next=%lu prev=%lu\n", search_steps, exact_hits, splits, merges_next, merges_prev);
    for (int i = 0; i < nlive; i++) h_free(live[i].off);
    heap_check(&nf, &fb, &big);
    CHECK(nf == 1 && fb == HEAP);
    printf("drained: one free block of %u bytes\n", big);
    return 0;
}
