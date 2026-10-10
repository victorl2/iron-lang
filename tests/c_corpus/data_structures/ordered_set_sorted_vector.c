/*
 * title: Ordered set on a sorted vector with bulk merge and set algebra
 * topic: data_structures
 * covers: ordered set, sorted contiguous array, lower_bound and upper_bound, rank and select, insert and erase by memmove, bulk insert by merge, union, intersection, difference, range erase, flat oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U 4000

static unsigned long long rs = 0x05E7BEC7ULL * 0x9E3779ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef struct { int *a; int n, cap; } OSet;

static void os_init(OSet *s) { s->a = NULL; s->n = s->cap = 0; }
static void os_free(OSet *s) { free(s->a); os_init(s); }
static void reserve(OSet *s, int n) { if (n > s->cap) { s->cap = n > 2 * s->cap ? n : 2 * s->cap; s->a = realloc(s->a, (size_t)s->cap * sizeof(int)); } }
static int lower_bound(const OSet *s, int x) { int lo = 0, hi = s->n; while (lo < hi) { int m = (lo + hi) / 2; if (s->a[m] < x) lo = m + 1; else hi = m; } return lo; }
static int upper_bound(const OSet *s, int x) { int lo = 0, hi = s->n; while (lo < hi) { int m = (lo + hi) / 2; if (s->a[m] <= x) lo = m + 1; else hi = m; } return lo; }
static int contains(const OSet *s, int x) { int i = lower_bound(s, x); return i < s->n && s->a[i] == x; }
static int insert(OSet *s, int x) {
    int i = lower_bound(s, x);
    if (i < s->n && s->a[i] == x) return 0;
    reserve(s, s->n + 1);
    memmove(s->a + i + 1, s->a + i, (size_t)(s->n - i) * sizeof(int)); s->a[i] = x; s->n++;
    return 1;
}
static int erase(OSet *s, int x) {
    int i = lower_bound(s, x);
    if (i >= s->n || s->a[i] != x) return 0;
    memmove(s->a + i, s->a + i + 1, (size_t)(s->n - i - 1) * sizeof(int)); s->n--;
    return 1;
}
static int erase_range(OSet *s, int lo, int hi) {   /* erase keys in [lo,hi) */
    int i = lower_bound(s, lo), j = lower_bound(s, hi);
    memmove(s->a + i, s->a + j, (size_t)(s->n - j) * sizeof(int)); s->n -= j - i;
    return j - i;
}
static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
/* bulk insert an unsorted batch: sort and dedupe it, then merge with the set into a fresh array */
static int insert_bulk(OSet *s, int *batch, int m) {
    qsort(batch, (size_t)m, sizeof(int), cmp_int);
    int newcap = s->n + m + 1;
    int *out = malloc((size_t)newcap * sizeof(int));
    int i = 0, j = 0, w = 0, added = 0;
    while (i < s->n || j < m) {
        if (j >= m || (i < s->n && s->a[i] < batch[j])) out[w++] = s->a[i++];
        else if (i >= s->n || batch[j] < s->a[i]) {
            int v = batch[j++];
            out[w++] = v; added++;
            while (j < m && batch[j] == v) j++;      /* skip duplicates inside the batch */
        } else { out[w++] = s->a[i++]; j++; while (j < m && batch[j] == out[w - 1]) j++; }
    }
    free(s->a); s->a = out; s->n = w; s->cap = newcap;
    return added;
}
static OSet set_op(const OSet *a, const OSet *b, char op) {
    OSet o; os_init(&o); reserve(&o, a->n + b->n + 1);
    int i = 0, j = 0;
    while (i < a->n || j < b->n) {
        if (j >= b->n || (i < a->n && a->a[i] < b->a[j])) { if (op != '&') o.a[o.n++] = a->a[i]; i++; }
        else if (i >= a->n || b->a[j] < a->a[i]) { if (op == '|') o.a[o.n++] = b->a[j]; j++; }
        else { if (op != '-') o.a[o.n++] = a->a[i]; i++; j++; }
    }
    return o;
}
static void verify(const OSet *s, const unsigned char *ref) {
    int c = 0;
    for (int x = 0; x < U; x++) c += ref[x];
    check(c == s->n, "size");
    for (int i = 0, k = 0; i < U; i++) if (ref[i]) check(s->a[k++] == i, "content");
}

int main(void) {
    OSet s; os_init(&s);
    static unsigned char ref[U];
    long ins = 0, del = 0, rdel = 0, bulk_added = 0;
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 20; int x = (int)(rnd() % U);
        if (op < 8) { int r = insert(&s, x); check(r == !ref[x], "insert result"); ref[x] = 1; ins += r; }
        else if (op < 12) { int r = erase(&s, x); check(r == ref[x], "erase result"); ref[x] = 0; del += r; }
        else if (op < 14) {
            int lb = lower_bound(&s, x), ub = upper_bound(&s, x), rl = 0, ru = 0;
            for (int v = 0; v < x; v++) rl += ref[v];
            ru = rl + ref[x];
            check(lb == rl && ub == ru, "rank via bounds");
            if (lb < s.n) { int sel = s.a[lb], y = x; while (!ref[y]) y++; check(sel == y, "successor"); }
        } else if (op < 15) {
            int hi = x + 1 + (int)(rnd() % 50); if (hi > U) hi = U;
            int r = erase_range(&s, x, hi), rr = 0;
            for (int v = x; v < hi; v++) { rr += ref[v]; ref[v] = 0; }
            check(r == rr, "range erase count"); rdel += r;
        } else if (op < 17) {
            int m = 1 + (int)(rnd() % 40); int batch[40];
            int expect_new = 0;
            unsigned char mark[U]; memset(mark, 0, sizeof mark);
            for (int i = 0; i < m; i++) { batch[i] = (int)(rnd() % U); if (!ref[batch[i]] && !mark[batch[i]]) { mark[batch[i]] = 1; expect_new++; } }
            for (int i = 0; i < m; i++) ref[batch[i]] = 1;
            int added = insert_bulk(&s, batch, m);
            check(added == expect_new, "bulk added count"); bulk_added += added;
        } else check(contains(&s, x) == ref[x], "contains");
        if (step % 500 == 0) verify(&s, ref);
    }
    verify(&s, ref);
    /* set algebra against a second set */
    OSet t; os_init(&t); static unsigned char rt[U];
    for (int i = 0; i < 1500; i++) { int x = (int)(rnd() % U); insert(&t, x); rt[x] = 1; }
    const char ops[3] = {'&', '|', '-'}; int sizes[3];
    for (int k = 0; k < 3; k++) {
        OSet o = set_op(&s, &t, ops[k]); static unsigned char rr[U];
        for (int x = 0; x < U; x++) rr[x] = ops[k] == '&' ? ref[x] & rt[x] : ops[k] == '|' ? ref[x] | rt[x] : ref[x] & !rt[x];
        verify(&o, rr); sizes[k] = o.n; os_free(&o);
    }
    printf("inserted=%ld erased=%ld range_erased=%ld bulk_added=%ld size=%d\n", ins, del, rdel, bulk_added, s.n);
    printf("|S|=%d |T|=%d  S&T=%d S|T=%d S-T=%d\n", s.n, t.n, sizes[0], sizes[1], sizes[2]);
    printf("min=%d median=%d max=%d\n", s.a[0], s.a[s.n / 2], s.a[s.n - 1]);
    os_free(&s); os_free(&t);
    return 0;
}
