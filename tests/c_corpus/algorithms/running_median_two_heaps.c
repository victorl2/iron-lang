/*
 * title: Running median with two heaps
 * topic: algorithms
 * covers: streaming selection, max-heap and min-heap balancing, sliding-window median by insertion into sorted buffer, brute-force check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 8888u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef struct {
    int *v;
    int n;
    int sign; /* +1: min-heap, -1: max-heap (stores negated compare) */
} Heap;

static int before(const Heap *h, int a, int b) { return h->sign > 0 ? a < b : a > b; }

static void push(Heap *h, int x) {
    int i = h->n++;
    h->v[i] = x;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!before(h, h->v[i], h->v[p]))
            break;
        int t = h->v[i];
        h->v[i] = h->v[p];
        h->v[p] = t;
        i = p;
    }
}

static int pop(Heap *h) {
    int top = h->v[0];
    h->v[0] = h->v[--h->n];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, b = i;
        if (l < h->n && before(h, h->v[l], h->v[b]))
            b = l;
        if (r < h->n && before(h, h->v[r], h->v[b]))
            b = r;
        if (b == i)
            break;
        int t = h->v[i];
        h->v[i] = h->v[b];
        h->v[b] = t;
        i = b;
    }
    return top;
}

typedef struct {
    Heap lo, hi; /* lo: max-heap of the smaller half, hi: min-heap of the larger half */
} Median;

static void med_add(Median *m, int x) {
    if (m->lo.n == 0 || x <= m->lo.v[0])
        push(&m->lo, x);
    else
        push(&m->hi, x);
    if (m->lo.n > m->hi.n + 1)
        push(&m->hi, pop(&m->lo));
    else if (m->hi.n > m->lo.n)
        push(&m->lo, pop(&m->hi));
}

/* twice the median */
static long med_get2(const Median *m) {
    if (m->lo.n > m->hi.n)
        return 2L * m->lo.v[0];
    return (long)m->lo.v[0] + m->hi.v[0];
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    enum { N = 1500, W = 25 };
    static int a[N], hb[N], lb[N], sorted[N];
    Median m = {{lb, 0, -1}, {hb, 0, +1}};
    long chk = 0;
    for (int i = 0; i < N; i++) {
        a[i] = (int)(rnd() % 10000) - 5000;
        med_add(&m, a[i]);
        memcpy(sorted, a, (size_t)(i + 1) * sizeof(int));
        if (i % 7 == 0 || i < 30) {
            qsort(sorted, (size_t)(i + 1), sizeof(int), cmp_int);
            int n = i + 1;
            long want = n % 2 ? 2L * sorted[n / 2] : (long)sorted[n / 2 - 1] + sorted[n / 2];
            if (med_get2(&m) != want)
                fail("running median");
        }
        chk += med_get2(&m);
    }
    printf("running median checksum (2x): %ld\n", chk);
    printf("final medians: %ld.%s\n", med_get2(&m) / 2, med_get2(&m) % 2 ? "5" : "0");

    /* sliding-window median via a maintained sorted buffer */
    int buf[W];
    long wchk = 0;
    int cnt = 0;
    for (int i = 0; i < N; i++) {
        if (i >= W) { /* remove a[i-W] */
            int lo = 0, hi = cnt - 1;
            while (lo < hi) {
                int mid = lo + (hi - lo) / 2;
                if (buf[mid] < a[i - W])
                    lo = mid + 1;
                else
                    hi = mid;
            }
            memmove(buf + lo, buf + lo + 1, (size_t)(cnt - lo - 1) * sizeof(int));
            cnt--;
        }
        int lo = 0, hi = cnt;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (buf[mid] < a[i])
                lo = mid + 1;
            else
                hi = mid;
        }
        memmove(buf + lo + 1, buf + lo, (size_t)(cnt - lo) * sizeof(int));
        buf[lo] = a[i];
        cnt++;
        if (i >= W - 1) {
            memcpy(sorted, a + i - W + 1, W * sizeof(int));
            qsort(sorted, W, sizeof(int), cmp_int);
            if (sorted[W / 2] != buf[W / 2])
                fail("window median");
            wchk += buf[W / 2];
        }
    }
    printf("window(%d) median checksum: %ld\n", W, wchk);
    return 0;
}
