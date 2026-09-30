/*
 * title: BST with parent pointers and bidirectional iterators
 * topic: data_structures
 * covers: binary search tree, parent pointers, successor and predecessor iteration without stack, lower_bound and upper_bound, erase returning next iterator, range erase, distance, sorted-array model
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048

static unsigned long long rs = 88172645463325252ULL;
unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}
void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) exit(2);
    return p;
}
/* reference model: sorted array of unique keys */
static int mod[MAXN], mn;
int mfind(int k) {
    int lo = 0, hi = mn;
    while (lo < hi) { int m = (lo + hi) / 2; if (mod[m] < k) lo = m + 1; else hi = m; }
    return lo;
}
int mhas(int k) { int p = mfind(k); return p < mn && mod[p] == k; }
int minsert(int k) {
    int p = mfind(k);
    if (p < mn && mod[p] == k) return 0;
    memmove(mod + p + 1, mod + p, (size_t)(mn - p) * sizeof(int));
    mod[p] = k; mn++;
    return 1;
}
int mdelete(int k) {
    int p = mfind(k);
    if (p >= mn || mod[p] != k) return 0;
    memmove(mod + p, mod + p + 1, (size_t)(mn - p - 1) * sizeof(int));
    mn--;
    return 1;
}
int no, outk[MAXN];

typedef struct N {
    int k;
    struct N *l, *r, *p;
} N;

typedef struct { N *root; int size; } Tree;

static N *first(N *t) { while (t && t->l) t = t->l; return t; }
static N *last(N *t) { while (t && t->r) t = t->r; return t; }
static N *next(N *x) {
    if (x->r) return first(x->r);
    while (x->p && x == x->p->r) x = x->p;
    return x->p;
}
static N *prev(N *x) {
    if (x->l) return last(x->l);
    while (x->p && x == x->p->l) x = x->p;
    return x->p;
}
/* first node with key >= k */
static N *lower_bound(Tree *T, int k) {
    N *x = T->root, *best = NULL;
    while (x) {
        if (x->k >= k) { best = x; x = x->l; } else x = x->r;
    }
    return best;
}
static N *upper_bound(Tree *T, int k) {
    N *x = T->root, *best = NULL;
    while (x) {
        if (x->k > k) { best = x; x = x->l; } else x = x->r;
    }
    return best;
}
static int insert(Tree *T, int k) {
    N *par = NULL, *x = T->root;
    while (x) {
        if (k == x->k) return 0;
        par = x;
        x = k < x->k ? x->l : x->r;
    }
    x = xmalloc(sizeof *x);
    x->k = k; x->l = x->r = NULL; x->p = par;
    if (!par) T->root = x;
    else if (k < par->k) par->l = x;
    else par->r = x;
    T->size++;
    return 1;
}
static void transplant(Tree *T, N *u, N *v) {
    if (!u->p) T->root = v;
    else if (u == u->p->l) u->p->l = v;
    else u->p->r = v;
    if (v) v->p = u->p;
}
/* erase by relinking nodes (never copying keys), so other iterators stay valid; returns next iterator */
static N *erase(Tree *T, N *z) {
    N *nx = next(z);
    if (!z->l) transplant(T, z, z->r);
    else if (!z->r) transplant(T, z, z->l);
    else {
        N *y = nx;             /* successor lives in the right subtree */
        if (y->p != z) {
            transplant(T, y, y->r);
            y->r = z->r; y->r->p = y;
        }
        transplant(T, z, y);
        y->l = z->l; y->l->p = y;
    }
    free(z);
    T->size--;
    return nx;
}
static N *find(Tree *T, int k) {
    N *x = T->root;
    while (x && x->k != k) x = k < x->k ? x->l : x->r;
    return x;
}
static int distance(N *a, N *b) {
    int d = 0;
    while (a != b) { a = next(a); d++; }
    return d;
}
static N *advance(N *x, int n) {
    while (n-- > 0 && x) x = next(x);
    return x;
}
static int verify_links(N *t, N *par, long lo, long hi) {
    if (!t) return 0;
    check(t->p == par, "parent link");
    check(t->k > lo && t->k < hi, "order");
    return 1 + verify_links(t->l, t, lo, t->k) + verify_links(t->r, t, t->k, hi);
}
static void verify(Tree *T) {
    check(verify_links(T->root, NULL, -1, 1000000) == T->size, "size");
    check(T->size == mn, "size equals model");
    int n = 0;
    for (N *x = first(T->root); x; x = next(x)) { check(n < mn, "overrun"); outk[n++] = x->k; }
    check(n == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "forward iteration");
    n = mn;
    for (N *x = last(T->root); x; x = prev(x)) check(n > 0 && x->k == mod[--n], "backward iteration");
    check(n == 0, "backward covers all");
}
static void freeall(Tree *T) {
    N *x = first(T->root);
    while (x) x = erase(T, x);
}

int main(void) {
    Tree T = {NULL, 0};
    long steps_checked = 0;
    int ins_n = 0, del_n = 0;
    for (int op = 0; op < 3000; op++) {
        int k = (int)(rnd() % 400);
        unsigned c = rnd() % 10;
        if (c < 5) { int a = insert(&T, k); check(a == minsert(k), "ins"); ins_n += a; }
        else if (c < 8) {
            N *x = find(&T, k);
            check((x != NULL) == mhas(k), "find");
            if (x) {
                N *nx = erase(&T, x);
                mdelete(k); del_n++;
                int p = mfind(k);
                check((nx == NULL) == (p >= mn) && (!nx || nx->k == mod[p]), "erase returns next");
            }
        } else {
            N *lb = lower_bound(&T, k), *ub = upper_bound(&T, k);
            int p = mfind(k);
            check((lb == NULL) == (p >= mn) && (!lb || lb->k == mod[p]), "lower_bound");
            int q = p + (p < mn && mod[p] == k);
            check((ub == NULL) == (q >= mn) && (!ub || ub->k == mod[q]), "upper_bound");
            if (lb) {
                int d = distance(lb, ub ? ub : NULL);
                (void)d;
            }
            /* distance and advance agree with index arithmetic */
            N *f = first(T.root);
            if (f && mn > 0) {
                int i = (int)(rnd() % (unsigned)mn);
                N *y = advance(f, i);
                check(y && y->k == mod[i], "advance");
                if (lb) check(distance(f, lb) == p, "distance to lower_bound");
                steps_checked += i;
            }
        }
        if (op % 5 == 0) verify(&T);
    }
    verify(&T);
    printf("inserts=%d erases=%d size=%d\n", ins_n, del_n, mn);
    printf("iterator steps validated=%ld\n", steps_checked);
    /* erase while iterating: drop every key divisible by 3, the iterator survives */
    int erased = 0;
    for (N *x = first(T.root); x;) {
        if (x->k % 3 == 0) {
            int key = x->k;
            x = erase(&T, x);
            mdelete(key); erased++;
        } else x = next(x);
    }
    verify(&T);
    printf("erase-while-iterating removed %d keys, %d remain\n", erased, mn);
    /* range erase [100, 200) */
    int before = mn, removed = 0;
    N *lo = lower_bound(&T, 100), *hi = lower_bound(&T, 200);
    int width = lo ? distance(lo, hi) : 0;
    while (lo != hi) { int key = lo->k; lo = erase(&T, lo); mdelete(key); removed++; }
    check(removed == width, "range erase width");
    verify(&T);
    printf("range erase [100,200): %d of %d keys removed\n", removed, before);
    /* reverse iteration sample */
    printf("last five keys:");
    N *x = last(T.root);
    for (int i = 0; i < 5 && x; i++, x = prev(x)) printf(" %d", x->k);
    printf("\n");
    freeall(&T);
    return 0;
}
