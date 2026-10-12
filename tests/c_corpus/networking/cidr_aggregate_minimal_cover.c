/*
 * title: CIDR route aggregation to a minimal cover
 * topic: networking
 * covers: supernetting, sibling merging, covered prefix removal, sort and sweep, bitmap verification of covered address sets
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { uint32_t addr; int len; } Pfx;

static uint32_t mask_of(int len) { return len == 0 ? 0u : (0xffffffffu << (32 - len)); }
static uint32_t last_of(Pfx p) { return p.addr | ~mask_of(p.len); }

static int cmp(const void *a, const void *b) {
    const Pfx *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    if (x->len != y->len) return x->len < y->len ? -1 : 1; /* shorter (bigger) first */
    return 0;
}

/* Aggregate in place, returns new count. */
static int aggregate(Pfx *v, int n) {
    qsort(v, (size_t)n, sizeof *v, cmp);
    /* remove covered */
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m > 0 && v[i].addr <= last_of(v[m - 1]) && v[i].addr >= v[m - 1].addr) continue;
        v[m++] = v[i];
    }
    /* merge siblings with a stack, repeat until stable */
    int changed = 1;
    while (changed) {
        changed = 0;
        int k = 0;
        for (int i = 0; i < m; i++) {
            v[k++] = v[i];
            while (k >= 2) {
                Pfx a = v[k - 2], b = v[k - 1];
                if (a.len == b.len && a.len > 0 && (a.addr ^ b.addr) == (1u << (32 - a.len)) && (a.addr & (1u << (32 - a.len))) == 0) {
                    v[k - 2].len = a.len - 1;
                    k--;
                    changed = 1;
                } else break;
            }
        }
        m = k;
    }
    return m;
}

static uint32_t rs = 0xfeed1234u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static void show(const Pfx *v, int n) {
    for (int i = 0; i < n; i++)
        printf("  %u.%u.%u.%u/%d\n", (unsigned)(v[i].addr >> 24), (unsigned)((v[i].addr >> 16) & 255),
               (unsigned)((v[i].addr >> 8) & 255), (unsigned)(v[i].addr & 255), v[i].len);
}

int main(void) {
    /* Hand made example: four /26s in a /24 plus a covered /28 and a lone /24. */
    Pfx h[] = {
        { 0xc0000200u, 26 }, { 0xc0000240u, 26 }, { 0xc0000280u, 26 }, { 0xc00002c0u, 26 },
        { 0xc0000210u, 28 }, { 0xc0000300u, 24 }, { 0xc0000400u, 25 }, { 0xc0000480u, 25 },
    };
    int hn = (int)(sizeof h / sizeof h[0]);
    printf("input %d prefixes\n", hn);
    hn = aggregate(h, hn);
    printf("aggregated to %d:\n", hn);
    show(h, hn);
    CHECK(hn == 2 && h[0].len == 23 && h[1].len == 24);

    /* Random trials within 10.9.0.0/16 checked against a bitmap of all 65536 addresses. */
    static unsigned char bm[65536], bm2[65536];
    long in_total = 0, out_total = 0;
    int worst_ratio = 0;
    for (int trial = 0; trial < 60; trial++) {
        Pfx v[200];
        int n = 20 + (int)(rnd() % 180);
        memset(bm, 0, sizeof bm);
        for (int i = 0; i < n; i++) {
            uint32_t r = rnd();
            int len = 20 + (int)(r % 13);
            uint32_t off = rnd() & 0xffffu;
            v[i].len = len;
            v[i].addr = (0x0a090000u | off) & mask_of(len);
            for (uint32_t a = v[i].addr; a <= last_of(v[i]); a++) bm[a & 0xffff] = 1;
        }
        int m = aggregate(v, n);
        memset(bm2, 0, sizeof bm2);
        for (int i = 0; i < m; i++) {
            for (uint32_t a = v[i].addr; a <= last_of(v[i]); a++) {
                CHECK(bm2[a & 0xffff] == 0); /* disjoint */
                bm2[a & 0xffff] = 1;
            }
            if (i > 0) CHECK(v[i].addr > last_of(v[i - 1]));
        }
        CHECK(memcmp(bm, bm2, sizeof bm) == 0);
        /* minimality: no two adjacent prefixes are mergeable siblings */
        for (int i = 0; i + 1 < m; i++)
            CHECK(!(v[i].len == v[i + 1].len && v[i].len > 0 && (v[i].addr ^ v[i + 1].addr) == (1u << (32 - v[i].len)) && !(v[i].addr & (1u << (32 - v[i].len)))));
        in_total += n;
        out_total += m;
        int ratio = m * 100 / n;
        if (ratio > worst_ratio) worst_ratio = ratio;
    }
    printf("60 random trials: %ld input prefixes -> %ld aggregated, worst ratio %d%%\n", in_total, out_total, worst_ratio);

    /* Full /16 built from 256 random-order /24s collapses to one route. */
    Pfx full[256];
    for (int i = 0; i < 256; i++) { full[i].addr = 0xac100000u | ((uint32_t)i << 8); full[i].len = 24; }
    for (int i = 255; i > 0; i--) { int j = (int)(rnd() % (uint32_t)(i + 1)); Pfx t = full[i]; full[i] = full[j]; full[j] = t; }
    int fm = aggregate(full, 256);
    printf("256 shuffled /24 -> %d route: ", fm);
    show(full, fm);
    CHECK(fm == 1 && full[0].len == 16);
    /* Remove one /24: 8 routes remain (a /17, /18, ... /24 chain). */
    Pfx hole[255];
    int hk = 0;
    for (int i = 0; i < 256; i++) if (i != 77) { hole[hk].addr = 0xac100000u | ((uint32_t)i << 8); hole[hk].len = 24; hk++; }
    int hm = aggregate(hole, hk);
    printf("/16 minus one /24 -> %d routes\n", hm);
    show(hole, hm);
    CHECK(hm == 8);
    return 0;
}
