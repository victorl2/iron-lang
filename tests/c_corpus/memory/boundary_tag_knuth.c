/*
 * title: Knuth boundary-tag allocator with circular free list
 * topic: memory
 * covers: boundary tags, circular doubly linked available list, allocation from the upper end of a free block, four-case coalescing, sentinel tags, TAOCP 2.5 algorithms
 * deps: libc
 */
#define SEED 0x4B4E555448ULL
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
#define NIL 0xFFFFFFFFu
#define MINBLK 16u
#define RESERVED 1u

static _Alignas(16) unsigned char heap[HEAP];
static uint32_t avail = NIL;    /* rover into the circular list of free blocks */
static unsigned nfree_list;
static unsigned long visits, allocs, frees, case_count[4], upper_splits, whole_takes;

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, heap + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(heap + o, &v, 4); }
static unsigned size_at(unsigned p) { return rd(p) & ~3u; }
static uint32_t linkf(unsigned p) { return rd(p + 4); }
static uint32_t linkb(unsigned p) { return rd(p + 8); }

static void tag_free(unsigned p, unsigned size) { wr(p, size); wr(p + size - 4, size); }
static void tag_res(unsigned p, unsigned size) { wr(p, size | RESERVED); wr(p + size - 4, size | RESERVED); }

static void list_insert(unsigned p) {
    if (avail == NIL) { wr(p + 4, p); wr(p + 8, p); }
    else {
        uint32_t nx = linkf(avail);
        wr(p + 4, nx); wr(p + 8, avail);
        wr(avail + 4, p); wr(nx + 8, p);
    }
    avail = p; nfree_list++;
}
static void list_remove(unsigned p) {
    if (linkf(p) == p) avail = NIL;
    else {
        uint32_t f = linkf(p), b = linkb(p);
        wr(b + 4, f); wr(f + 8, b);
        if (avail == p) avail = f;
    }
    nfree_list--;
}

static void heap_init(void) {
    wr(0, RESERVED);                       /* left sentinel: reserved, size 0 */
    wr(HEAP - 4, RESERVED);                /* right sentinel */
    tag_free(4, HEAP - 8);
    list_insert(4);
}

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = (unsigned)((n + 3) & ~(size_t)3) + 8;
    if (need < MINBLK) need = MINBLK;
    if (avail == NIL) return -1;
    uint32_t p = avail;
    do {
        visits++;
        unsigned sz = size_at(p);
        if (sz >= need) {
            if (sz - need < MINBLK) {          /* take the whole block */
                list_remove(p);
                tag_res(p, sz);
                whole_takes++;
                allocs++;
                return (long)(p + 4);
            }
            unsigned rem = sz - need;          /* reserve the UPPER part; free block keeps its list position */
            tag_free(p, rem);
            tag_res(p + rem, need);
            avail = p;
            upper_splits++;
            allocs++;
            return (long)(p + rem + 4);
        }
        p = linkf(p);
    } while (p != avail);
    return -1;
}

static void h_free(unsigned payload) {
    unsigned p = payload - 4, sz = size_at(p);
    CHECK((rd(p) & RESERVED) && rd(p + sz - 4) == rd(p));
    frees++;
    int prev_free = !(rd(p - 4) & RESERVED);
    int next_free = !(rd(p + sz) & RESERVED);
    case_count[prev_free * 2 + next_free]++;
    if (next_free) {
        unsigned nx = p + sz;
        sz += size_at(nx);
        list_remove(nx);
    }
    if (prev_free) {
        unsigned psz = rd(p - 4) & ~3u;
        unsigned pp = p - psz;
        sz += psz;
        p = pp;
        list_remove(pp);
    }
    tag_free(p, sz);
    list_insert(p);
}

static void heap_check(unsigned *nfb, unsigned *fbytes, unsigned *largest) {
    unsigned p = 4, nf = 0, fb = 0, big = 0;
    int prev_free = 0;
    CHECK(rd(0) == RESERVED && rd(HEAP - 4) == RESERVED);
    while (p < HEAP - 4) {
        unsigned sz = size_at(p);
        CHECK(sz >= MINBLK && p + sz <= HEAP - 4);
        CHECK(rd(p) == rd(p + sz - 4));
        if (rd(p) & RESERVED) prev_free = 0;
        else { CHECK(!prev_free); prev_free = 1; nf++; fb += sz; if (sz > big) big = sz; }
        p += sz;
    }
    CHECK(p == HEAP - 4);
    unsigned ln = 0;
    if (avail != NIL) {
        uint32_t q = avail;
        do {
            CHECK(!(rd(q) & RESERVED) && linkb(linkf(q)) == q && linkf(linkb(q)) == q);
            ln++; CHECK(ln <= nf);
            q = linkf(q);
        } while (q != avail);
    }
    CHECK(ln == nf && nf == nfree_list);
    *nfb = nf; *fbytes = fb; *largest = big;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[700];
    int nlive = 0, fails = 0;
    unsigned tag = 1, nf, fb, big;
    for (int step = 0; step < 12000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 700)) {
            size_t n = 1 + rnd() % 110;
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
    }
    heap_check(&nf, &fb, &big);
    printf("allocs=%lu frees=%lu failed=%d live=%d\n", allocs, frees, fails, nlive);
    printf("list visits=%lu upper splits=%lu whole-block takes=%lu\n", visits, upper_splits, whole_takes);
    printf("coalesce cases: neither=%lu next=%lu prev=%lu both=%lu\n", case_count[0], case_count[1], case_count[2], case_count[3]);
    printf("free blocks=%u free bytes=%u largest=%u\n", nf, fb, big);
    for (int i = 0; i < nlive; i++) h_free(live[i].off);
    heap_check(&nf, &fb, &big);
    CHECK(nf == 1 && fb == HEAP - 8);
    printf("drained: single block of %u bytes\n", fb);
    return 0;
}
