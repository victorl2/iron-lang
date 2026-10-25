/*
 * title: dlmalloc-style allocator with small bins, large bins, unsorted list and top chunk
 * topic: memory
 * covers: prev_inuse bit, footer only in free chunks, unsorted list processing, small exact bins, sorted large bins, remainder to unsorted, top chunk splitting and merging
 * deps: libc
 */
#define SEED 0xD1A110CULL
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

#define HEAP 12288u
#define NIL 0xFFFFFFFFu
#define USEDF 1u
#define PREVU 2u
#define NSMALL 32          /* bins 2..31 hold exact sizes 16..248 */
#define NLARGE 6
#define NBINS (NSMALL + NLARGE)
#define UNS NBINS          /* unsorted list index */

static _Alignas(16) unsigned char heap[HEAP];
static uint32_t bin[NBINS + 1];
static unsigned top_off, top_size;
static unsigned long src_count[6], allocs, frees, top_merges, sorted_in, top_splits, fails;
static const char *src_name[6] = { "small bin exact", "unsorted exact", "large/next-bin best fit", "top split", "failed", "remainder reuse" };

static uint32_t rd(unsigned o) { uint32_t v; memcpy(&v, heap + o, 4); return v; }
static void wr(unsigned o, uint32_t v) { memcpy(heap + o, &v, 4); }
static unsigned csize(unsigned p) { return rd(p) & ~7u; }
static int cused(unsigned p) { return (int)(rd(p) & USEDF); }
static int cprevu(unsigned p) { return (int)((rd(p) & PREVU) != 0); }
static void set_prevu(unsigned p, int v) { wr(p, v ? rd(p) | PREVU : rd(p) & ~PREVU); }
static uint32_t fd(unsigned p) { return rd(p + 4); }
static uint32_t bk(unsigned p) { return rd(p + 8); }

static int bin_index(unsigned size) {
    if (size < 256) return (int)(size / 8);
    int l = 0;
    unsigned s = size >> 8;
    while (s > 1 && l < NLARGE - 1) { s >>= 1; l++; }
    return NSMALL + l;
}

static void list_unlink(unsigned p, int b) {
    uint32_t f = fd(p), k = bk(p);
    if (k != NIL) wr(k + 4, f); else bin[b] = f;
    if (f != NIL) wr(f + 8, k);
}
static void list_push_head(unsigned p, int b) {
    wr(p + 4, bin[b]); wr(p + 8, NIL);
    if (bin[b] != NIL) wr(bin[b] + 8, p);
    bin[b] = p;
}
static void list_push_tail(unsigned p, int b) {
    uint32_t t = bin[b];
    if (t == NIL) { wr(p + 4, NIL); wr(p + 8, NIL); bin[b] = p; return; }
    while (fd(t) != NIL) t = fd(t);
    wr(p + 4, NIL); wr(p + 8, t); wr(t + 4, p);
}
static void bin_insert_sorted(unsigned p) {
    int b = bin_index(csize(p));
    if (b < NSMALL) { list_push_head(p, b); return; }
    uint32_t prev = NIL, cur = bin[b];
    while (cur != NIL && csize(cur) < csize(p)) { prev = cur; cur = fd(cur); }
    wr(p + 4, cur); wr(p + 8, prev);
    if (cur != NIL) wr(cur + 8, p);
    if (prev != NIL) wr(prev + 4, p); else bin[b] = p;
}

static void mark_free(unsigned p, unsigned size, int prevu) {
    wr(p, size | (prevu ? PREVU : 0));
    wr(p + size - 4, size);
    if (p + size == top_off) set_prevu(top_off, 0);
    else set_prevu(p + size, 0);
}

static void heap_init(void) {
    for (int i = 0; i <= NBINS; i++) bin[i] = NIL;
    top_off = 0; top_size = HEAP;
    wr(0, HEAP | PREVU);
}

/* carve `need` from free chunk p (already unlinked), remainder goes to the unsorted list */
static long use_chunk(unsigned p, unsigned need) {
    unsigned sz = csize(p);
    int pu = cprevu(p);
    if (sz - need >= 16) {
        wr(p, need | USEDF | (pu ? PREVU : 0));
        unsigned r = p + need;
        mark_free(r, sz - need, 1);
        list_push_tail(r, UNS);
        src_count[5]++;
    } else {
        wr(p, sz | USEDF | (pu ? PREVU : 0));
        if (p + sz == top_off) set_prevu(top_off, 1); else set_prevu(p + sz, 1);
    }
    allocs++;
    return (long)(p + 4);
}

static long h_alloc(size_t n) {
    if (n == 0) return -1;
    unsigned need = (unsigned)((n + 4 + 7) & ~(size_t)7);
    if (need < 16) need = 16;
    int nb = bin_index(need);
    if (nb < NSMALL && bin[nb] != NIL) {
        unsigned p = bin[nb];
        list_unlink(p, nb);
        src_count[0]++;
        return use_chunk(p, need);
    }
    while (bin[UNS] != NIL) {
        unsigned p = bin[UNS];
        list_unlink(p, UNS);
        if (csize(p) == need) { src_count[1]++; return use_chunk(p, need); }
        bin_insert_sorted(p);
        sorted_in++;
    }
    /* best fit: own bin (sorted), then every larger bin, first chunk that fits */
    for (int b = nb; b < NBINS; b++) {
        for (uint32_t p = bin[b]; p != NIL; p = fd(p)) {
            if (csize(p) < need) continue;
            list_unlink(p, b);
            src_count[2]++;
            return use_chunk(p, need);
        }
    }
    if (top_size >= need + 16) {
        unsigned p = top_off;
        int pu = cprevu(top_off);
        wr(p, need | USEDF | (pu ? PREVU : 0));
        top_off += need; top_size -= need;
        wr(top_off, top_size | PREVU);
        top_splits++; src_count[3]++; allocs++;
        return (long)(p + 4);
    }
    src_count[4]++; fails++;
    return -1;
}

static void unlink_free_chunk(unsigned p) {
    /* find which list holds it: by size class, or unsorted; scan-free via a tag-less lookup */
    int b = bin_index(csize(p));
    /* it is either in its class bin or in the unsorted list; test membership cheaply by walking the shorter path */
    for (uint32_t q = bin[b]; q != NIL; q = fd(q)) if (q == p) { list_unlink(p, b); return; }
    list_unlink(p, UNS);
}

static void h_free(unsigned payload) {
    unsigned p = payload - 4, sz = csize(p);
    CHECK(cused(p));
    frees++;
    int pu = cprevu(p);
    if (!pu) {
        unsigned ps = rd(p - 4);
        unsigned pp = p - ps;
        unlink_free_chunk(pp);
        p = pp; sz += ps; pu = cprevu(pp);
    }
    if (p + sz == top_off) {                 /* merge into the wilderness */
        top_size += sz; top_off = p;
        wr(top_off, top_size | (pu ? PREVU : 0));
        top_merges++;
        return;
    }
    unsigned nx = p + sz;
    if (nx != top_off && !cused(nx)) { unlink_free_chunk(nx); sz += csize(nx); }
    if (p + sz == top_off) {
        top_size += sz; top_off = p;
        wr(top_off, top_size | (pu ? PREVU : 0));
        top_merges++;
        return;
    }
    mark_free(p, sz, pu);
    list_push_tail(p, UNS);
}

static void heap_check(unsigned *nf, unsigned *fb, unsigned *largest) {
    unsigned p = 0, cnt = 0, bytes = 0, big = 0;
    int prev_free = 0, first = 1;
    while (p < top_off) {
        unsigned sz = csize(p);
        CHECK(sz >= 16 && (sz & 7) == 0 && p + sz <= top_off);
        CHECK(cprevu(p) == (first ? 1 : !prev_free));
        if (cused(p)) prev_free = 0;
        else { CHECK(!prev_free && rd(p + sz - 4) == sz); prev_free = 1; cnt++; bytes += sz; if (sz > big) big = sz; }
        first = 0; p += sz;
    }
    CHECK(p == top_off && csize(top_off) == top_size && top_off + top_size == HEAP);
    CHECK(!prev_free);                        /* a free chunk next to top must have merged */
    CHECK(cprevu(top_off) == 1);
    unsigned listed = 0;
    for (int b = 0; b <= NBINS; b++) {
        uint32_t pv = NIL;
        unsigned last = 0;
        for (uint32_t q = bin[b]; q != NIL; q = fd(q)) {
            CHECK(!cused(q) && bk(q) == pv);
            if (b < NSMALL) CHECK(csize(q) == (unsigned)b * 8);
            else if (b < NBINS) { CHECK(bin_index(csize(q)) == b && csize(q) >= last); last = csize(q); }
            pv = q; listed++; CHECK(listed <= cnt);
        }
    }
    CHECK(listed == cnt);
    *nf = cnt; *fb = bytes; *largest = big;
}

typedef struct { unsigned off; size_t n; unsigned tag; } Rec;

int main(void) {
    heap_init();
    Rec live[600];
    int nlive = 0, nfails = 0;
    unsigned tag = 1, nf, fb, big;
    unsigned min_top = HEAP;
    for (int step = 0; step < 20000; step++) {
        if (nlive == 0 || (rnd() % 100 < 52 && nlive < 600)) {
            unsigned k = rnd() % 20;
            size_t n = k < 12 ? 1 + rnd() % 60 : k < 18 ? 61 + rnd() % 200 : 261 + rnd() % 900;
            long off = h_alloc(n);
            if (off < 0) { nfails++; continue; }
            pat_fill(heap + off, n, tag);
            live[nlive].off = (unsigned)off; live[nlive].n = n; live[nlive].tag = tag++;
            nlive++;
        } else {
            int i = (int)(rnd() % (unsigned)nlive);
            CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
            h_free(live[i].off);
            live[i] = live[--nlive];
        }
        if (top_size < min_top) min_top = top_size;
        if (step % 100 == 0) {
            heap_check(&nf, &fb, &big);
            for (int i = 0; i < nlive; i++) CHECK(pat_ok(heap + live[i].off, live[i].n, live[i].tag));
        }
    }
    heap_check(&nf, &fb, &big);
    printf("allocs=%lu frees=%lu failed=%d live=%d\n", allocs, frees, nfails, nlive);
    for (int s = 0; s < 6; s++) printf("  %-24s %lu\n", src_name[s], src_count[s]);
    printf("unsorted chunks binned=%lu top merges=%lu top splits=%lu\n", sorted_in, top_merges, top_splits);
    printf("top size now=%u (min seen %u); free chunks below top=%u bytes=%u largest=%u\n", top_size, min_top, nf, fb, big);
    for (int i = 0; i < nlive; i++) h_free(live[i].off);
    heap_check(&nf, &fb, &big);
    CHECK(nf == 0 && top_off == 0 && top_size == HEAP);
    printf("drained: everything merged back into top (%u bytes)\n", top_size);
    return 0;
}
