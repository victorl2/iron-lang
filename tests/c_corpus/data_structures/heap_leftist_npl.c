/*
 * title: Leftist heap with null-path length and linear-time build
 * topic: data_structures
 * covers: leftist heap, null path length, recursive meld, right spine bound, queue-based heapify
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 5150;
static unsigned rng(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return (unsigned)((rs * 0x2545F4914F6CDD1Dull) >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct LNode {
    int key, npl;
    struct LNode *l, *r;
} LNode;

static int npl(const LNode *x) { return x ? x->npl : -1; }

static LNode *meld(LNode *a, LNode *b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (b->key < a->key) {
        LNode *t = a;
        a = b;
        b = t;
    }
    a->r = meld(a->r, b);
    if (npl(a->l) < npl(a->r)) {
        LNode *t = a->l;
        a->l = a->r;
        a->r = t;
    }
    a->npl = npl(a->r) + 1;
    return a;
}

static LNode *push(LNode *h, int k) {
    LNode *n = calloc(1, sizeof(LNode));
    n->key = k;
    return meld(h, n);
}

static LNode *pop(LNode *h, int *k) {
    LNode *l = h->l, *r = h->r;
    *k = h->key;
    free(h);
    return meld(l, r);
}

static long verify(const LNode *x, int depth_from_root, int *maxdepth) {
    if (!x)
        return 0;
    if (depth_from_root > *maxdepth)
        *maxdepth = depth_from_root;
    if (x->l)
        check(x->l->key >= x->key, "order left");
    if (x->r)
        check(x->r->key >= x->key, "order right");
    check(npl(x->l) >= npl(x->r), "leftist property");
    check(x->npl == npl(x->r) + 1, "npl definition");
    return 1 + verify(x->l, depth_from_root + 1, maxdepth) + verify(x->r, depth_from_root + 1, maxdepth);
}

static int right_spine(const LNode *x) {
    int n = 0;
    for (; x; x = x->r)
        n++;
    return n;
}

static int floor_log2(long n) {
    int k = 0;
    while (n > 1) {
        n >>= 1;
        k++;
    }
    return k;
}

static long full_check(const LNode *h) {
    int md = 0;
    long n = verify(h, 0, &md);
    if (h)
        check(right_spine(h) <= floor_log2(n + 1) + 1, "right spine is O(log n)");
    return n;
}

/* linear-time build: queue of singleton heaps, meld the two at the front */
static LNode *build_queue(const int *v, int n) {
    LNode **q = malloc(sizeof(LNode *) * (size_t)(2 * n + 2));
    int head = 0, tail = 0;
    for (int i = 0; i < n; i++) {
        LNode *x = calloc(1, sizeof(LNode));
        x->key = v[i];
        q[tail++] = x;
    }
    while (tail - head > 1) {
        LNode *a = q[head++], *b = q[head++];
        q[tail++] = meld(a, b);
    }
    LNode *r = tail > head ? q[head] : NULL;
    free(q);
    return r;
}

static void destroy(LNode *x) {
    if (!x)
        return;
    destroy(x->l);
    destroy(x->r);
    free(x);
}

int main(void) {
    LNode *H[2] = {NULL, NULL};
    int model[2][1500], mn[2] = {0, 0};
    long popsum = 0;
    int melds = 0;
    for (int op = 0; op < 3000; op++) {
        int r = (int)(rng() % 100), s = (int)(rng() % 2);
        if ((r < 55 || mn[s] == 0) && mn[s] < 1500) {
            int k = (int)(rng() % 5000);
            H[s] = push(H[s], k);
            model[s][mn[s]++] = k;
        } else if (r < 92 && mn[s] > 0) {
            int bi = 0;
            for (int i = 1; i < mn[s]; i++)
                if (model[s][i] < model[s][bi])
                    bi = i;
            int want = model[s][bi], got;
            model[s][bi] = model[s][--mn[s]];
            H[s] = pop(H[s], &got);
            check(got == want, "pop equals model");
            popsum += got;
        } else if (mn[0] + mn[1] < 1500) {
            H[0] = meld(H[0], H[1]);
            H[1] = NULL;
            for (int i = 0; i < mn[1]; i++)
                model[0][mn[0]++] = model[1][i];
            mn[1] = 0;
            melds++;
        }
        for (int h = 0; h < 2; h++)
            check(full_check(H[h]) == mn[h], "size");
    }
    int md = 0;
    long size = verify(H[0], 0, &md);
    printf("melds=%d popsum=%ld heap0_size=%ld heap0_height=%d spine=%d\n", melds, popsum, size, md, right_spine(H[0]));
    destroy(H[0]);
    destroy(H[1]);

    int vals[1000];
    for (int i = 0; i < 1000; i++)
        vals[i] = (int)(rng() % 100000);
    LNode *b = build_queue(vals, 1000);
    md = 0;
    check(verify(b, 0, &md) == 1000, "queue build size");
    printf("queue build: height=%d spine=%d root=%d\n", md, right_spine(b), b->key);
    int prev = -1, k;
    for (int i = 0; i < 1000; i++) {
        b = pop(b, &k);
        check(k >= prev, "queue-built heap drains ascending");
        prev = k;
    }
    check(b == NULL, "empty at end");
    printf("last=%d\n", prev);
    return 0;
}
