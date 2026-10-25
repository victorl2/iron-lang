/*
 * title: Merging binary search trees three ways
 * topic: data_structures
 * covers: tree merging, repeated insertion, inorder flatten and sorted merge, balanced rebuild from array, right-vine list merge with node reuse, duplicate handling, comparison counts
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
int mod[MAXN], mn;
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
    struct N *l, *r;
} N;

static long cmps;
static N *mk(int k) {
    N *n = xmalloc(sizeof *n);
    n->k = k; n->l = n->r = NULL;
    return n;
}
static N *insert(N *t, int k) {
    N **p = &t;
    while (*p) {
        cmps++;
        if (k == (*p)->k) return t;
        p = k < (*p)->k ? &(*p)->l : &(*p)->r;
    }
    *p = mk(k);
    return t;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int flatten(N *t, int *a, int n) {
    if (!t) return n;
    n = flatten(t->l, a, n);
    a[n++] = t->k;
    return flatten(t->r, a, n);
}
static N *from_sorted(const int *a, int lo, int hi) {
    if (lo >= hi) return NULL;
    int m = (lo + hi) / 2;
    N *t = mk(a[m]);
    t->l = from_sorted(a, lo, m);
    t->r = from_sorted(a, m + 1, hi);
    return t;
}
static int merge_arrays(const int *a, int na, const int *b, int nb, int *out) {
    int i = 0, j = 0, n = 0;
    while (i < na || j < nb) {
        if (i < na && j < nb) {
            cmps++;
            if (a[i] < b[j]) out[n++] = a[i++];
            else if (b[j] < a[i]) out[n++] = b[j++];
            else { out[n++] = a[i]; i++; j++; }
        } else if (i < na) out[n++] = a[i++];
        else out[n++] = b[j++];
    }
    return n;
}
/* method 3: convert to right-leaning sorted lists in place (right vine), merge lists, rebuild */
static N *to_vine(N *t, N *tail) {
    if (!t) return tail;
    N *l = t->l, *r = t->r;
    t->l = NULL;
    t->r = to_vine(r, tail);
    return to_vine(l, t);
}
static N *merge_vines(N *a, N *b, int *freed) {
    N head, *tail = &head;
    head.r = NULL;
    while (a && b) {
        cmps++;
        if (a->k < b->k) { tail->r = a; a = a->r; tail = tail->r; }
        else if (b->k < a->k) { tail->r = b; b = b->r; tail = tail->r; }
        else {
            N *dup = b;
            b = b->r;
            free(dup);
            (*freed)++;
        }
    }
    tail->r = a ? a : b;
    return head.r;
}
static N *vine_to_tree(N **list, int n) {
    if (n == 0) return NULL;
    int left = (n - 1) / 2;
    N *l = vine_to_tree(list, left);
    N *root = *list;
    *list = root->r;
    root->l = l;
    root->r = vine_to_tree(list, n - 1 - left);
    return root;
}
static int list_len(N *v) { int n = 0; while (v) { n++; v = v->r; } return n; }
static int verify_sorted(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    return 1 + verify_sorted(t->l, lo, t->k) + verify_sorted(t->r, t->k, hi);
}
static N *random_bst(int n, int range) {
    N *t = NULL;
    for (int i = 0; i < n; i++) t = insert(t, (int)(rnd() % (unsigned)range));
    return t;
}
static N *copy(N *t) {
    if (!t) return NULL;
    N *c = mk(t->k);
    c->l = copy(t->l); c->r = copy(t->r);
    return c;
}

int main(void) {
    static int A[MAXN], B[MAXN], M[MAXN * 2], R1[MAXN * 2], R2[MAXN * 2], R3[MAXN * 2];
    static const int sizes[6][2] = {{300, 10}, {10, 300}, {200, 200}, {0, 150}, {150, 0}, {64, 64}};
    for (int s = 0; s < 6; s++) {
        int na = sizes[s][0], nb = sizes[s][1];
        cmps = 0;
        N *ta = random_bst(na, 600), *tb = random_bst(nb, 600);
        cmps = 0;
        int la = flatten(ta, A, 0), lb = flatten(tb, B, 0);
        int lm = merge_arrays(A, la, B, lb, M);
        /* method 1: insert every key of b into a copy of a */
        N *m1 = copy(ta);
        long c1;
        cmps = 0;
        for (int i = 0; i < lb; i++) m1 = insert(m1, B[i]);
        c1 = cmps;
        int n1 = flatten(m1, R1, 0);
        check(n1 == lm && !memcmp(R1, M, sizeof(int) * (size_t)lm), "insertion merge");
        /* method 2: flatten both, merge arrays, rebuild balanced */
        cmps = 0;
        int a2 = flatten(ta, A, 0), b2 = flatten(tb, B, 0);
        int m2n = merge_arrays(A, a2, B, b2, M);
        N *m2 = from_sorted(M, 0, m2n);
        long c2 = cmps;
        int n2 = flatten(m2, R2, 0);
        check(n2 == lm && !memcmp(R2, R1, sizeof(int) * (size_t)lm), "array merge");
        /* method 3: vines, destructive (reuses nodes of both trees) */
        cmps = 0;
        int freed = 0;
        N *va = to_vine(ta, NULL), *vb = to_vine(tb, NULL);
        N *vm = merge_vines(va, vb, &freed);
        int len = list_len(vm);
        N *cursor = vm;
        N *m3 = vine_to_tree(&cursor, len);
        long c3 = cmps;
        check(cursor == NULL, "list fully consumed");
        int n3 = flatten(m3, R3, 0);
        check(n3 == lm && !memcmp(R3, R1, sizeof(int) * (size_t)lm), "vine merge");
        check(verify_sorted(m1, -1, 100000) == lm && verify_sorted(m2, -1, 100000) == lm && verify_sorted(m3, -1, 100000) == lm, "all valid");
        check(len + freed == la + lb, "node accounting");
        printf("|A|=%d |B|=%d merged=%d dups=%d | height insert=%d array=%d vine=%d | cmps insert=%ld array=%ld vine=%ld\n",
               la, lb, lm, freed, height(m1), height(m2), height(m3), c1, c2, c3);
        freet(m1); freet(m2); freet(m3);
    }
    return 0;
}
