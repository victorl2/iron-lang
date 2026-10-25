/*
 * title: Pointer-based binary heap on a complete linked tree
 * topic: data_structures
 * covers: linked heap, bit-path addressing of the last node, parent-walk successor slot, payload swapping, completeness check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0xC0FFEEull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct TNode {
    int key;
    struct TNode *parent, *l, *r;
} TNode;

typedef struct {
    TNode *root, *last;
    int n;
    long swaps, walk_steps, path_steps;
} LHeap;

/* node number k (1-based, BFS order): follow the bits of k after its leading one */
static TNode *node_by_bits(LHeap *h, int k) {
    int top = 1;
    while ((top << 1) <= k)
        top <<= 1;
    TNode *x = h->root;
    for (int bit = top >> 1; bit > 0; bit >>= 1) {
        h->path_steps++;
        x = (k & bit) ? x->r : x->l;
    }
    return x;
}

/* parent of the next free slot, found by walking from the last node */
static TNode *next_parent_by_walk(LHeap *h, int *go_right) {
    if (!h->root) {
        *go_right = 0;
        return NULL;
    }
    TNode *x = h->last;
    while (x->parent && x->parent->r == x) {
        h->walk_steps++;
        x = x->parent;
    }
    if (!x->parent) { /* last was rightmost on its level: new level, go down the left spine */
        TNode *y = h->root;
        while (y->l) {
            h->walk_steps++;
            y = y->l;
        }
        *go_right = 0;
        return y;
    }
    /* x is a left child: sibling subtree's leftmost leaf receives the node */
    if (!x->parent->r) {
        *go_right = 1;
        return x->parent;
    }
    TNode *y = x->parent->r;
    while (y->l) {
        h->walk_steps++;
        y = y->l;
    }
    *go_right = 0;
    return y;
}

static void sift_up(LHeap *h, TNode *x) {
    while (x->parent && x->key < x->parent->key) {
        int t = x->key;
        x->key = x->parent->key;
        x->parent->key = t;
        h->swaps++;
        x = x->parent;
    }
}

static void sift_down(LHeap *h, TNode *x) {
    for (;;) {
        TNode *m = x->l;
        if (x->r && x->r->key < m->key)
            m = x->r;
        if (!m || m->key >= x->key)
            return;
        int t = x->key;
        x->key = m->key;
        m->key = t;
        h->swaps++;
        x = m;
        if (!x->l)
            return;
    }
}

static void push(LHeap *h, int key) {
    TNode *nd = calloc(1, sizeof(TNode));
    nd->key = key;
    int right;
    TNode *p = next_parent_by_walk(h, &right);
    if (!p) {
        h->root = nd;
    } else {
        /* cross-check against bit-path addressing of slot number n+1 */
        TNode *q = node_by_bits(h, (h->n + 1) / 2);
        check(q == p, "walk and bit-path agree on the parent slot");
        check(((h->n + 1) & 1) == (unsigned)right, "walk and bit-path agree on the side");
        nd->parent = p;
        if (right)
            p->r = nd;
        else
            p->l = nd;
    }
    h->last = nd;
    h->n++;
    sift_up(h, nd);
}

static int pop(LHeap *h) {
    int top = h->root->key;
    TNode *last = h->last;
    if (h->n == 1) {
        free(last);
        h->root = h->last = NULL;
        h->n = 0;
        return top;
    }
    h->root->key = last->key;
    /* new last node is number n-1 */
    TNode *par = last->parent;
    if (par->r == last)
        par->r = NULL;
    else
        par->l = NULL;
    free(last);
    h->n--;
    h->last = node_by_bits(h, h->n);
    if (h->root->l)
        sift_down(h, h->root);
    return top;
}

static int verify(const TNode *x, const TNode *parent, int depth, int *maxdepth) {
    if (!x)
        return 0;
    check(x->parent == parent, "parent pointer");
    check(!parent || parent->key <= x->key, "heap order");
    check(x->l || !x->r, "no right child without a left child");
    if (depth > *maxdepth)
        *maxdepth = depth;
    return 1 + verify(x->l, x, depth + 1, maxdepth) + verify(x->r, x, depth + 1, maxdepth);
}

/* completeness: BFS numbering must match bit-path addressing for every k in 1..n */
static void complete_check(LHeap *h) {
    TNode *queue[4096];
    int head = 0, tail = 0, k = 1;
    if (!h->root)
        return;
    queue[tail++] = h->root;
    int seen_gap = 0;
    while (head < tail) {
        TNode *x = queue[head++];
        check(node_by_bits(h, k) == x, "BFS position equals bit-path node");
        k++;
        TNode *ch[2] = {x->l, x->r};
        for (int i = 0; i < 2; i++) {
            if (ch[i]) {
                check(!seen_gap, "complete: no node after a gap");
                queue[tail++] = ch[i];
            } else
                seen_gap = 1;
        }
    }
    check(k - 1 == h->n, "count");
    check(node_by_bits(h, h->n) == h->last, "last pointer");
}

static void destroy(TNode *x) {
    if (!x)
        return;
    destroy(x->l);
    destroy(x->r);
    free(x);
}

int main(void) {
    LHeap h = {NULL, NULL, 0, 0, 0, 0};
    int model[4000], mn = 0;
    long popsum = 0;
    int pushes = 0, pops = 0, maxn = 0;
    for (int op = 0; op < 8000; op++) {
        if ((rng() % 100 < 53 || mn == 0) && mn < 3000) {
            int v = (int)(rng() % 20000);
            model[mn++] = v;
            push(&h, v);
            pushes++;
        } else {
            int bi = 0;
            for (int i = 1; i < mn; i++)
                if (model[i] < model[bi])
                    bi = i;
            int want = model[bi];
            model[bi] = model[--mn];
            int got = pop(&h);
            check(got == want, "pop equals model");
            popsum += got;
            pops++;
        }
        if (mn > maxn)
            maxn = mn;
        int md = 0;
        check(verify(h.root, NULL, 0, &md) == mn && h.n == mn, "size");
        if (op % 7 == 0)
            complete_check(&h);
    }
    int md = 0;
    verify(h.root, NULL, 0, &md);
    printf("pushes=%d pops=%d peak=%d final=%d height=%d\n", pushes, pops, maxn, h.n, md);
    printf("popsum=%ld swaps=%ld walk_steps=%ld path_steps=%ld\n", popsum, h.swaps, h.walk_steps, h.path_steps);
    destroy(h.root);
    return 0;
}
