/*
 * title: Buddy allocator with per-order free lists
 * topic: memory
 * covers: buddy system, split and coalesce, per-order doubly linked free lists, buddy address xor, internal fragmentation, heap walk checker
 * deps: libc
 */
#define SEED 0xB0DD1E5ULL
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

#define MIN_ORDER 4      /* 16 bytes */
#define MAX_ORDER 12     /* 4096 bytes */
#define HEAP (1u << MAX_ORDER)
#define UNITS (HEAP >> MIN_ORDER)
#define NONE 0xFFFFFFFFu

static _Alignas(16) unsigned char heap[HEAP];
/* per-unit metadata for the block that STARTS at this unit */
static signed char blk_order[UNITS];  /* -1 if the unit is not a block start */
static unsigned char blk_free[UNITS];
static uint32_t fhead[MAX_ORDER + 1];
static size_t req_bytes_live, blk_bytes_live, peak_blk, splits, merges;

static void fl_link(unsigned off, int order) {
    uint32_t nx = fhead[order], pv = NONE;
    memcpy(heap + off, &pv, 4);
    memcpy(heap + off + 4, &nx, 4);
    if (nx != NONE) memcpy(heap + nx, &off, 4);
    fhead[order] = off;
    blk_order[off >> MIN_ORDER] = (signed char)order;
    blk_free[off >> MIN_ORDER] = 1;
}
static void fl_unlink(unsigned off, int order) {
    uint32_t pv, nx;
    memcpy(&pv, heap + off, 4);
    memcpy(&nx, heap + off + 4, 4);
    if (pv != NONE) memcpy(heap + pv + 4, &nx, 4); else fhead[order] = nx;
    if (nx != NONE) memcpy(heap + nx, &pv, 4);
    blk_free[off >> MIN_ORDER] = 0;
}

static void buddy_init(void) {
    for (unsigned i = 0; i < UNITS; i++) blk_order[i] = -1;
    for (int o = 0; o <= MAX_ORDER; o++) fhead[o] = NONE;
    fl_link(0, MAX_ORDER);
}

static int order_for(size_t n) {
    int o = MIN_ORDER;
    while (((size_t)1 << o) < n) o++;
    return o;
}

static long buddy_alloc(size_t n) {
    if (n == 0 || n > HEAP) return -1;
    int want = order_for(n), o = want;
    while (o <= MAX_ORDER && fhead[o] == NONE) o++;
    if (o > MAX_ORDER) return -1;
    unsigned off = fhead[o];
    fl_unlink(off, o);
    while (o > want) {
        o--;
        splits++;
        fl_link(off + (1u << o), o);   /* upper half becomes free */
    }
    blk_order[off >> MIN_ORDER] = (signed char)want;
    blk_free[off >> MIN_ORDER] = 0;
    req_bytes_live += n;
    blk_bytes_live += (size_t)1 << want;
    if (blk_bytes_live > peak_blk) peak_blk = blk_bytes_live;
    return (long)off;
}

static void buddy_free(unsigned off, size_t n) {
    int o = blk_order[off >> MIN_ORDER];
    CHECK(o >= MIN_ORDER && !blk_free[off >> MIN_ORDER]);
    req_bytes_live -= n;
    blk_bytes_live -= (size_t)1 << o;
    while (o < MAX_ORDER) {
        unsigned buddy = off ^ (1u << o);
        if (!blk_free[buddy >> MIN_ORDER] || blk_order[buddy >> MIN_ORDER] != o) break;
        fl_unlink(buddy, o);
        blk_order[buddy >> MIN_ORDER] = -1;
        if (buddy < off) { blk_order[off >> MIN_ORDER] = -1; off = buddy; }
        o++;
        merges++;
    }
    fl_link(off, o);
}

/* walk the heap block by block and compare against the free lists */
static void buddy_check(size_t *nfree_blocks, size_t *free_bytes, size_t *largest) {
    size_t pos = 0, fb = 0, fbytes = 0, big = 0;
    while (pos < HEAP) {
        int o = blk_order[pos >> MIN_ORDER];
        CHECK(o >= MIN_ORDER && o <= MAX_ORDER);
        CHECK((pos & (((size_t)1 << o) - 1)) == 0);
        if (blk_free[pos >> MIN_ORDER]) { fb++; fbytes += (size_t)1 << o; if (((size_t)1 << o) > big) big = (size_t)1 << o; }
        pos += (size_t)1 << o;
    }
    CHECK(pos == HEAP);
    size_t listed = 0;
    for (int o = MIN_ORDER; o <= MAX_ORDER; o++) {
        uint32_t prev = NONE;
        for (uint32_t b = fhead[o]; b != NONE; ) {
            CHECK(blk_free[b >> MIN_ORDER] && blk_order[b >> MIN_ORDER] == o);
            uint32_t pv, nx;
            memcpy(&pv, heap + b, 4); memcpy(&nx, heap + b + 4, 4);
            CHECK(pv == prev);
            prev = b; b = nx; listed++;
        }
    }
    CHECK(listed == fb);
    CHECK(fbytes + blk_bytes_live == HEAP);
    *nfree_blocks = fb; *free_bytes = fbytes; *largest = big;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    buddy_init();
    Rec live[UNITS];
    int nlive = 0, fails = 0;
    unsigned tag = 1;
    size_t nfb, fbytes, big;
    for (int step = 0; step < 8000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < (int)UNITS)) {
            unsigned k = rnd() % 10;
            size_t n = k < 6 ? 1 + rnd() % 60 : k < 9 ? 61 + rnd() % 300 : 361 + rnd() % 1200;
            long off = buddy_alloc(n);
            if (off < 0) { fails++; continue; }
            pat_fill(heap + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            buddy_free(live[i].off, live[i].n);
            live[i] = live[--nlive];
        }
        if (step % 97 == 0) {
            buddy_check(&nfb, &fbytes, &big);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
        if (step == 3999) {
            buddy_check(&nfb, &fbytes, &big);
            printf("mid-run: live=%d free blocks=%zu free bytes=%zu largest=%zu\n", nlive, nfb, fbytes, big);
            printf("mid-run internal fragmentation=%zu bytes (%zu block vs %zu requested)\n", blk_bytes_live - req_bytes_live, blk_bytes_live, req_bytes_live);
        }
    }
    printf("splits=%zu merges=%zu failed allocs=%d peak block bytes=%zu\n", splits, merges, fails, peak_blk);
    for (int i = 0; i < nlive; i++) buddy_free(live[i].off, live[i].n);
    buddy_check(&nfb, &fbytes, &big);
    CHECK(nfb == 1 && big == HEAP && fhead[MAX_ORDER] == 0);
    printf("after freeing everything: single free block of %zu bytes\n", big);
    return 0;
}
