/*
 * title: Parallel balanced BST construction from a sorted array
 * topic: concurrency
 * covers: recursive divide with thread budget, subtree ownership, node pool slices, traversal and height checks
 * deps: libc, pthread
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline uint64_t sm64(uint64_t *s) {
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

enum { N = 5000 };

typedef struct Node {
    int key;
    struct Node *l, *r;
} Node;

static Node *build_seq(const int *keys, int lo, int hi, Node *pool_base) {
    if (lo >= hi)
        return NULL;
    int mid = lo + (hi - lo) / 2;
    Node *n = &pool_base[mid];
    n->key = keys[mid];
    n->l = build_seq(keys, lo, mid, pool_base);
    n->r = build_seq(keys, mid + 1, hi, pool_base);
    return n;
}

typedef struct {
    const int *keys;
    int lo, hi;
    Node *pool_base;
    int threads;
    Node *result;
} Task;

static Node *build_par(const int *keys, int lo, int hi, Node *pool_base, int threads);

static void *task_main(void *p) {
    Task *t = p;
    t->result = build_par(t->keys, t->lo, t->hi, t->pool_base, t->threads);
    return NULL;
}

/* Left subtree is built by a new thread while this thread builds the right one.
 * Each thread splits its thread budget with the child. Subtrees write disjoint
 * pool slots (the pool is indexed by key position). */
static Node *build_par(const int *keys, int lo, int hi, Node *pool_base, int threads) {
    if (lo >= hi)
        return NULL;
    if (threads <= 1)
        return build_seq(keys, lo, hi, pool_base);
    int mid = lo + (hi - lo) / 2;
    Node *n = &pool_base[mid];
    n->key = keys[mid];
    Task t = {keys, lo, mid, pool_base, threads / 2, NULL};
    pthread_t th;
    if (pthread_create(&th, NULL, task_main, &t) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        exit(1);
    }
    n->r = build_par(keys, mid + 1, hi, pool_base, threads - threads / 2);
    pthread_join(th, NULL);
    n->l = t.result;
    return n;
}

static int height(const Node *n) {
    if (!n)
        return 0;
    int a = height(n->l), b = height(n->r);
    return 1 + (a > b ? a : b);
}

static int inorder(const Node *n, int *out, int k) {
    if (!n)
        return k;
    k = inorder(n->l, out, k);
    out[k++] = n->key;
    return inorder(n->r, out, k);
}

static int same_shape(const Node *a, const Node *b) {
    if (!a || !b)
        return a == b;
    return a->key == b->key && same_shape(a->l, b->l) && same_shape(a->r, b->r);
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    static int keys[N], walk[N];
    static Node p1[N], p2[N];
    uint64_t seed = 8;
    int sizes[] = {1, 2, 3, 100, 1023, 1024, 5000};
    for (int t = 0; t < 7; t++) {
        int n = sizes[t];
        for (int i = 0; i < n; i++)
            keys[i] = (int)(sm64(&seed) % 100000u);
        qsort(keys, (size_t)n, sizeof(int), cmp_int);
        /* dedupe is unnecessary for the shape test; equal keys are fine */
        Node *seq = build_seq(keys, 0, n, p1);
        Node *par = build_par(keys, 0, n, p2, 8);
        check(same_shape(seq, par), "parallel tree has the same shape");
        int k = inorder(par, walk, 0);
        check(k == n && memcmp(walk, keys, (size_t)n * sizeof(int)) == 0, "inorder equals sorted input");
        int h = height(par);
        int lg = 0;
        while ((1 << lg) < n + 1)
            lg++;
        check(h == lg, "perfectly balanced height");
        printf("n=%4d height=%2d root=%d leftmost=%d rightmost=%d\n", n, h, par->key, walk[0],
               walk[n - 1]);
    }
    return 0;
}
