/*
 * title: Skew heap with iterative top-down meld
 * topic: data_structures
 * covers: skew heap, amortized analysis, right-path merge without recursion, path length statistics
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 271828;
static unsigned rng(void) {
    rs = rs * 6364136223846793005ull + 1;
    return (unsigned)(rs >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct SNode {
    int key;
    struct SNode *l, *r;
} SNode;

static long steps;

/* Top-down skew meld: walk down the merged right paths, swapping children at each visited node. */
static SNode *meld(SNode *a, SNode *b) {
    SNode *root = NULL, **link = &root;
    while (a && b) {
        steps++;
        if (b->key < a->key) {
            SNode *t = a;
            a = b;
            b = t;
        }
        /* a is the smaller root: its old left child becomes the right child, and
         * the merge of its old right child with b becomes the new left child. */
        SNode *oldl = a->l, *oldr = a->r;
        a->r = oldl;
        *link = a;
        link = &a->l;
        a = oldr;
    }
    *link = a ? a : b;
    return root;
}

static SNode *push(SNode *h, int k) {
    SNode *n = calloc(1, sizeof(SNode));
    n->key = k;
    return meld(h, n);
}

static SNode *pop(SNode *h, int *k) {
    SNode *l = h->l, *r = h->r;
    *k = h->key;
    free(h);
    return meld(l, r);
}

static long size_check(const SNode *x, int *height, int d) {
    if (!x)
        return 0;
    if (d > *height)
        *height = d;
    check(!x->l || x->l->key >= x->key, "left order");
    check(!x->r || x->r->key >= x->key, "right order");
    return 1 + size_check(x->l, height, d + 1) + size_check(x->r, height, d + 1);
}

static void destroy(SNode *x) {
    if (!x)
        return;
    destroy(x->l);
    destroy(x->r);
    free(x);
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    enum { N = 2000 };
    static int vals[N], sorted[N];
    for (int i = 0; i < N; i++)
        vals[i] = (int)(rng() % 10000);
    for (int i = 0; i < N; i++)
        sorted[i] = vals[i];
    qsort(sorted, N, sizeof(int), cmp_int);

    /* phase 1: insert everything, then drain */
    SNode *h = NULL;
    int maxh = 0;
    for (int i = 0; i < N; i++)
        h = push(h, vals[i]);
    long build_steps = steps;
    long n = size_check(h, &maxh, 0);
    check(n == N, "size after build");
    int k;
    for (int i = 0; i < N; i++) {
        h = pop(h, &k);
        check(k == sorted[i], "drain equals sorted");
        if (i % 200 == 0) {
            int hh = 0;
            check(size_check(h, &hh, 0) == N - i - 1, "size while draining");
        }
    }
    printf("build steps=%ld height=%d drain steps=%ld\n", build_steps, maxh, steps - build_steps);

    /* phase 2: adversarial ascending inserts keep a long left path but stay correct */
    steps = 0;
    for (int i = 0; i < 500; i++)
        h = push(h, i);
    int hh = 0;
    size_check(h, &hh, 0);
    printf("ascending: steps=%ld height=%d\n", steps, hh);
    destroy(h);
    h = NULL;

    /* phase 3: random melds of small heaps against a sorted model */
    SNode *parts[8] = {0};
    int cnt[8] = {0};
    long total = 0;
    for (int round = 0; round < 40; round++) {
        int a = (int)(rng() % 8), b = (int)(rng() % 8);
        for (int i = 0; i < 20; i++) {
            parts[a] = push(parts[a], (int)(rng() % 1000));
            cnt[a]++;
        }
        if (a != b) {
            parts[a] = meld(parts[a], parts[b]);
            parts[b] = NULL;
            cnt[a] += cnt[b];
            cnt[b] = 0;
        }
        for (int i = 0; i < 8; i++) {
            int hx = 0;
            check(size_check(parts[i], &hx, 0) == cnt[i], "part size");
        }
    }
    for (int i = 0; i < 8; i++) {
        int prev = -1;
        long c = 0;
        while (parts[i]) {
            parts[i] = pop(parts[i], &k);
            check(k >= prev, "part drains ascending");
            prev = k;
            c++;
        }
        check(c == cnt[i], "part drained fully");
        total += c;
    }
    printf("melded parts total=%ld\n", total);
    return 0;
}
