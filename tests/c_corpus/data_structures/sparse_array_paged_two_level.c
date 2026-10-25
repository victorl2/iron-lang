/*
 * title: Sparse array with a lazily allocated two-level page table
 * topic: data_structures
 * covers: sparse array, page directory, lazily allocated pages, per-page occupancy count, page reclamation when empty, huge index space, iteration in index order, default values, hash-map oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define PAGE_BITS 8
#define PAGE_SIZE (1u << PAGE_BITS)
#define DIR_BITS 12
#define DIR_SIZE (1u << DIR_BITS)      /* index space: 2^20 slots */
#define SPACE (1u << (PAGE_BITS + DIR_BITS))

static unsigned long long rs = 0x5A85E7ULL * 0x9E3779B9ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int32_t v[PAGE_SIZE]; uint32_t present[PAGE_SIZE / 32]; int used; } Page;
typedef struct { Page *dir[DIR_SIZE]; long size; int pages, peak_pages; long allocs, frees; } Sparse;

static int has(const Sparse *s, uint32_t i) {
    const Page *p = s->dir[i >> PAGE_BITS]; uint32_t o = i & (PAGE_SIZE - 1);
    return p && ((p->present[o >> 5] >> (o & 31)) & 1u);
}
static int32_t get(const Sparse *s, uint32_t i, int32_t dflt) {
    const Page *p = s->dir[i >> PAGE_BITS]; uint32_t o = i & (PAGE_SIZE - 1);
    return (p && ((p->present[o >> 5] >> (o & 31)) & 1u)) ? p->v[o] : dflt;
}
static void put(Sparse *s, uint32_t i, int32_t v) {
    Page **pp = &s->dir[i >> PAGE_BITS]; uint32_t o = i & (PAGE_SIZE - 1);
    if (!*pp) { *pp = calloc(1, sizeof(Page)); s->pages++; s->allocs++; if (s->pages > s->peak_pages) s->peak_pages = s->pages; }
    Page *p = *pp;
    if (!((p->present[o >> 5] >> (o & 31)) & 1u)) { p->present[o >> 5] |= 1u << (o & 31); p->used++; s->size++; }
    p->v[o] = v;
}
static int del(Sparse *s, uint32_t i) {
    Page **pp = &s->dir[i >> PAGE_BITS]; uint32_t o = i & (PAGE_SIZE - 1);
    Page *p = *pp;
    if (!p || !((p->present[o >> 5] >> (o & 31)) & 1u)) return 0;
    p->present[o >> 5] &= ~(1u << (o & 31)); p->used--; s->size--;
    if (p->used == 0) { free(p); *pp = NULL; s->pages--; s->frees++; }
    return 1;
}
/* next present index >= i, or SPACE; skips absent pages in one step */
static uint32_t next_present(const Sparse *s, uint32_t i) {
    while (i < SPACE) {
        const Page *p = s->dir[i >> PAGE_BITS];
        if (!p) { i = ((i >> PAGE_BITS) + 1) << PAGE_BITS; continue; }
        for (uint32_t o = i & (PAGE_SIZE - 1); o < PAGE_SIZE; o++) if ((p->present[o >> 5] >> (o & 31)) & 1u) return (i & ~(PAGE_SIZE - 1)) + o;
        i = ((i >> PAGE_BITS) + 1) << PAGE_BITS;
    }
    return SPACE;
}
static void destroy(Sparse *s) { for (uint32_t d = 0; d < DIR_SIZE; d++) { free(s->dir[d]); s->dir[d] = NULL; } }

/* oracle: open-addressing hash of (index -> value) with tombstone-free backshift deletion via rebuild-free linear table */
#define OT 8192
static uint32_t ok[OT]; static int32_t ov[OT]; static unsigned char ost[OT]; static int on;
static unsigned oh(uint32_t k) { return (k * 2654435761u) >> 19; }
static int ofind(uint32_t k) { unsigned i = oh(k) % OT; while (ost[i]) { if (ok[i] == k) return (int)i; i = (i + 1) % OT; } return -1; }
static void oput(uint32_t k, int32_t v) { int i = ofind(k); if (i >= 0) { ov[i] = v; return; } unsigned j = oh(k) % OT; while (ost[j]) j = (j + 1) % OT; ost[j] = 1; ok[j] = k; ov[j] = v; on++; }
static void orebuild_without(uint32_t k) {
    uint32_t k2[OT]; int32_t v2[OT]; int n = 0;
    for (int i = 0; i < OT; i++) if (ost[i] && ok[i] != k) { k2[n] = ok[i]; v2[n] = ov[i]; n++; }
    memset(ost, 0, sizeof ost); on = 0;
    for (int i = 0; i < n; i++) oput(k2[i], v2[i]);
}

int main(void) {
    static Sparse s; memset(&s, 0, sizeof s);
    /* clustered indices: a handful of hot regions in the 1M-slot space */
    uint32_t centers[6];
    for (int i = 0; i < 6; i++) centers[i] = rnd() % SPACE;
    long puts = 0, dels = 0, hits = 0;
    for (int step = 0; step < 30000; step++) {
        unsigned c = rnd() % 6, off = rnd() % 600, op = rnd() % 10;
        uint32_t idx = (centers[c] + off * (1u + (c & 1u) * 37u)) % SPACE;
        if (op < 5 && on < 6000) { int32_t v = (int32_t)(rnd() % 100000); put(&s, idx, v); oput(idx, v); puts++; }
        else if (op < 8) { int r = del(&s, idx), i = ofind(idx); check(r == (i >= 0), "delete result"); if (i >= 0) orebuild_without(idx); dels += r; }
        else { int i = ofind(idx); check(has(&s, idx) == (i >= 0), "presence"); check(get(&s, idx, -7) == (i >= 0 ? ov[i] : -7), "get with default"); hits += i >= 0; }
        check(s.size == on, "size");
    }
    /* ordered iteration equals sorted oracle keys */
    uint32_t prev = 0; long n = 0; int first = 1; long sum = 0;
    for (uint32_t i = next_present(&s, 0); i < SPACE; i = next_present(&s, i + 1)) {
        check(first || i > prev, "increasing iteration"); first = 0; prev = i; n++;
        int k = ofind(i); check(k >= 0 && ov[k] == get(&s, i, 0), "iteration value"); sum += get(&s, i, 0);
    }
    check(n == on, "iteration count");
    int used_pages = 0; for (uint32_t d = 0; d < DIR_SIZE; d++) if (s.dir[d]) { used_pages++; check(s.dir[d]->used > 0, "no empty pages kept"); }
    check(used_pages == s.pages, "page count");
    printf("puts=%ld deletes=%ld hits=%ld size=%ld\n", puts, dels, hits, s.size);
    printf("pages live=%d peak=%d allocated=%ld freed=%ld; dense array would need %u pages\n", s.pages, s.peak_pages, s.allocs, s.frees, DIR_SIZE);
    printf("iterated %ld entries, value sum=%ld\n", n, sum);
    destroy(&s);
    return 0;
}
