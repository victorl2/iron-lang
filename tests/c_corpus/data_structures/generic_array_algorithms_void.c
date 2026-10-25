/*
 * title: Type-erased array algorithms over element size and comparator
 * topic: data_structures
 * covers: void pointer arrays, byte-wise swap and rotate, heap sort, quickselect, binary bounds, stable partition, next permutation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 271u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef int (*Cmp)(const void *, const void *);
typedef int (*Pred)(const void *);
#define AT(base, i, sz) ((unsigned char *)(base) + (size_t)(i) * (sz))

static void swap_bytes(void *a, void *b, size_t sz) {
    unsigned char *x = a, *y = b;
    while (sz--) { unsigned char t = *x; *x++ = *y; *y++ = t; }
}
static void reverse_range(void *base, size_t lo, size_t hi, size_t sz) {
    while (lo + 1 < hi) { swap_bytes(AT(base, lo, sz), AT(base, hi - 1, sz), sz); lo++; hi--; }
}
/* left rotation by k using three reversals */
static void rotate_left(void *base, size_t n, size_t k, size_t sz) {
    if (n == 0) return;
    k %= n;
    reverse_range(base, 0, k, sz);
    reverse_range(base, k, n, sz);
    reverse_range(base, 0, n, sz);
}
static size_t lower_bound(const void *base, size_t n, const void *key, size_t sz, Cmp cmp) {
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (cmp(AT(base, m, sz), key) < 0) lo = m + 1; else hi = m; }
    return lo;
}
static size_t upper_bound(const void *base, size_t n, const void *key, size_t sz, Cmp cmp) {
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t m = lo + (hi - lo) / 2; if (cmp(AT(base, m, sz), key) <= 0) lo = m + 1; else hi = m; }
    return lo;
}
/* heap sort: sift down with byte swaps */
static void sift_down(void *base, size_t n, size_t i, size_t sz, Cmp cmp) {
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, big = i;
        if (l < n && cmp(AT(base, l, sz), AT(base, big, sz)) > 0) big = l;
        if (r < n && cmp(AT(base, r, sz), AT(base, big, sz)) > 0) big = r;
        if (big == i) return;
        swap_bytes(AT(base, i, sz), AT(base, big, sz), sz);
        i = big;
    }
}
static void heap_sort(void *base, size_t n, size_t sz, Cmp cmp) {
    for (size_t i = n / 2; i-- > 0;) sift_down(base, n, i, sz, cmp);
    for (size_t end = n; end > 1; end--) {
        swap_bytes(AT(base, 0, sz), AT(base, end - 1, sz), sz);
        sift_down(base, end - 1, 0, sz, cmp);
    }
}
/* quickselect (Hoare with median-of-three), result: element with rank k at position k */
static void nth_element(void *base, size_t n, size_t k, size_t sz, Cmp cmp) {
    size_t lo = 0, hi = n;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (cmp(AT(base, mid, sz), AT(base, lo, sz)) < 0) swap_bytes(AT(base, mid, sz), AT(base, lo, sz), sz);
        if (cmp(AT(base, hi - 1, sz), AT(base, lo, sz)) < 0) swap_bytes(AT(base, hi - 1, sz), AT(base, lo, sz), sz);
        if (cmp(AT(base, hi - 1, sz), AT(base, mid, sz)) < 0) swap_bytes(AT(base, hi - 1, sz), AT(base, mid, sz), sz);
        swap_bytes(AT(base, mid, sz), AT(base, hi - 1, sz), sz); /* pivot to the end */
        size_t store = lo;
        for (size_t i = lo; i + 1 < hi; i++)
            if (cmp(AT(base, i, sz), AT(base, hi - 1, sz)) < 0) { swap_bytes(AT(base, i, sz), AT(base, store, sz), sz); store++; }
        swap_bytes(AT(base, store, sz), AT(base, hi - 1, sz), sz);
        if (k == store) return;
        if (k < store) hi = store; else lo = store + 1;
    }
}
/* stable partition with a scratch buffer; returns the number of elements satisfying pred */
static size_t stable_partition(void *base, size_t n, size_t sz, Pred pred) {
    unsigned char *tmp = malloc(n * sz + 1);
    CHECK(tmp);
    size_t a = 0;
    for (size_t i = 0; i < n; i++) if (pred(AT(base, i, sz))) { memcpy(AT(tmp, a, sz), AT(base, i, sz), sz); a++; }
    size_t b = a;
    for (size_t i = 0; i < n; i++) if (!pred(AT(base, i, sz))) { memcpy(AT(tmp, b, sz), AT(base, i, sz), sz); b++; }
    memcpy(base, tmp, n * sz);
    free(tmp);
    return a;
}
static int next_permutation(void *base, size_t n, size_t sz, Cmp cmp) {
    if (n < 2) return 0;
    size_t i = n - 1;
    while (i > 0 && cmp(AT(base, i - 1, sz), AT(base, i, sz)) >= 0) i--;
    if (i == 0) { reverse_range(base, 0, n, sz); return 0; }
    size_t j = n - 1;
    while (cmp(AT(base, j, sz), AT(base, i - 1, sz)) <= 0) j--;
    swap_bytes(AT(base, i - 1, sz), AT(base, j, sz), sz);
    reverse_range(base, i, n, sz);
    return 1;
}
static size_t unique_sorted(void *base, size_t n, size_t sz, Cmp cmp) {
    if (n == 0) return 0;
    size_t w = 1;
    for (size_t i = 1; i < n; i++)
        if (cmp(AT(base, w - 1, sz), AT(base, i, sz)) != 0) { memcpy(AT(base, w, sz), AT(base, i, sz), sz); w++; }
    return w;
}
static int is_sorted(const void *base, size_t n, size_t sz, Cmp cmp) {
    for (size_t i = 1; i < n; i++) if (cmp(AT(base, i - 1, sz), AT(base, i, sz)) > 0) return 0;
    return 1;
}

typedef struct { short key; char tag[6]; } Rec;   /* 8 bytes with padding-free layout on all targets */
typedef struct { double w; int a, b, c; } Wide;   /* wider than a machine word */

static int cmp_int(const void *a, const void *b) { int x, y; memcpy(&x, a, sizeof x); memcpy(&y, b, sizeof y); return (x > y) - (x < y); }
static int cmp_rec(const void *a, const void *b) { const Rec *x = a, *y = b; return (x->key > y->key) - (x->key < y->key); }
static int cmp_wide(const void *a, const void *b) { const Wide *x = a, *y = b; return (x->w > y->w) - (x->w < y->w); }
static int is_even_int(const void *a) { int x; memcpy(&x, a, sizeof x); return x % 2 == 0; }

int main(void) {
    /* ints against a simple insertion-sorted model */
    enum { N = 300 };
    int a[N], m[N];
    for (int i = 0; i < N; i++) a[i] = m[i] = (int)(rnd() % 100);
    for (int i = 1; i < N; i++) { int v = m[i], j = i - 1; while (j >= 0 && m[j] > v) { m[j + 1] = m[j]; j--; } m[j + 1] = v; }
    int b[N];
    memcpy(b, a, sizeof a);
    heap_sort(b, N, sizeof(int), cmp_int);
    CHECK(memcmp(b, m, sizeof m) == 0);
    long checked = 0;
    for (int k = 0; k < N; k += 7) {
        memcpy(b, a, sizeof a);
        nth_element(b, N, (size_t)k, sizeof(int), cmp_int);
        CHECK(b[k] == m[k]);
        for (int i = 0; i < k; i++) CHECK(b[i] <= b[k]);
        for (int i = k + 1; i < N; i++) CHECK(b[i] >= b[k]);
        checked++;
    }
    for (int probe = -1; probe <= 100; probe += 9) {
        size_t lb = lower_bound(m, N, &probe, sizeof(int), cmp_int), ub = upper_bound(m, N, &probe, sizeof(int), cmp_int);
        size_t elb = 0, eub = 0;
        for (int i = 0; i < N; i++) { if (m[i] < probe) elb++; if (m[i] <= probe) eub++; }
        CHECK(lb == elb && ub == eub);
    }
    memcpy(b, m, sizeof m);
    size_t nu = unique_sorted(b, N, sizeof(int), cmp_int);
    size_t mu = 0;
    for (int i = 0; i < N; i++) if (i == 0 || m[i] != m[i - 1]) mu++;
    CHECK(nu == mu);
    printf("ints: heap sort ok, %ld quickselects ok, %zu distinct, median %d, min %d max %d\n", checked, nu, m[N / 2], m[0], m[N - 1]);

    /* rotation for element sizes 4, 8 and 24 */
    int r[10];
    for (int i = 0; i < 10; i++) r[i] = i;
    rotate_left(r, 10, 3, sizeof(int));
    printf("rotate left 3:");
    for (int i = 0; i < 10; i++) { printf(" %d", r[i]); CHECK(r[i] == (i + 3) % 10); }
    printf("\n");
    Wide w[9];
    for (int i = 0; i < 9; i++) { w[i].w = i * 0.5; w[i].a = i; w[i].b = i * 2; w[i].c = i * 3; }
    rotate_left(w, 9, 4, sizeof(Wide));
    for (int i = 0; i < 9; i++) CHECK(w[i].a == (i + 4) % 9 && w[i].c == 3 * ((i + 4) % 9));

    /* records with a key field: heap sort is not stable, so compare keys and multiset of tags */
    Rec rec[120], rm[120];
    for (int i = 0; i < 120; i++) {
        memset(&rec[i], 0, sizeof rec[i]);
        rec[i].key = (short)(rnd() % 40);
        snprintf(rec[i].tag, sizeof rec[i].tag, "t%d", i % 100);
        rm[i] = rec[i];
    }
    heap_sort(rec, 120, sizeof(Rec), cmp_rec);
    CHECK(is_sorted(rec, 120, sizeof(Rec), cmp_rec));
    long tag_sum_a = 0, tag_sum_b = 0;
    for (int i = 0; i < 120; i++) { tag_sum_a += atoi(rec[i].tag + 1); tag_sum_b += atoi(rm[i].tag + 1); }
    CHECK(tag_sum_a == tag_sum_b);
    Rec probe;
    memset(&probe, 0, sizeof probe);
    probe.key = 20;
    size_t lb = lower_bound(rec, 120, &probe, sizeof(Rec), cmp_rec), ub = upper_bound(rec, 120, &probe, sizeof(Rec), cmp_rec);
    printf("records: sorted by key, key 20 occupies [%zu,%zu), first key %d last key %d\n", lb, ub, rec[0].key, rec[119].key);

    /* wide elements */
    Wide wd[80];
    for (int i = 0; i < 80; i++) { wd[i].w = (double)(int)(rnd() % 1000) / 8.0; wd[i].a = i; wd[i].b = 0; wd[i].c = 0; }
    heap_sort(wd, 80, sizeof(Wide), cmp_wide);
    CHECK(is_sorted(wd, 80, sizeof(Wide), cmp_wide));
    printf("wide: min %.3f max %.3f\n", wd[0].w, wd[79].w);

    /* stable partition */
    int p[60], q[60];
    for (int i = 0; i < 60; i++) p[i] = q[i] = (int)(rnd() % 1000);
    size_t ne = stable_partition(p, 60, sizeof(int), is_even_int);
    size_t me = 0;
    int evens[60], odds[60], no = 0;
    for (int i = 0; i < 60; i++) { if (q[i] % 2 == 0) evens[me++] = q[i]; else odds[no++] = q[i]; }
    CHECK(ne == me);
    for (size_t i = 0; i < me; i++) CHECK(p[i] == evens[i]);
    for (int i = 0; i < no; i++) CHECK(p[me + (size_t)i] == odds[i]);
    printf("stable partition: %zu even values kept in order in front of %d odd values\n", ne, no);

    /* permutations of 6 distinct ints: count and lexicographic order */
    int perm[6] = { 1, 2, 3, 4, 5, 6 };
    int prev[6];
    long count = 1;
    do {
        memcpy(prev, perm, sizeof perm);
        if (!next_permutation(perm, 6, sizeof(int), cmp_int)) break;
        CHECK(memcmp(prev, perm, sizeof perm) != 0);
        int i = 0;
        while (i < 6 && prev[i] == perm[i]) i++;
        CHECK(i < 6 && prev[i] < perm[i]);
        count++;
    } while (1);
    CHECK(count == 720 && perm[0] == 1 && perm[5] == 6);
    printf("permutations of 6: %ld visited, wrapped to sorted order\n", count);
    /* multiset permutations: 1 1 2 2 3 has 30 distinct arrangements */
    int ms[5] = { 1, 1, 2, 2, 3 };
    long mc = 1;
    while (next_permutation(ms, 5, sizeof(int), cmp_int)) mc++;
    CHECK(mc == 30);
    printf("multiset permutations of 11223: %ld\n", mc);
    return 0;
}
