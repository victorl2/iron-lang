/*
 * title: Page allocator with a small-object sub-allocator layered on top
 * topic: memory
 * covers: layered allocators, contiguous page runs, per-class page headers, partial page lists, page release on empty, large direct page allocations, page kind table
 * deps: libc
 */
#define SEED 0x9A6E5AULL
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

#define PAGE 256u
#define NPAGES 96u
#define PHDR 16u
#define NCLS 6
#define NILP 0xFFFFu

static _Alignas(16) unsigned char mem[NPAGES * PAGE];

/* ---------- layer 1: page allocator (first-fit over a page table) ---------- */
enum { PK_FREE, PK_SMALL, PK_LARGE_HEAD, PK_LARGE_TAIL };
static unsigned char page_kind[NPAGES];
static unsigned char page_class[NPAGES];      /* size class for PK_SMALL pages */
static uint16_t large_len[NPAGES];            /* run length at PK_LARGE_HEAD */
static unsigned pages_in_use, pages_peak;
static unsigned long page_alloc_calls, page_free_calls;

static long page_alloc(unsigned n) {
    unsigned run = 0;
    for (unsigned i = 0; i < NPAGES; i++) {
        run = page_kind[i] == PK_FREE ? run + 1 : 0;
        if (run == n) {
            unsigned s = i + 1 - n;
            for (unsigned k = s; k < s + n; k++) page_kind[k] = PK_LARGE_TAIL;  /* caller re-labels */
            pages_in_use += n;
            if (pages_in_use > pages_peak) pages_peak = pages_in_use;
            page_alloc_calls++;
            return (long)s;
        }
    }
    return -1;
}
static void page_free_run(unsigned s, unsigned n) {
    for (unsigned k = s; k < s + n; k++) { CHECK(page_kind[k] != PK_FREE); page_kind[k] = PK_FREE; }
    memset(mem + (size_t)s * PAGE, 0xDD, (size_t)n * PAGE);
    pages_in_use -= n;
    page_free_calls++;
}

/* ---------- layer 2: size classes packed into single pages ---------- */
static const unsigned cls_size[NCLS] = { 16, 24, 32, 48, 64, 96 };
/* page header: u16 cls, u16 nfree, u16 free_head (byte offset in page or 0), u16 prev, u16 next, u16 inuse */
static uint16_t partial_head[NCLS];
static unsigned small_live[NCLS], small_pages[NCLS];
static unsigned long small_allocs, large_allocs;

static uint16_t hget(unsigned pg, unsigned f) { uint16_t v; memcpy(&v, mem + (size_t)pg * PAGE + f * 2, 2); return v; }
static void hput(unsigned pg, unsigned f, unsigned v) { uint16_t x = (uint16_t)v; memcpy(mem + (size_t)pg * PAGE + f * 2, &x, 2); }
enum { H_CLS, H_NFREE, H_HEAD, H_PREV, H_NEXT, H_INUSE };

static void partial_unlink(unsigned pg) {
    unsigned c = hget(pg, H_CLS), pv = hget(pg, H_PREV), nx = hget(pg, H_NEXT);
    if (pv != NILP) hput(pv, H_NEXT, nx); else partial_head[c] = (uint16_t)nx;
    if (nx != NILP) hput(nx, H_PREV, pv);
    hput(pg, H_PREV, NILP); hput(pg, H_NEXT, NILP);
}
static void partial_push(unsigned pg) {
    unsigned c = hget(pg, H_CLS);
    unsigned h = partial_head[c];
    hput(pg, H_PREV, NILP); hput(pg, H_NEXT, h);
    if (h != NILP) hput(h, H_PREV, pg);
    partial_head[c] = (uint16_t)pg;
}

static int new_small_page(int c) {
    long pg = page_alloc(1);
    if (pg < 0) return -1;
    unsigned p = (unsigned)pg;
    page_kind[p] = PK_SMALL; page_class[p] = (unsigned char)c;
    unsigned n = (PAGE - PHDR) / cls_size[c];
    hput(p, H_CLS, (unsigned)c); hput(p, H_NFREE, n); hput(p, H_INUSE, 0);
    for (unsigned i = 0; i < n; i++) {
        unsigned off = PHDR + i * cls_size[c];
        unsigned nx = i + 1 < n ? off + cls_size[c] : 0;
        uint16_t v = (uint16_t)nx;
        memcpy(mem + (size_t)p * PAGE + off, &v, 2);
    }
    hput(p, H_HEAD, PHDR);
    partial_push(p);
    small_pages[c]++;
    return (int)p;
}

static long sub_alloc(size_t n) {
    if (n > cls_size[NCLS - 1]) {
        unsigned np = (unsigned)((n + PAGE - 1) / PAGE);
        long pg = page_alloc(np);
        if (pg < 0) return -1;
        page_kind[pg] = PK_LARGE_HEAD; large_len[pg] = (uint16_t)np;
        large_allocs++;
        return pg * (long)PAGE;
    }
    int c = 0;
    while (cls_size[c] < n) c++;
    int pg = partial_head[c] != NILP ? (int)partial_head[c] : new_small_page(c);
    if (pg < 0) return -1;
    unsigned p = (unsigned)pg;
    unsigned off = hget(p, H_HEAD);
    uint16_t nx;
    memcpy(&nx, mem + (size_t)p * PAGE + off, 2);
    hput(p, H_HEAD, nx);
    hput(p, H_NFREE, hget(p, H_NFREE) - 1u);
    hput(p, H_INUSE, hget(p, H_INUSE) + 1u);
    if (hget(p, H_NFREE) == 0) partial_unlink(p);
    small_live[c]++; small_allocs++;
    return (long)((size_t)p * PAGE + off);
}

static void sub_free(unsigned byte_off) {
    unsigned p = byte_off / PAGE, off = byte_off % PAGE;
    if (page_kind[p] == PK_LARGE_HEAD) {
        CHECK(off == 0);
        unsigned len = large_len[p];
        large_len[p] = 0;
        page_free_run(p, len);
        return;
    }
    CHECK(page_kind[p] == PK_SMALL && off >= PHDR);
    unsigned c = hget(p, H_CLS);
    CHECK((off - PHDR) % cls_size[c] == 0);
    unsigned was_full = hget(p, H_NFREE) == 0;
    uint16_t hd = (uint16_t)hget(p, H_HEAD);
    memcpy(mem + byte_off, &hd, 2);
    hput(p, H_HEAD, off);
    hput(p, H_NFREE, hget(p, H_NFREE) + 1u);
    hput(p, H_INUSE, hget(p, H_INUSE) - 1u);
    small_live[c]--;
    if (was_full) partial_push(p);
    if (hget(p, H_INUSE) == 0) {             /* page empty: give it back to the page allocator */
        partial_unlink(p);
        small_pages[c]--;
        page_kind[p] = PK_SMALL;
        page_free_run(p, 1);
    }
}

static void check_layers(unsigned *fr_pages, unsigned *big_run) {
    unsigned used = 0, run = 0, best = 0, fr = 0;
    for (unsigned i = 0; i < NPAGES; i++) {
        if (page_kind[i] == PK_FREE) { fr++; run++; if (run > best) best = run; }
        else { used++; run = 0; }
        if (page_kind[i] == PK_LARGE_HEAD) for (unsigned k = 1; k < large_len[i]; k++) CHECK(page_kind[i + k] == PK_LARGE_TAIL);
        if (page_kind[i] == PK_SMALL) {
            unsigned c = hget(i, H_CLS), n = (PAGE - PHDR) / cls_size[c];
            CHECK(hget(i, H_NFREE) + hget(i, H_INUSE) == n && hget(i, H_INUSE) > 0);
            unsigned cnt = 0;
            for (unsigned o = hget(i, H_HEAD); o; ) { cnt++; CHECK(cnt <= n); uint16_t v; memcpy(&v, mem + (size_t)i * PAGE + o, 2); o = v; }
            CHECK(cnt == hget(i, H_NFREE));
        }
    }
    CHECK(used == pages_in_use);
    for (int c = 0; c < NCLS; c++) {
        for (unsigned p = partial_head[c]; p != NILP; p = hget(p, H_NEXT)) CHECK(page_kind[p] == PK_SMALL && hget(p, H_NFREE) > 0 && hget(p, H_CLS) == (unsigned)c);
    }
    *fr_pages = fr; *big_run = best;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    for (int c = 0; c < NCLS; c++) partial_head[c] = NILP;
    Rec live[1500];
    int nlive = 0, fails = 0;
    unsigned tag = 1, fr, big;
    for (int step = 0; step < 15000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 1500)) {
            unsigned k = rnd() % 100;
            size_t n = k < 90 ? 1 + rnd() % 96 : 97 + rnd() % 600;
            long off = sub_alloc(n);
            if (off < 0) { fails++; continue; }
            pat_fill(mem + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(mem + live[i].off, live[i].n, live[i].tag));
            sub_free(live[i].off);
            live[i] = live[--nlive];
        }
        if (step % 200 == 0) check_layers(&fr, &big);
    }
    check_layers(&fr, &big);
    printf("small allocs=%lu large allocs=%lu failed=%d live=%d\n", small_allocs, large_allocs, fails, nlive);
    printf("page allocator calls=%lu frees=%lu peak pages=%u of %u\n", page_alloc_calls, page_free_calls, pages_peak, NPAGES);
    for (int c = 0; c < NCLS; c++) printf("class %2u: pages=%2u live objects=%3u\n", cls_size[c], small_pages[c], small_live[c]);
    printf("free pages=%u largest free run=%u\n", fr, big);
    for (int i = 0; i < nlive; i++) sub_free(live[i].off);
    check_layers(&fr, &big);
    CHECK(pages_in_use == 0 && fr == NPAGES && big == NPAGES);
    for (int c = 0; c < NCLS; c++) CHECK(partial_head[c] == NILP && small_pages[c] == 0);
    printf("drained: all %u pages back with the page allocator\n", fr);
    return 0;
}
