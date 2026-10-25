/*
 * title: Multiset (bag) with counts, bag algebra and order statistics
 * topic: data_structures
 * covers: multiset, sorted (value, count) pairs, insert with multiplicity, remove n copies, bag union max, sum, intersection min, difference, sub-bag test, k-th element, mode, count array oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U 200

static unsigned long long rs = 0xBA6C0417ULL * 0x9E3779ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int v; int cnt; } Pair;
typedef struct { Pair *p; int n, cap; long total; } Bag;

static void bag_init(Bag *b) { b->p = NULL; b->n = b->cap = 0; b->total = 0; }
static void bag_free(Bag *b) { free(b->p); bag_init(b); }
static int find(const Bag *b, int v) { int lo = 0, hi = b->n; while (lo < hi) { int m = (lo + hi) / 2; if (b->p[m].v < v) lo = m + 1; else hi = m; } return lo; }
static int count(const Bag *b, int v) { int i = find(b, v); return i < b->n && b->p[i].v == v ? b->p[i].cnt : 0; }
static void add(Bag *b, int v, int k) {
    int i = find(b, v);
    if (i < b->n && b->p[i].v == v) { b->p[i].cnt += k; b->total += k; return; }
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 8; b->p = realloc(b->p, (size_t)b->cap * sizeof(Pair)); }
    memmove(b->p + i + 1, b->p + i, (size_t)(b->n - i) * sizeof(Pair));
    b->p[i].v = v; b->p[i].cnt = k; b->n++; b->total += k;
}
static int remove_n(Bag *b, int v, int k) {   /* removes up to k copies, returns how many were removed */
    int i = find(b, v);
    if (i >= b->n || b->p[i].v != v) return 0;
    int r = k < b->p[i].cnt ? k : b->p[i].cnt;
    b->p[i].cnt -= r; b->total -= r;
    if (b->p[i].cnt == 0) { memmove(b->p + i, b->p + i + 1, (size_t)(b->n - i - 1) * sizeof(Pair)); b->n--; }
    return r;
}
static int kth(const Bag *b, long k, int *v) {   /* 0-based k-th smallest with multiplicity */
    for (int i = 0; i < b->n; i++) { if (k < b->p[i].cnt) { *v = b->p[i].v; return 1; } k -= b->p[i].cnt; }
    return 0;
}
static int mode(const Bag *b, int *cnt) {   /* most frequent value, smallest on ties */
    int best = -1; *cnt = 0;
    for (int i = 0; i < b->n; i++) if (b->p[i].cnt > *cnt) { *cnt = b->p[i].cnt; best = b->p[i].v; }
    return best;
}
static Bag combine(const Bag *a, const Bag *b, char op) {
    Bag o; bag_init(&o);
    int i = 0, j = 0;
    while (i < a->n || j < b->n) {
        int v, ca = 0, cb = 0;
        if (j >= b->n || (i < a->n && a->p[i].v < b->p[j].v)) { v = a->p[i].v; ca = a->p[i].cnt; i++; }
        else if (i >= a->n || b->p[j].v < a->p[i].v) { v = b->p[j].v; cb = b->p[j].cnt; j++; }
        else { v = a->p[i].v; ca = a->p[i].cnt; cb = b->p[j].cnt; i++; j++; }
        int c = op == '|' ? (ca > cb ? ca : cb) : op == '+' ? ca + cb : op == '&' ? (ca < cb ? ca : cb) : (ca > cb ? ca - cb : 0);
        if (c > 0) add(&o, v, c);
    }
    return o;
}
static int sub_bag(const Bag *a, const Bag *b) { for (int i = 0; i < a->n; i++) if (count(b, a->p[i].v) < a->p[i].cnt) return 0; return 1; }
static void verify(const Bag *b, const int *ref) {
    long t = 0; int distinct = 0;
    for (int v = 0; v < U; v++) { check(count(b, v) == ref[v], "count"); t += ref[v]; distinct += ref[v] > 0; }
    check(t == b->total && distinct == b->n, "totals");
    for (int i = 1; i < b->n; i++) check(b->p[i - 1].v < b->p[i].v && b->p[i].cnt > 0, "invariants");
}

int main(void) {
    Bag a, b; bag_init(&a); bag_init(&b);
    static int ra[U], rb[U];
    long adds = 0, removed = 0;
    for (int step = 0; step < 8000; step++) {
        Bag *s = (rnd() & 1u) ? &a : &b; int *ref = s == &a ? ra : rb;
        int v = (int)(rnd() % U); int k = 1 + (int)(rnd() % 4); unsigned op = rnd() % 10;
        if (op < 6) { add(s, v, k); ref[v] += k; adds += k; }
        else if (op < 9) { int r = remove_n(s, v, k), want = ref[v] < k ? ref[v] : k; check(r == want, "removed count"); ref[v] -= r; removed += r; }
        else {
            long q = (long)(rnd() % (unsigned)(s->total + 1)); int got, ok = kth(s, q, &got), acc = 0, want = -1;
            for (int x = 0; x < U && want < 0; x++) { acc += ref[x]; if (q < acc) want = x; }
            check(ok == (want >= 0) && (!ok || got == want), "k-th element");
        }
    }
    verify(&a, ra); verify(&b, rb);
    const char ops[4] = {'|', '+', '&', '-'}; long tot[4]; int dist[4];
    for (int k = 0; k < 4; k++) {
        Bag o = combine(&a, &b, ops[k]); int rr[U];
        for (int v = 0; v < U; v++) rr[v] = ops[k] == '|' ? (ra[v] > rb[v] ? ra[v] : rb[v]) : ops[k] == '+' ? ra[v] + rb[v] : ops[k] == '&' ? (ra[v] < rb[v] ? ra[v] : rb[v]) : (ra[v] > rb[v] ? ra[v] - rb[v] : 0);
        verify(&o, rr); tot[k] = o.total; dist[k] = o.n;
        if (ops[k] == '&') check(sub_bag(&o, &a) && sub_bag(&o, &b), "intersection is sub-bag of both");
        if (ops[k] == '|') check(sub_bag(&a, &o) && sub_bag(&b, &o), "union contains both");
        bag_free(&o);
    }
    check(tot[1] == a.total + b.total, "sum total");
    check(tot[0] + tot[2] == a.total + b.total, "max + min = sum");
    int ma, mb; int va = mode(&a, &ma), vb = mode(&b, &mb);
    printf("adds=%ld removed=%ld |A|=%ld distinct=%d |B|=%ld distinct=%d\n", adds, removed, a.total, a.n, b.total, b.n);
    printf("A|B=%ld/%d A+B=%ld/%d A&B=%ld/%d A-B=%ld/%d (total/distinct)\n", tot[0], dist[0], tot[1], dist[1], tot[2], dist[2], tot[3], dist[3]);
    int med = -1; kth(&a, a.total / 2, &med);
    printf("mode(A)=%d x%d mode(B)=%d x%d median(A)=%d sub_bag(A,B)=%d\n", va, ma, vb, mb, med, sub_bag(&a, &b));
    bag_free(&a); bag_free(&b);
    return 0;
}
