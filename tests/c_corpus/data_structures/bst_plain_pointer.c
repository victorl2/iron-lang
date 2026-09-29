/*
 * title: Unbalanced binary search tree with full invariant checks
 * topic: data_structures
 * covers: binary search tree, insert, delete with successor, search, min/max, floor/ceil, pointer-to-pointer links, bounds invariant, sorted-array model
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 1024

typedef struct N {
    int k;
    struct N *l, *r;
} N;

static unsigned long long rs = 88172645463325252ULL;
static unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
static void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}

static int mod[MAXN], mn;
static int mfind(int k) {
    int lo = 0, hi = mn;
    while (lo < hi) { int m = (lo + hi) / 2; if (mod[m] < k) lo = m + 1; else hi = m; }
    return lo;
}
static int minsert(int k) {
    int p = mfind(k);
    if (p < mn && mod[p] == k) return 0;
    memmove(mod + p + 1, mod + p, (size_t)(mn - p) * sizeof(int));
    mod[p] = k; mn++;
    return 1;
}
static int mdelete(int k) {
    int p = mfind(k);
    if (p >= mn || mod[p] != k) return 0;
    memmove(mod + p, mod + p + 1, (size_t)(mn - p - 1) * sizeof(int));
    mn--;
    return 1;
}

static int insert(N **root, int k) {
    N **p = root;
    while (*p) {
        if (k == (*p)->k) return 0;
        p = k < (*p)->k ? &(*p)->l : &(*p)->r;
    }
    N *n = malloc(sizeof *n);
    if (!n) exit(2);
    n->k = k; n->l = n->r = NULL;
    *p = n;
    return 1;
}
static N *search(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t;
}
static int delete(N **root, int k) {
    N **p = root;
    while (*p && (*p)->k != k) p = k < (*p)->k ? &(*p)->l : &(*p)->r;
    if (!*p) return 0;
    N *d = *p;
    if (!d->l) *p = d->r;
    else if (!d->r) *p = d->l;
    else {
        N **sp = &d->r;
        while ((*sp)->l) sp = &(*sp)->l;
        N *s = *sp;
        *sp = s->r;
        s->l = d->l; s->r = d->r;
        *p = s;
    }
    free(d);
    return 1;
}
static int floor_key(N *t, int k, int *out) {
    int found = 0;
    while (t) {
        if (t->k == k) { *out = k; return 1; }
        if (t->k < k) { *out = t->k; found = 1; t = t->r; } else t = t->l;
    }
    return found;
}
static int ceil_key(N *t, int k, int *out) {
    int found = 0;
    while (t) {
        if (t->k == k) { *out = k; return 1; }
        if (t->k > k) { *out = t->k; found = 1; t = t->l; } else t = t->r;
    }
    return found;
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "bounds");
    return 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
}
static int no;
static int outk[MAXN];
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

static void verify_all(N *root) {
    check(verify(root, -1000000, 1000000) == mn, "size");
    no = 0; inorder(root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder equals model");
}

int main(void) {
    N *root = NULL;
    int ins = 0, del = 0, hits = 0, maxh = 0;
    for (int op = 0; op < 3000; op++) {
        int k = (int)(rnd() % 400);
        unsigned c = rnd() % 10;
        if (c < 5) {
            int a = insert(&root, k), b = minsert(k);
            check(a == b, "insert result");
            ins += a;
        } else if (c < 8) {
            int a = delete(&root, k), b = mdelete(k);
            check(a == b, "delete result");
            del += a;
        } else {
            int a = search(root, k) != NULL;
            int p = mfind(k);
            check(a == (p < mn && mod[p] == k), "search");
            hits += a;
        }
        int lo, hi;
        int f = floor_key(root, k, &lo), ce = ceil_key(root, k, &hi);
        int p = mfind(k);
        int mf = (p < mn && mod[p] == k) ? 1 : p > 0;
        int mfv = (p < mn && mod[p] == k) ? k : (p > 0 ? mod[p - 1] : 0);
        check(f == mf && (!f || lo == mfv), "floor");
        check(ce == (p < mn) && (!ce || hi == mod[p]), "ceil");
        verify_all(root);
        int h = height(root);
        if (h > maxh) maxh = h;
    }
    printf("inserted=%d deleted=%d hits=%d size=%d maxheight=%d finalheight=%d\n",
           ins, del, hits, mn, maxh, height(root));
    printf("min=%d max=%d\n", mod[0], mod[mn - 1]);
    N *m = root;
    while (m->l) m = m->l;
    check(m->k == mod[0], "min");
    /* sorted insertion makes a degenerate chain */
    N *chain = NULL;
    for (int i = 0; i < 100; i++) insert(&chain, i);
    printf("chain height=%d\n", height(chain));
    for (int i = 0; i < 100; i += 2) delete(&chain, i);
    printf("chain after deleting evens: height=%d\n", height(chain));
    freet(chain);
    freet(root);
    return 0;
}
