/*
 * title: realloc in a custom boundary-tag heap
 * topic: memory
 * covers: realloc cases, shrink in place, grow into next free block, grow backward with memmove, move fallback, failure leaves block intact, realloc(NULL) and realloc(p,0)
 * deps: libc
 */
#define SEED 0x8EA110CULL
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
#define TAG 4u
#define MINBLK 16u
#define USED 1u

static _Alignas(16) unsigned char heap[HEAP];

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, heap + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(heap + o, &v, 4); }
static unsigned bsize(unsigned p) { return rd(p) & ~7u; }
static int bused(unsigned p) { return (int)(rd(p) & USED); }
static void setblk(unsigned p, unsigned size, int used) { uint32_t t = size | (used ? USED : 0); wr(p, t); wr(p + size - TAG, t); }
static unsigned need_for(size_t n) { unsigned s = (unsigned)((n + 7) & ~(size_t)7) + 2 * TAG; return s < MINBLK ? MINBLK : s; }

static void heap_init(void) { setblk(0, HEAP, 0); }

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = need_for(n);
    for (unsigned p = 0; p < HEAP; p += bsize(p)) {
        unsigned sz = bsize(p);
        if (bused(p) || sz < need) continue;
        if (sz - need >= MINBLK) { setblk(p, need, 1); setblk(p + need, sz - need, 0); }
        else setblk(p, sz, 1);
        return (long)(p + TAG);
    }
    return -1;
}

static void coalesce_free(unsigned p) {   /* p is a free block: merge neighbours */
    unsigned sz = bsize(p);
    if (p + sz < HEAP && !bused(p + sz)) sz += bsize(p + sz);
    if (p > 0 && !(rd(p - TAG) & USED)) { unsigned ps = rd(p - TAG) & ~7u; p -= ps; sz += ps; }
    setblk(p, sz, 0);
}
static void h_free(unsigned payload) {
    unsigned p = payload - TAG;
    CHECK(bused(p));
    setblk(p, bsize(p), 0);
    coalesce_free(p);
}

enum { R_NOOP, R_SHRINK, R_GROW_NEXT, R_GROW_PREV, R_MOVE, R_FAIL, R_ALLOC, R_FREE, NR };
static const char *rname[NR] = { "same-size/noop", "shrink in place", "grow into next", "grow into prev", "move", "failed (kept)", "realloc(NULL)", "realloc(p,0)" };
static unsigned long rcount[NR];

/* returns new payload offset, -1 on failure (original untouched), -2 when freed */
static long h_realloc(long payload, size_t old_n, size_t new_n) {
    if (payload < 0) { long r = h_alloc(new_n); rcount[r < 0 ? R_FAIL : R_ALLOC]++; return r; }
    if (new_n == 0) { h_free((unsigned)payload); rcount[R_FREE]++; return -2; }
    unsigned p = (unsigned)payload - TAG, cur = bsize(p), need = need_for(new_n);
    if (need <= cur) {
        if (cur - need >= MINBLK) {
            setblk(p, need, 1);
            setblk(p + need, cur - need, 0);
            coalesce_free(p + need);
            rcount[R_SHRINK]++;
        } else rcount[R_NOOP]++;
        return payload;
    }
    unsigned nxt_free = (p + cur < HEAP && !bused(p + cur)) ? bsize(p + cur) : 0;
    unsigned prv_free = (p > 0 && !(rd(p - TAG) & USED)) ? (rd(p - TAG) & ~7u) : 0;
    if (cur + nxt_free >= need) {
        unsigned tot = cur + nxt_free;
        if (tot - need >= MINBLK) { setblk(p, need, 1); setblk(p + need, tot - need, 0); }
        else setblk(p, tot, 1);
        rcount[R_GROW_NEXT]++;
        return payload;
    }
    if (prv_free && prv_free + cur + nxt_free >= need) {
        unsigned q = p - prv_free, tot = prv_free + cur + nxt_free;
        memmove(heap + q + TAG, heap + p + TAG, old_n);     /* regions overlap */
        if (tot - need >= MINBLK) { setblk(q, need, 1); setblk(q + need, tot - need, 0); }
        else setblk(q, tot, 1);
        rcount[R_GROW_PREV]++;
        return (long)(q + TAG);
    }
    long np = h_alloc(new_n);
    if (np < 0) { rcount[R_FAIL]++; return -1; }
    memcpy(heap + np, heap + payload, old_n);
    h_free((unsigned)payload);
    rcount[R_MOVE]++;
    return np;
}

static void heap_check(unsigned *nf, unsigned *fb, unsigned *big) {
    unsigned p = 0, cnt = 0, bytes = 0, mx = 0;
    int prev_free = 0;
    while (p < HEAP) {
        unsigned sz = bsize(p);
        CHECK(sz >= MINBLK && (sz & 7) == 0 && p + sz <= HEAP && rd(p) == rd(p + sz - TAG));
        if (bused(p)) prev_free = 0;
        else { CHECK(!prev_free); prev_free = 1; cnt++; bytes += sz; if (sz > mx) mx = sz; }
        p += sz;
    }
    CHECK(p == HEAP);
    *nf = cnt; *fb = bytes; *big = mx;
}

typedef struct { long off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[100];
    int nlive = 0;
    unsigned tag = 1, nf, fb, big;
    for (int step = 0; step < 20000; step++) {
        unsigned op = rnd() % 100;
        if (nlive == 0 || (op < 30 && nlive < 100)) {
            size_t n = 1 + rnd() % 100;
            long off = h_alloc(n);
            if (off < 0) continue;
            pat_fill(heap + off, n, tag);
            live[nlive].off = off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else if (op < 80) {
            Rec *r = &live[rnd() % (unsigned)nlive];
            size_t nn;
            unsigned k = rnd() % 10;
            if (k < 4) nn = r->n + 1 + rnd() % 60;
            else if (k < 9) nn = 1 + rnd() % (r->n + 1);
            else nn = r->n + 200 + rnd() % 400;
            long res = h_realloc(r->off, r->n, nn);
            if (res == -1) { CHECK(pat_ok(heap + r->off, r->n, r->tag)); continue; }
            size_t keep = nn < r->n ? nn : r->n;
            CHECK(pat_ok(heap + res, keep, r->tag));     /* contents preserved */
            r->off = res; r->n = nn;
            pat_fill(heap + res, nn, r->tag);
        } else if (op < 82) {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(h_realloc(live[i].off, live[i].n, 0) == -2);
            live[i] = live[--nlive];
        } else if (op < 84 && nlive < 100) {
            size_t n = 1 + rnd() % 80;
            long off = h_realloc(-1, 0, n);
            if (off >= 0) { pat_fill(heap + off, n, tag); live[nlive].off = off; live[nlive].n = n; live[nlive].tag = tag++; nlive++; }
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            h_free((unsigned)live[i].off);
            live[i] = live[--nlive];
        }
        if (step % 100 == 0) {
            heap_check(&nf, &fb, &big);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    heap_check(&nf, &fb, &big);
    for (int r = 0; r < NR; r++) printf("%-16s %lu\n", rname[r], rcount[r]);
    printf("live=%d free blocks=%u free bytes=%u largest=%u\n", nlive, nf, fb, big);
    for (int i = 0; i < nlive; i++) h_free((unsigned)live[i].off);
    heap_check(&nf, &fb, &big);
    CHECK(nf == 1 && fb == HEAP);
    printf("drained\n");
    return 0;
}
