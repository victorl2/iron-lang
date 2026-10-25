/*
 * title: WAVL tree with rank-difference rebalancing
 * topic: data_structures
 * covers: WAVL tree, rank differences 1 and 2, promote demote rotate, 2-2 leaf rule, parent pointers, insert-only trees are AVL, invariant checker, sorted-array model
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048

static unsigned long long rs = 88172645463325252ULL;
static inline unsigned rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
static inline void check(int c, const char *w) {
    if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); }
}
static inline void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) exit(2);
    return p;
}
/* reference model: sorted array of unique keys */
static int mod[MAXN], mn;
static inline int mfind(int k) {
    int lo = 0, hi = mn;
    while (lo < hi) { int m = (lo + hi) / 2; if (mod[m] < k) lo = m + 1; else hi = m; }
    return lo;
}
static inline int mhas(int k) { int p = mfind(k); return p < mn && mod[p] == k; }
static inline int minsert(int k) {
    int p = mfind(k);
    if (p < mn && mod[p] == k) return 0;
    memmove(mod + p + 1, mod + p, (size_t)(mn - p) * sizeof(int));
    mod[p] = k; mn++;
    return 1;
}
static inline int mdelete(int k) {
    int p = mfind(k);
    if (p >= mn || mod[p] != k) return 0;
    memmove(mod + p, mod + p + 1, (size_t)(mn - p - 1) * sizeof(int));
    mn--;
    return 1;
}
static int no, outk[MAXN];

typedef struct N {
    int k, rank;
    struct N *l, *r, *p;
} N;

typedef struct { N *root; long promotes, demotes, rots; } Tree;

static int rk(N *x) { return x ? x->rank : -1; }
static void rotate_up(Tree *T, N *x) {
    N *p = x->p, *g = p->p;
    if (x == p->l) {
        p->l = x->r;
        if (x->r) x->r->p = p;
        x->r = p;
    } else {
        p->r = x->l;
        if (x->l) x->l->p = p;
        x->l = p;
    }
    p->p = x; x->p = g;
    if (!g) T->root = x;
    else if (g->l == p) g->l = x;
    else g->r = x;
    T->rots++;
}
static int insert(Tree *T, int k) {
    N *par = NULL, *x = T->root;
    while (x) {
        if (k == x->k) return 0;
        par = x;
        x = k < x->k ? x->l : x->r;
    }
    x = xmalloc(sizeof *x);
    x->k = k; x->rank = 0; x->l = x->r = NULL; x->p = par;
    if (!par) T->root = x;
    else if (k < par->k) par->l = x;
    else par->r = x;
    N *p = par;
    while (p && x->rank == p->rank) {
        N *s = x == p->l ? p->r : p->l;
        if (p->rank - rk(s) == 1) {
            p->rank++; T->promotes++;
            x = p; p = p->p;
        } else {
            if (x == p->l) {
                N *y = x->r;
                if (rk(x) - rk(y) == 2) { rotate_up(T, x); p->rank--; T->demotes++; }
                else { rotate_up(T, y); rotate_up(T, y); y->rank++; x->rank--; p->rank--; T->demotes += 2; T->promotes++; }
            } else {
                N *y = x->l;
                if (rk(x) - rk(y) == 2) { rotate_up(T, x); p->rank--; T->demotes++; }
                else { rotate_up(T, y); rotate_up(T, y); y->rank++; x->rank--; p->rank--; T->demotes += 2; T->promotes++; }
            }
            break;
        }
    }
    return 1;
}
static int delete(Tree *T, int k) {
    N *z = T->root;
    while (z && z->k != k) z = k < z->k ? z->l : z->r;
    if (!z) return 0;
    if (z->l && z->r) {
        N *s = z->r;
        while (s->l) s = s->l;
        z->k = s->k;
        z = s;
    }
    N *child = z->l ? z->l : z->r, *p = z->p;
    if (child) child->p = p;
    if (!p) T->root = child;
    else if (p->l == z) p->l = child;
    else p->r = child;
    free(z);
    N *x = child;
    if (p && !p->l && !p->r && p->rank == 1) {
        p->rank--; T->demotes++;
        x = p; p = p->p;
    }
    while (p && p->rank - rk(x) == 3) {
        int xleft = x ? x == p->l : p->l == NULL;
        N *y = xleft ? p->r : p->l;
        if (p->rank - y->rank == 2) {
            p->rank--; T->demotes++;
            x = p; p = p->p;
            continue;
        }
        N *yo = xleft ? y->r : y->l;   /* outer child of y */
        N *yi = xleft ? y->l : y->r;   /* inner child of y */
        if (y->rank - rk(yi) == 2 && y->rank - rk(yo) == 2) {
            p->rank--; y->rank--; T->demotes += 2;
            x = p; p = p->p;
        } else if (y->rank - rk(yo) == 1) {
            rotate_up(T, y);
            y->rank++; p->rank--;
            T->promotes++; T->demotes++;
            if (!p->l && !p->r) { p->rank--; T->demotes++; }
            break;
        } else {
            rotate_up(T, yi);
            rotate_up(T, yi);
            yi->rank += 2; p->rank -= 2; y->rank--;
            T->promotes += 2; T->demotes += 3;
            break;
        }
    }
    return 1;
}
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int verify(N *t, N *par, long lo, long hi, int avl_only) {
    if (!t) return 0;
    check(t->p == par, "parent link");
    check(t->k > lo && t->k < hi, "order");
    int dl = t->rank - rk(t->l), dr = t->rank - rk(t->r);
    check(dl == 1 || dl == 2, "left rank difference");
    check(dr == 1 || dr == 2, "right rank difference");
    if (!t->l && !t->r) check(t->rank == 0, "leaf rank 0");
    if (avl_only) check(t->rank == height(t) - 1, "insert-only tree is AVL-like");
    return 1 + verify(t->l, t, lo, t->k, avl_only) + verify(t->r, t, t->k, hi, avl_only);
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}

int main(void) {
    Tree T = {NULL, 0, 0, 0};
    int ins_n = 0, del_n = 0, maxh = 0;
    for (int i = 0; i < 300; i++) {
        insert(&T, i); minsert(i);
        check(verify(T.root, NULL, -1, 100000, 1) == mn, "asc");
    }
    printf("ascending 300: height=%d root rank=%d promotes=%ld rotations=%ld\n",
           height(T.root), T.root->rank, T.promotes, T.rots);
    for (int i = 0; i < 300; i++) {
        int k = (int)((unsigned)i * 37u % 300u);
        insert(&T, k + 1000); minsert(k + 1000);
        check(verify(T.root, NULL, -1, 100000, 1) == mn, "insert-only permuted");
    }
    printf("insert-only after 600 keys: height=%d root rank=%d\n", height(T.root), T.root->rank);
    for (int op = 0; op < 6000; op++) {
        int k = (int)(rnd() % 900);
        if (rnd() % 2) { int a = insert(&T, k); check(a == minsert(k), "ins"); ins_n += a; }
        else { int a = delete(&T, k); check(a == mdelete(k), "del"); del_n += a; }
        check(verify(T.root, NULL, -1, 100000, 0) == mn, "size and ranks");
        check(has(T.root, k) == mhas(k), "search");
        if (op % 10 == 0) {
            no = 0; inorder(T.root);
            check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
        }
        int h = height(T.root);
        if (h > maxh) maxh = h;
        if (T.root) check(h <= T.root->rank + 1 + T.root->rank, "height <= 2 * rank + 1");
    }
    printf("random: inserts=%d deletes=%d size=%d maxheight=%d\n", ins_n, del_n, mn, maxh);
    printf("promotes=%ld demotes=%ld rotations=%ld\n", T.promotes, T.demotes, T.rots);
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        check(delete(&T, k), "drain"); mdelete(k);
        check(verify(T.root, NULL, -1, 100000, 0) == mn, "drain size");
    }
    check(T.root == NULL, "empty");
    printf("drained\n");
    freet(T.root);
    return 0;
}
