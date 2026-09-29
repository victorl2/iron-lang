/*
 * title: Sliding window median with two heaps and delayed deletion
 * topic: data_structures
 * covers: two-heap median, sliding window, delayed deletion counters, balance invariant, brute-force cross-check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0x3ED1A9ull;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1442695040888963407ull;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* generic binary heap of ints; sign = 1 for min-heap, -1 for max-heap */
typedef struct {
    int *a;
    int n, sign;
} H;

static int before(const H *h, int x, int y) { return h->sign * x < h->sign * y; }

static void hpush(H *h, int v) {
    int i = h->n++;
    while (i > 0 && before(h, v, h->a[(i - 1) / 2])) {
        h->a[i] = h->a[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    h->a[i] = v;
}

static void hpop(H *h) {
    int x = h->a[--h->n], i = 0;
    for (;;) {
        int c = 2 * i + 1;
        if (c >= h->n)
            break;
        if (c + 1 < h->n && before(h, h->a[c + 1], h->a[c]))
            c++;
        if (!before(h, h->a[c], x))
            break;
        h->a[i] = h->a[c];
        i = c;
    }
    if (h->n > 0)
        h->a[i] = x;
}

/* delayed deletion: values live in a small count table keyed by value */
enum { VMAX = 500 };
static int pending[VMAX]; /* pending removals per value */

typedef struct {
    H lo, hi;    /* lo: max-heap of the smaller half, hi: min-heap of the larger half */
    int lo_size, hi_size; /* logical sizes (excluding pending removals) */
    long pruned;
} Med;

static void prune(H *h, Med *m) {
    while (h->n > 0 && pending[h->a[0]] > 0) {
        pending[h->a[0]]--;
        hpop(h);
        m->pruned++;
    }
}

static void balance(Med *m) {
    if (m->lo_size > m->hi_size + 1) {
        hpush(&m->hi, m->lo.a[0]);
        hpop(&m->lo);
        m->lo_size--;
        m->hi_size++;
        prune(&m->lo, m);
    } else if (m->lo_size < m->hi_size) {
        hpush(&m->lo, m->hi.a[0]);
        hpop(&m->hi);
        m->hi_size--;
        m->lo_size++;
        prune(&m->hi, m);
    }
}

static void add(Med *m, int v) {
    if (m->lo_size == 0 || v <= m->lo.a[0]) {
        hpush(&m->lo, v);
        m->lo_size++;
    } else {
        hpush(&m->hi, v);
        m->hi_size++;
    }
    balance(m);
}

static void erase(Med *m, int v) {
    pending[v]++;
    if (v <= m->lo.a[0])
        m->lo_size--;
    else
        m->hi_size--;
    if (m->lo.a[0] == v)
        prune(&m->lo, m);
    if (m->hi_size > 0 && m->hi.a[0] == v)
        prune(&m->hi, m);
    balance(m);
    prune(&m->lo, m);
    prune(&m->hi, m);
}

static int cmp_int(const void *x, const void *y) { return *(const int *)x - *(const int *)y; }

int main(void) {
    enum { N = 6000 };
    static int data[N];
    int windows[] = {1, 2, 3, 8, 25, 100};
    for (int i = 0; i < N; i++) {
        int base = (int)(rng() % 200);
        data[i] = base + (int)(rng() % 200) / (1 + i / 1500);
    }
    for (size_t wi = 0; wi < sizeof windows / sizeof windows[0]; wi++) {
        int k = windows[wi];
        Med m;
        m.lo.a = malloc(sizeof(int) * (N + 1));
        m.hi.a = malloc(sizeof(int) * (N + 1));
        m.lo.n = m.hi.n = 0;
        m.lo.sign = -1;
        m.hi.sign = 1;
        m.lo_size = m.hi_size = 0;
        m.pruned = 0;
        memset(pending, 0, sizeof pending);
        long lower_sum = 0, upper_sum = 0;
        int checked = 0, max_phys = 0;
        for (int i = 0; i < N; i++) {
            add(&m, data[i]);
            if (i >= k)
                erase(&m, data[i - k]);
            if (i >= k - 1) {
                int w[128];
                memcpy(w, data + i - k + 1, sizeof(int) * (size_t)k);
                qsort(w, (size_t)k, sizeof(int), cmp_int);
                int lower = w[(k - 1) / 2], upper = w[k / 2];
                check(m.lo_size + m.hi_size == k, "logical window size");
                check(m.lo_size == (k + 1) / 2, "lower half holds ceil(k/2)");
                check(m.lo.a[0] == lower, "lower median matches sorted window");
                int up = (k % 2) ? m.lo.a[0] : m.hi.a[0];
                check(up == upper, "upper median matches sorted window");
                lower_sum += lower;
                upper_sum += upper;
                checked++;
                int phys = m.lo.n + m.hi.n;
                if (phys > max_phys)
                    max_phys = phys;
            }
        }
        printf("k=%-4d windows=%d lower_sum=%ld upper_sum=%ld pruned=%ld max_physical=%d\n", k, checked, lower_sum, upper_sum, m.pruned, max_phys);
        free(m.lo.a);
        free(m.hi.a);
    }
    return 0;
}
