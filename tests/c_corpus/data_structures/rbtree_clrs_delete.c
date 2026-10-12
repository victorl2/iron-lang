/*
 * title: Red-black tree (CLRS) with sentinel, parent pointers and delete fixup
 * topic: data_structures
 * covers: red-black tree, nil sentinel, left/right rotation, insert fixup, transplant, delete fixup, black-height invariant, sorted-array model
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 2048
enum { RED, BLACK };

typedef struct N {
    int k, color;
    struct N *l, *r, *p;
} N;

static N nil_node = {0, BLACK, &nil_node, &nil_node, &nil_node};
static N *const NIL = &nil_node;

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

typedef struct { N *root; long rotations, fixups; } Tree;

static void left_rotate(Tree *T, N *x) {
    N *y = x->r;
    x->r = y->l;
    if (y->l != NIL) y->l->p = x;
    y->p = x->p;
    if (x->p == NIL) T->root = y;
    else if (x == x->p->l) x->p->l = y;
    else x->p->r = y;
    y->l = x; x->p = y;
    T->rotations++;
}
static void right_rotate(Tree *T, N *x) {
    N *y = x->l;
    x->l = y->r;
    if (y->r != NIL) y->r->p = x;
    y->p = x->p;
    if (x->p == NIL) T->root = y;
    else if (x == x->p->r) x->p->r = y;
    else x->p->l = y;
    y->r = x; x->p = y;
    T->rotations++;
}
static void insert_fixup(Tree *T, N *z) {
    while (z->p->color == RED) {
        T->fixups++;
        if (z->p == z->p->p->l) {
            N *y = z->p->p->r;
            if (y->color == RED) {
                z->p->color = BLACK; y->color = BLACK; z->p->p->color = RED;
                z = z->p->p;
            } else {
                if (z == z->p->r) { z = z->p; left_rotate(T, z); }
                z->p->color = BLACK; z->p->p->color = RED;
                right_rotate(T, z->p->p);
            }
        } else {
            N *y = z->p->p->l;
            if (y->color == RED) {
                z->p->color = BLACK; y->color = BLACK; z->p->p->color = RED;
                z = z->p->p;
            } else {
                if (z == z->p->l) { z = z->p; right_rotate(T, z); }
                z->p->color = BLACK; z->p->p->color = RED;
                left_rotate(T, z->p->p);
            }
        }
    }
    T->root->color = BLACK;
}
static N *find(Tree *T, int k) {
    N *x = T->root;
    while (x != NIL && x->k != k) x = k < x->k ? x->l : x->r;
    return x;
}
static int insert(Tree *T, int k) {
    N *y = NIL, *x = T->root;
    while (x != NIL) {
        y = x;
        if (k == x->k) return 0;
        x = k < x->k ? x->l : x->r;
    }
    N *z = malloc(sizeof *z);
    if (!z) exit(2);
    z->k = k; z->color = RED; z->l = z->r = NIL; z->p = y;
    if (y == NIL) T->root = z;
    else if (k < y->k) y->l = z;
    else y->r = z;
    insert_fixup(T, z);
    return 1;
}
static void transplant(Tree *T, N *u, N *v) {
    if (u->p == NIL) T->root = v;
    else if (u == u->p->l) u->p->l = v;
    else u->p->r = v;
    v->p = u->p;
}
static N *minimum(N *x) { while (x->l != NIL) x = x->l; return x; }
static void delete_fixup(Tree *T, N *x) {
    while (x != T->root && x->color == BLACK) {
        T->fixups++;
        if (x == x->p->l) {
            N *w = x->p->r;
            if (w->color == RED) {
                w->color = BLACK; x->p->color = RED;
                left_rotate(T, x->p);
                w = x->p->r;
            }
            if (w->l->color == BLACK && w->r->color == BLACK) {
                w->color = RED; x = x->p;
            } else {
                if (w->r->color == BLACK) {
                    w->l->color = BLACK; w->color = RED;
                    right_rotate(T, w);
                    w = x->p->r;
                }
                w->color = x->p->color; x->p->color = BLACK; w->r->color = BLACK;
                left_rotate(T, x->p);
                x = T->root;
            }
        } else {
            N *w = x->p->l;
            if (w->color == RED) {
                w->color = BLACK; x->p->color = RED;
                right_rotate(T, x->p);
                w = x->p->l;
            }
            if (w->r->color == BLACK && w->l->color == BLACK) {
                w->color = RED; x = x->p;
            } else {
                if (w->l->color == BLACK) {
                    w->r->color = BLACK; w->color = RED;
                    left_rotate(T, w);
                    w = x->p->l;
                }
                w->color = x->p->color; x->p->color = BLACK; w->l->color = BLACK;
                right_rotate(T, x->p);
                x = T->root;
            }
        }
    }
    x->color = BLACK;
}
static int delete(Tree *T, int k) {
    N *z = find(T, k);
    if (z == NIL) return 0;
    N *y = z, *x;
    int ycolor = y->color;
    if (z->l == NIL) { x = z->r; transplant(T, z, z->r); }
    else if (z->r == NIL) { x = z->l; transplant(T, z, z->l); }
    else {
        y = minimum(z->r);
        ycolor = y->color;
        x = y->r;
        if (y->p == z) x->p = y;
        else { transplant(T, y, y->r); y->r = z->r; y->r->p = y; }
        transplant(T, z, y);
        y->l = z->l; y->l->p = y;
        y->color = z->color;
    }
    free(z);
    if (ycolor == BLACK) delete_fixup(T, x);
    NIL->p = NIL; NIL->l = NIL; NIL->r = NIL; NIL->color = BLACK;
    return 1;
}
static int bh_check(N *t, N *parent, long lo, long hi, int *maxdepth, int depth) {
    if (t == NIL) { if (depth > *maxdepth) *maxdepth = depth; return 1; }
    check(t->p == parent, "parent link");
    check(t->k > lo && t->k < hi, "bst order");
    if (t->color == RED) check(t->l->color == BLACK && t->r->color == BLACK, "red has red child");
    int a = bh_check(t->l, t, lo, t->k, maxdepth, depth + 1);
    int b = bh_check(t->r, t, t->k, hi, maxdepth, depth + 1);
    check(a == b, "black height");
    return a + (t->color == BLACK);
}
static int no, outk[MAXN];
static void inorder(N *t) { if (t == NIL) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static void verify(Tree *T, int *maxdepth) {
    check(T->root->color == BLACK, "root black");
    check(NIL->color == BLACK, "nil black");
    *maxdepth = 0;
    int bh = bh_check(T->root, NIL, -1, 1000000, maxdepth, 0);
    (void)bh;
    no = 0; inorder(T->root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
}
static void freet(N *t) { if (t == NIL) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    Tree T = {NIL, 0, 0};
    int md, ins = 0, del = 0, worst = 0;
    for (int i = 0; i < 300; i++) { ins += insert(&T, i); minsert(i); verify(&T, &md); }
    printf("ascending 300: depth=%d rotations=%ld\n", md, T.rotations);
    for (int op = 0; op < 5000; op++) {
        int k = (int)(rnd() % 500);
        if (rnd() % 100 < 52) { int a = insert(&T, k); check(a == minsert(k), "ins"); ins += a; }
        else { int a = delete(&T, k); check(a == mdelete(k), "del"); del += a; }
        verify(&T, &md);
        if (md > worst) worst = md;
        check((find(&T, k) != NIL) == (mfind(k) < mn && mod[mfind(k)] == k), "find");
    }
    printf("inserts=%d deletes=%d size=%d\n", ins, del, mn);
    printf("worst depth=%d rotations=%ld fixup steps=%ld\n", worst, T.rotations, T.fixups);
    int red = 0;
    /* count red nodes via explicit stack */
    N *st[MAXN]; int sp = 0;
    if (T.root != NIL) st[sp++] = T.root;
    while (sp) {
        N *x = st[--sp];
        red += x->color == RED;
        if (x->l != NIL) st[sp++] = x->l;
        if (x->r != NIL) st[sp++] = x->r;
    }
    printf("red nodes=%d of %d\n", red, mn);
    while (mn) {
        int k = mod[(unsigned)mn * 7u % (unsigned)mn / 2 + (unsigned)mn / 3];
        check(delete(&T, k) == 1, "drain");
        mdelete(k);
        verify(&T, &md);
    }
    check(T.root == NIL, "empty");
    printf("drained\n");
    freet(T.root);
    return 0;
}
