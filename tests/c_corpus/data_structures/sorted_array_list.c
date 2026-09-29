/*
 * title: Sorted array list with binary search bounds
 * topic: data_structures
 * covers: sorted list, lower_bound, upper_bound, equal_range, insert keeps order, multiset semantics, model check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x3C6EF372FE94F82BULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 10); }

typedef struct { int *a; size_t n, cap; } SList;

static size_t lower_bound(const SList *l, int x) {
    size_t lo = 0, hi = l->n;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (l->a[mid] < x) lo = mid + 1; else hi = mid; }
    return lo;
}
static size_t upper_bound(const SList *l, int x) {
    size_t lo = 0, hi = l->n;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (l->a[mid] <= x) lo = mid + 1; else hi = mid; }
    return lo;
}
static void insert(SList *l, int x) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->a = realloc(l->a, l->cap * sizeof(int)); CHECK(l->a); }
    size_t p = upper_bound(l, x);
    memmove(l->a + p + 1, l->a + p, (l->n - p) * sizeof(int));
    l->a[p] = x; l->n++;
}
static int insert_unique(SList *l, int x) {
    size_t p = lower_bound(l, x);
    if (p < l->n && l->a[p] == x) return 0;
    insert(l, x); return 1;
}
static size_t erase_all(SList *l, int x) {
    size_t lo = lower_bound(l, x), hi = upper_bound(l, x);
    memmove(l->a + lo, l->a + hi, (l->n - hi) * sizeof(int));
    l->n -= hi - lo; return hi - lo;
}
static int erase_one(SList *l, int x) {
    size_t p = lower_bound(l, x);
    if (p == l->n || l->a[p] != x) return 0;
    memmove(l->a + p, l->a + p + 1, (l->n - p - 1) * sizeof(int)); l->n--; return 1;
}
static long count_range(const SList *l, int lo, int hi) { /* [lo, hi] */
    return lo > hi ? 0 : (long)(upper_bound(l, hi) - lower_bound(l, lo));
}
/* nearest element to x; on a tie prefer the smaller */
static int nearest(const SList *l, int x, int *out) {
    if (!l->n) return 0;
    size_t p = lower_bound(l, x);
    if (p == 0) *out = l->a[0];
    else if (p == l->n) *out = l->a[l->n - 1];
    else *out = (x - l->a[p - 1] <= l->a[p] - x) ? l->a[p - 1] : l->a[p];
    return 1;
}

int main(void) {
    SList l = {0};
    static int m[4000]; size_t mn = 0;
    long cnt[7] = {0};
    for (int step = 0; step < 15000; step++) {
        unsigned op = rnd() % 100;
        int x = (int)(rnd() % 300);
        if (mn > 3000) op = 60;
        if (op < 35) { insert(&l, x); size_t p = 0; while (p < mn && m[p] <= x) p++; memmove(m + p + 1, m + p, (mn - p) * sizeof(int)); m[p] = x; mn++; cnt[0]++; }
        else if (op < 50) {
            int has = 0; for (size_t i = 0; i < mn; i++) if (m[i] == x) has = 1;
            int r = insert_unique(&l, x); CHECK(r == !has);
            if (r) { size_t p = 0; while (p < mn && m[p] < x) p++; memmove(m + p + 1, m + p, (mn - p) * sizeof(int)); m[p] = x; mn++; }
            cnt[1]++;
        } else if (op < 62) {
            int r = erase_one(&l, x); size_t p = 0; while (p < mn && m[p] != x) p++;
            CHECK(r == (p < mn)); if (r) { memmove(m + p, m + p + 1, (mn - p - 1) * sizeof(int)); mn--; } cnt[2]++;
        } else if (op < 68) {
            size_t r = erase_all(&l, x), k = 0; for (size_t i = 0; i < mn; i++) if (m[i] != x) m[k++] = m[i];
            CHECK(r == mn - k); mn = k; cnt[3]++;
        } else if (op < 85) {
            int y = (int)(rnd() % 300); long g = 0; for (size_t i = 0; i < mn; i++) if (m[i] >= x && m[i] <= y) g++;
            CHECK(count_range(&l, x, y) == g); cnt[4]++;
        } else {
            int got = -1, want = -1, ok = nearest(&l, x, &got);
            if (mn) { int bd = 1 << 30; for (size_t i = 0; i < mn; i++) { int d = abs(m[i] - x); if (d < bd) { bd = d; want = m[i]; } } }
            CHECK(ok == (mn > 0) && (!ok || got == want)); cnt[5]++;
        }
        CHECK(l.n == mn);
        CHECK(memcmp(l.a, m, mn * sizeof(int)) == 0);
        for (size_t i = 1; i < mn; i++) CHECK(m[i - 1] <= m[i]);
        cnt[6] += (long)mn;
    }
    printf("insert=%ld insert_unique=%ld erase_one=%ld erase_all=%ld range=%ld nearest=%ld\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
    printf("final n=%zu avg_size=%ld\n", l.n, cnt[6] / 15000);
    printf("min=%d median=%d max=%d\n", l.n ? l.a[0] : -1, l.n ? l.a[l.n / 2] : -1, l.n ? l.a[l.n - 1] : -1);
    printf("count of 100..199 = %ld\n", count_range(&l, 100, 199));
    free(l.a);
    return 0;
}
