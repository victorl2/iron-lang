/*
 * title: Selecting the k smallest inside a heap without modifying it
 * topic: data_structures
 * covers: heap-ordered tree selection, auxiliary frontier heap, pruned traversal, d-ary index arithmetic, visit-count bounds
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0xF4ED3ull;
static unsigned rng(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return (unsigned)((rs * 0x2545F4914F6CDD1Dull) >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* ---- array d-ary heap ---- */
static void build_dary(int *a, int n, int d) {
    for (int i = n - 1; i >= 0; i--) {
        /* sift down from i */
        int x = a[i], k = i;
        for (;;) {
            int first = k * d + 1;
            if (first >= n)
                break;
            int best = first, last = first + d < n ? first + d : n;
            for (int c = first + 1; c < last; c++)
                if (a[c] < a[best])
                    best = c;
            if (a[best] >= x)
                break;
            a[k] = a[best];
            k = best;
        }
        a[k] = x;
    }
}

/* frontier heap of array indices ordered by the heap value stored there */
typedef struct {
    int *idx;
    int n;
    const int *val;
    long ops;
} Frontier;

static void fpush(Frontier *f, int i) {
    int k = f->n++;
    while (k > 0) {
        f->ops++;
        int p = (k - 1) / 2;
        if (f->val[f->idx[p]] <= f->val[i])
            break;
        f->idx[k] = f->idx[p];
        k = p;
    }
    f->idx[k] = i;
}

static int fpop(Frontier *f) {
    int top = f->idx[0], x = f->idx[--f->n], k = 0;
    for (;;) {
        int c = 2 * k + 1;
        if (c >= f->n)
            break;
        f->ops++;
        if (c + 1 < f->n && f->val[f->idx[c + 1]] < f->val[f->idx[c]])
            c++;
        if (f->val[f->idx[c]] >= f->val[x])
            break;
        f->idx[k] = f->idx[c];
        k = c;
    }
    if (f->n > 0)
        f->idx[k] = x;
    return top;
}

static long k_smallest(const int *a, int n, int d, int k, int *out, int *frontier_peak) {
    Frontier f = {malloc(sizeof(int) * ((size_t)k * (size_t)d + 2)), 0, a, 0};
    *frontier_peak = 0;
    if (n > 0)
        fpush(&f, 0);
    for (int t = 0; t < k && f.n > 0; t++) {
        int i = fpop(&f);
        out[t] = a[i];
        for (int c = i * d + 1; c <= i * d + d && c < n; c++)
            fpush(&f, c);
        if (f.n > *frontier_peak)
            *frontier_peak = f.n;
    }
    long ops = f.ops;
    free(f.idx);
    return ops;
}

/* pruned traversal: visit only nodes whose value is < x; each visited node has a smaller parent */
static long count_less(const int *a, int n, int d, int i, int x, long *visits) {
    if (i >= n)
        return 0;
    (*visits)++;
    if (a[i] >= x)
        return 0;
    long c = 1;
    for (int j = i * d + 1; j <= i * d + d && j < n; j++)
        c += count_less(a, n, d, j, x, visits);
    return c;
}

/* ---- pointer-based leftist tree, same selection idea on child pointers ---- */
typedef struct LN {
    int key, npl;
    struct LN *l, *r;
} LN;

static int npl(const LN *x) { return x ? x->npl : -1; }

static LN *meld(LN *a, LN *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (b->key < a->key) {
        LN *t = a;
        a = b;
        b = t;
    }
    a->r = meld(a->r, b);
    if (npl(a->l) < npl(a->r)) {
        LN *t = a->l;
        a->l = a->r;
        a->r = t;
    }
    a->npl = npl(a->r) + 1;
    return a;
}

static void lfree(LN *x) {
    if (x) {
        lfree(x->l);
        lfree(x->r);
        free(x);
    }
}

typedef struct {
    LN **a;
    int n;
} NF;

static void npush(NF *f, LN *x) {
    int k = f->n++;
    while (k > 0 && x->key < f->a[(k - 1) / 2]->key) {
        f->a[k] = f->a[(k - 1) / 2];
        k = (k - 1) / 2;
    }
    f->a[k] = x;
}

static LN *npop(NF *f) {
    LN *top = f->a[0], *x = f->a[--f->n];
    int k = 0;
    for (;;) {
        int c = 2 * k + 1;
        if (c >= f->n)
            break;
        if (c + 1 < f->n && f->a[c + 1]->key < f->a[c]->key)
            c++;
        if (f->a[c]->key >= x->key)
            break;
        f->a[k] = f->a[c];
        k = c;
    }
    if (f->n > 0)
        f->a[k] = x;
    return top;
}

int main(void) {
    enum { N = 5000 };
    static int base[N], sorted[N], heap[N], out[N];
    for (int i = 0; i < N; i++)
        base[i] = (int)(rng() % 100000);
    memcpy(sorted, base, sizeof base);
    qsort(sorted, N, sizeof(int), cmp_int);

    int ks[] = {1, 2, 10, 100, 1000, N};
    for (int d = 2; d <= 4; d++) {
        memcpy(heap, base, sizeof base);
        build_dary(heap, N, d);
        int snapshot[N];
        memcpy(snapshot, heap, sizeof heap);
        printf("d=%d\n", d);
        for (size_t t = 0; t < sizeof ks / sizeof ks[0]; t++) {
            int k = ks[t], peak;
            long ops = k_smallest(heap, N, d, k, out, &peak);
            for (int i = 0; i < k; i++)
                check(out[i] == sorted[i], "k smallest equal the sorted prefix");
            check(memcmp(snapshot, heap, sizeof heap) == 0, "the heap array is untouched");
            check(peak <= (d - 1) * k + 1, "frontier stays within (d-1)k+1");
            printf("  k=%-5d frontier_peak=%-6d frontier_ops=%ld\n", k, peak, ops);
        }
        int xs[] = {0, 500, 20000, 100001};
        for (size_t t = 0; t < sizeof xs / sizeof xs[0]; t++) {
            long visits = 0, brute = 0;
            long c = count_less(heap, N, d, 0, xs[t], &visits);
            for (int i = 0; i < N; i++)
                brute += base[i] < xs[t];
            check(c == brute, "pruned count equals brute-force count");
            check(visits <= d * c + 1, "visits bounded by d*count+1");
            printf("  count(<%d)=%ld visits=%ld\n", xs[t], c, visits);
        }
    }

    /* leftist tree: enumerate the 200 smallest by walking child pointers */
    LN *root = NULL;
    for (int i = 0; i < N; i++) {
        LN *n = calloc(1, sizeof(LN));
        n->key = base[i];
        root = meld(root, n);
    }
    NF f = {malloc(sizeof(LN *) * 1000), 0};
    npush(&f, root);
    int rank_sum = 0;
    for (int t = 0; t < 200; t++) {
        LN *x = npop(&f);
        check(x->key == sorted[t], "leftist frontier enumerates in sorted order");
        rank_sum += x->npl;
        if (x->l)
            npush(&f, x->l);
        if (x->r)
            npush(&f, x->r);
    }
    printf("leftist: 200 smallest verified, sum of npl values=%d\n", rank_sum);
    free(f.a);
    lfree(root);
    return 0;
}
