/*
 * title: van Emde Boas tree
 * topic: data_structures
 * covers: van Emde Boas tree, recursive universe splitting, min/max caching, successor, predecessor, delete
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct V {
    int u;          /* universe size, power of two >= 2 */
    int min, max;   /* -1 when empty */
    struct V *summary;
    struct V **cluster;
} V;

static unsigned long long rs = 0xC0FFEE1234567ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int lo_bits(int u) { int b = 0; while ((1 << b) < u) b++; return b / 2; }  /* floor(log/2) */
static int sq_hi(int u) { int b = 0; while ((1 << b) < u) b++; return (1 << (b - b / 2)); } /* number of clusters */
static int sq_lo(int u) { return 1 << lo_bits(u); }                              /* cluster size */
static int high(const V *v, int x) { return x >> lo_bits(v->u); }
static int low(const V *v, int x) { return x & (sq_lo(v->u) - 1); }
static int idx(const V *v, int h, int l) { return (h << lo_bits(v->u)) | l; }

static long nodes_made;
static V *make(int u) {
    V *v = malloc(sizeof *v);
    nodes_made++;
    v->u = u; v->min = v->max = -1; v->summary = NULL; v->cluster = NULL;
    if (u > 2) {
        int nc = sq_hi(u);
        v->summary = make(nc);
        v->cluster = malloc(sizeof(V *) * (size_t)nc);
        for (int i = 0; i < nc; i++) v->cluster[i] = make(sq_lo(u));
    }
    return v;
}
static void destroy(V *v) {
    if (v->cluster) { for (int i = 0; i < sq_hi(v->u); i++) destroy(v->cluster[i]); free(v->cluster); destroy(v->summary); }
    free(v);
}
static int member(const V *v, int x) {
    if (x == v->min || x == v->max) return 1;
    if (v->u == 2) return 0;
    return member(v->cluster[high(v, x)], low(v, x));
}
static void insert(V *v, int x) {
    if (v->min < 0) { v->min = v->max = x; return; }
    if (x == v->min || x == v->max) return;
    if (x < v->min) { int t = x; x = v->min; v->min = t; }
    if (v->u > 2) {
        V *c = v->cluster[high(v, x)];
        if (c->min < 0) insert(v->summary, high(v, x));
        insert(c, low(v, x));
    }
    if (x > v->max) v->max = x;
}
/* smallest element > x, or -1 */
static int succ(const V *v, int x) {
    if (v->u == 2) return (x == 0 && v->max == 1) ? 1 : -1;
    if (v->min >= 0 && x < v->min) return v->min;
    const V *c = v->cluster[high(v, x)];
    if (c->max >= 0 && low(v, x) < c->max) return idx(v, high(v, x), succ(c, low(v, x)));
    int sc = succ(v->summary, high(v, x));
    if (sc < 0) return -1;
    return idx(v, sc, v->cluster[sc]->min);
}
/* largest element < x, or -1 */
static int pred(const V *v, int x) {
    if (v->u == 2) return (x == 1 && v->min == 0) ? 0 : -1;
    if (v->max >= 0 && x > v->max) return v->max;
    const V *c = v->cluster[high(v, x)];
    if (c->min >= 0 && low(v, x) > c->min) return idx(v, high(v, x), pred(c, low(v, x)));
    int pc = pred(v->summary, high(v, x));
    if (pc < 0) return (v->min >= 0 && x > v->min) ? v->min : -1;
    return idx(v, pc, v->cluster[pc]->max);
}
static void erase(V *v, int x) {
    if (v->min == v->max) { if (v->min == x) v->min = v->max = -1; return; }
    if (v->u == 2) { v->min = v->max = (x == 0) ? 1 : 0; return; }
    if (x == v->min) {
        int fc = v->summary->min;
        x = idx(v, fc, v->cluster[fc]->min);
        v->min = x;
    }
    V *c = v->cluster[high(v, x)];
    erase(c, low(v, x));
    if (c->min < 0) {
        erase(v->summary, high(v, x));
        if (x == v->max) {
            int sm = v->summary->max;
            v->max = sm < 0 ? v->min : idx(v, sm, v->cluster[sm]->max);
        }
    } else if (x == v->max) v->max = idx(v, high(v, x), c->max);
}
static int depth_of(int u) { int d = 1; while (u > 2) { u = sq_lo(u); d++; } return d; }

int main(void) {
    int bitsz[] = { 1, 2, 3, 5, 8, 12 };
    for (unsigned bi = 0; bi < sizeof bitsz / sizeof bitsz[0]; bi++) {
        int u = 1 << bitsz[bi];
        V *v = make(u);
        long created = nodes_made; nodes_made = 0;
        unsigned char *ref = calloc((size_t)u, 1);
        int count = 0, ops = 0, qcount = 0;
        long checksum = 0;
        for (int step = 0; step < 6000; step++) {
            int x = (int)(rnd() % (unsigned)u);
            unsigned op = rnd() % 5;
            if (op < 2) { insert(v, x); if (!ref[x]) count++; ref[x] = 1; ops++; }
            else if (op < 3) { if (ref[x]) { erase(v, x); ref[x] = 0; count--; } else check(!member(v, x), "member absent"); ops++; }
            else {
                int es = -1, ep = -1;
                for (int k = x + 1; k < u; k++) if (ref[k]) { es = k; break; }
                for (int k = x - 1; k >= 0; k--) if (ref[k]) { ep = k; break; }
                int s = succ(v, x), p = pred(v, x);
                check(s == es, "succ vs brute force");
                check(p == ep, "pred vs brute force");
                check(member(v, x) == ref[x], "member vs brute force");
                checksum += s * 3 + p;
                qcount++;
            }
            int mn = -1, mx = -1;
            for (int k = 0; k < u && k < 4096; k++) if (ref[k]) { if (mn < 0) mn = k; mx = k; }
            check(v->min == mn && v->max == mx, "min/max cached");
        }
        /* full ordered walk via succ */
        int walked = 0, cur = v->min;
        while (cur >= 0) { check(ref[cur], "walk hits members"); walked++; cur = succ(v, cur); }
        check(walked == count, "walk length");
        printf("u=2^%-2d nodes=%5ld depth=%d ops=%d queries=%d members=%d checksum=%ld\n", bitsz[bi], created, depth_of(u), ops, qcount, count, checksum);
        free(ref); destroy(v);
    }
    return 0;
}
