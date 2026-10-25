/*
 * title: Run-length interval set with union, intersection and complement
 * topic: data_structures
 * covers: run-length encoded set, sorted disjoint half-open runs, range add/remove, coalescing, two-pointer merge algebra, complement, gap queries, boolean array oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U 3000

static unsigned long long rs = 0x121E5E7ULL * 0x9E37ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int lo, hi; } Run;
typedef struct { Run *r; int n, cap; } RSet;

static void rs_init(RSet *s) { s->r = NULL; s->n = s->cap = 0; }
static void rs_free(RSet *s) { free(s->r); s->r = NULL; s->n = s->cap = 0; }
static void rs_reserve(RSet *s, int n) { if (n > s->cap) { s->cap = n * 2; s->r = realloc(s->r, (size_t)s->cap * sizeof(Run)); } }
static void rs_push(RSet *s, int lo, int hi) {   /* append in order, coalescing touching runs */
    if (lo >= hi) return;
    if (s->n && s->r[s->n - 1].hi >= lo) { if (hi > s->r[s->n - 1].hi) s->r[s->n - 1].hi = hi; return; }
    rs_reserve(s, s->n + 1); s->r[s->n].lo = lo; s->r[s->n].hi = hi; s->n++;
}
static int first_ge(const RSet *s, int x) {  /* first run whose hi > x */
    int lo = 0, hi = s->n;
    while (lo < hi) { int m = (lo + hi) / 2; if (s->r[m].hi <= x) lo = m + 1; else hi = m; }
    return lo;
}
static int rs_has(const RSet *s, int x) { int i = first_ge(s, x); return i < s->n && s->r[i].lo <= x; }
static void rs_add(RSet *s, int lo, int hi) {
    if (lo >= hi) return;
    int i = first_ge(s, lo - 1), j = i;   /* runs touching or overlapping [lo,hi) */
    while (j < s->n && s->r[j].lo <= hi) j++;
    if (i < j) { if (s->r[i].lo < lo) lo = s->r[i].lo; if (s->r[j - 1].hi > hi) hi = s->r[j - 1].hi; }
    rs_reserve(s, s->n + 1);
    memmove(s->r + i + 1, s->r + j, (size_t)(s->n - j) * sizeof(Run));
    s->r[i].lo = lo; s->r[i].hi = hi; s->n = s->n - (j - i) + 1;
}
static void rs_remove(RSet *s, int lo, int hi) {
    if (lo >= hi) return;
    RSet t; rs_init(&t);
    for (int i = 0; i < s->n; i++) {
        Run r = s->r[i];
        if (r.hi <= lo || r.lo >= hi) rs_push(&t, r.lo, r.hi);
        else { rs_push(&t, r.lo, lo); rs_push(&t, hi > r.lo ? hi : r.lo, r.hi); }
    }
    /* rs_push coalesces: a split piece right of `hi` cannot touch the left piece */
    rs_free(s); *s = t;
}
static RSet rs_union(const RSet *a, const RSet *b) {
    RSet o; rs_init(&o); int i = 0, j = 0;
    while (i < a->n || j < b->n) {
        if (j >= b->n || (i < a->n && a->r[i].lo <= b->r[j].lo)) { rs_push(&o, a->r[i].lo, a->r[i].hi); i++; }
        else { rs_push(&o, b->r[j].lo, b->r[j].hi); j++; }
    }
    return o;
}
static RSet rs_inter(const RSet *a, const RSet *b) {
    RSet o; rs_init(&o); int i = 0, j = 0;
    while (i < a->n && j < b->n) {
        int lo = a->r[i].lo > b->r[j].lo ? a->r[i].lo : b->r[j].lo;
        int hi = a->r[i].hi < b->r[j].hi ? a->r[i].hi : b->r[j].hi;
        rs_push(&o, lo, hi);
        if (a->r[i].hi < b->r[j].hi) i++; else j++;
    }
    return o;
}
static RSet rs_complement(const RSet *a, int lo, int hi) {
    RSet o; rs_init(&o); int at = lo;
    for (int i = 0; i < a->n; i++) { rs_push(&o, at, a->r[i].lo < hi ? a->r[i].lo : hi); if (a->r[i].hi > at) at = a->r[i].hi; }
    rs_push(&o, at, hi);
    return o;
}
static long rs_count(const RSet *s) { long c = 0; for (int i = 0; i < s->n; i++) c += s->r[i].hi - s->r[i].lo; return c; }
static void verify(const RSet *s, const unsigned char *ref) {
    for (int i = 0; i < s->n; i++) { check(s->r[i].lo < s->r[i].hi, "nonempty run"); if (i) check(s->r[i - 1].hi < s->r[i].lo, "runs separated"); }
    for (int x = 0; x < U; x++) check(rs_has(s, x) == ref[x], "membership");
    long c = 0; for (int x = 0; x < U; x++) c += ref[x];
    check(c == rs_count(s), "count");
}
static int longest_gap(const RSet *s, int *at) {
    int best = 0; *at = 0; int prev = 0;
    for (int i = 0; i <= s->n; i++) {
        int nxt = i < s->n ? s->r[i].lo : U;
        if (nxt - prev > best) { best = nxt - prev; *at = prev; }
        if (i < s->n) prev = s->r[i].hi;
    }
    return best;
}

int main(void) {
    RSet a, b; rs_init(&a); rs_init(&b);
    static unsigned char ra[U], rb[U], rr[U];
    for (int round = 0; round < 6; round++) {
        for (int i = 0; i < 40; i++) {
            RSet *s = (rnd() & 1u) ? &a : &b; unsigned char *ref = s == &a ? ra : rb;
            int lo = (int)(rnd() % U), len = 1 + (int)(rnd() % 90);
            int hi = lo + len > U ? U : lo + len;
            if (rnd() % 100 < 70) { rs_add(s, lo, hi); for (int x = lo; x < hi; x++) ref[x] = 1; }
            else { rs_remove(s, lo, hi); for (int x = lo; x < hi; x++) ref[x] = 0; }
            verify(s, ref);
        }
        RSet u = rs_union(&a, &b), n = rs_inter(&a, &b), ca = rs_complement(&a, 0, U);
        for (int x = 0; x < U; x++) rr[x] = ra[x] | rb[x];
        verify(&u, rr);
        for (int x = 0; x < U; x++) rr[x] = ra[x] & rb[x];
        verify(&n, rr);
        for (int x = 0; x < U; x++) rr[x] = !ra[x];
        verify(&ca, rr);
        check(rs_count(&u) + rs_count(&n) == rs_count(&a) + rs_count(&b), "inclusion exclusion");
        int at, gap = longest_gap(&a, &at);
        printf("round %d: |A|=%4ld runs=%3d |B|=%4ld runs=%3d union=%4ld inter=%4ld complementA runs=%3d longest gap in A=%d at %d\n",
               round, rs_count(&a), a.n, rs_count(&b), b.n, rs_count(&u), rs_count(&n), ca.n, gap, at);
        rs_free(&u); rs_free(&n); rs_free(&ca);
    }
    rs_free(&a); rs_free(&b);
    return 0;
}
