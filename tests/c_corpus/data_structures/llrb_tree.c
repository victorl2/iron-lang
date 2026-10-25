/*
 * title: Left-leaning red-black tree with delete
 * topic: data_structures
 * covers: left-leaning red-black tree, 2-3 tree correspondence, moveRedLeft/moveRedRight, deleteMin, color flips, invariant checker, sorted-array model
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
    int k, red;
    struct N *l, *r;
} N;

static long flips, rots;
static int isred(N *h) { return h && h->red; }
static N *rotl(N *h) {
    N *x = h->r;
    h->r = x->l; x->l = h;
    x->red = h->red; h->red = 1;
    rots++;
    return x;
}
static N *rotr(N *h) {
    N *x = h->l;
    h->l = x->r; x->r = h;
    x->red = h->red; h->red = 1;
    rots++;
    return x;
}
static void flip(N *h) {
    h->red = !h->red; h->l->red = !h->l->red; h->r->red = !h->r->red;
    flips++;
}
static N *fixup(N *h) {
    if (isred(h->r) && !isred(h->l)) h = rotl(h);
    if (isred(h->l) && isred(h->l->l)) h = rotr(h);
    if (isred(h->l) && isred(h->r)) flip(h);
    return h;
}
static N *ins(N *h, int k, int *added) {
    if (!h) {
        N *n = xmalloc(sizeof *n);
        n->k = k; n->red = 1; n->l = n->r = NULL;
        *added = 1;
        return n;
    }
    if (k < h->k) h->l = ins(h->l, k, added);
    else if (k > h->k) h->r = ins(h->r, k, added);
    return fixup(h);
}
static N *move_red_left(N *h) {
    flip(h);
    if (isred(h->r->l)) { h->r = rotr(h->r); h = rotl(h); flip(h); }
    return h;
}
static N *move_red_right(N *h) {
    flip(h);
    if (isred(h->l->l)) { h = rotr(h); flip(h); }
    return h;
}
static N *del_min(N *h) {
    if (!h->l) { free(h); return NULL; }
    if (!isred(h->l) && !isred(h->l->l)) h = move_red_left(h);
    h->l = del_min(h->l);
    return fixup(h);
}
static N *del(N *h, int k) {
    if (k < h->k) {
        if (!isred(h->l) && !isred(h->l->l)) h = move_red_left(h);
        h->l = del(h->l, k);
    } else {
        if (isred(h->l)) h = rotr(h);
        if (k == h->k && !h->r) { free(h); return NULL; }
        if (!isred(h->r) && !isred(h->r->l)) h = move_red_right(h);
        if (k == h->k) {
            N *m = h->r;
            while (m->l) m = m->l;
            h->k = m->k;
            h->r = del_min(h->r);
        } else h->r = del(h->r, k);
    }
    return fixup(h);
}
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
static int insert(N **root, int k) {
    int added = 0;
    *root = ins(*root, k, &added);
    (*root)->red = 0;
    return added;
}
static int delete(N **root, int k) {
    if (!has(*root, k)) return 0;
    if (!isred((*root)->l) && !isred((*root)->r)) (*root)->red = 1;
    *root = del(*root, k);
    if (*root) (*root)->red = 0;
    return 1;
}
/* returns black height; checks 2-3 shape */
static int verify(N *t, long lo, long hi, int *cnt) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    check(!isred(t->r), "right-leaning red link");
    if (t->red) check(!isred(t->l), "two reds in a row");
    (*cnt)++;
    int a = verify(t->l, lo, t->k, cnt);
    int b = verify(t->r, t->k, hi, cnt);
    check(a == b, "black balance");
    return a + !t->red;
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }
static void verify_all(N *root) {
    int cnt = 0;
    if (root) check(!root->red, "root black");
    verify(root, -1, 1000000, &cnt);
    check(cnt == mn, "size");
    no = 0; inorder(root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
}

int main(void) {
    N *root = NULL;
    int ins_n = 0, del_n = 0, maxh = 0;
    for (int i = 0; i < 400; i++) { insert(&root, i); minsert(i); verify_all(root); }
    printf("ascending 400: height=%d rotations=%ld flips=%ld\n", height(root), rots, flips);
    for (int i = 399; i >= 200; i--) {
        check(delete(&root, i) == 1, "delete present"); mdelete(i); verify_all(root);
    }
    printf("after deleting top half: size=%d height=%d\n", mn, height(root));
    for (int op = 0; op < 4000; op++) {
        int k = (int)(rnd() % 500);
        if (rnd() % 2) {
            int a = insert(&root, k);
            check(a == minsert(k), "insert result"); ins_n += a;
        } else {
            int a = delete(&root, k);
            check(a == mdelete(k), "delete result"); del_n += a;
        }
        verify_all(root);
        check(has(root, k) == mhas(k), "search");
        int h = height(root);
        if (h > maxh) maxh = h;
    }
    int lg = 0;
    for (int x = mn + 1; x > 0; x >>= 1) lg++;
    check(maxh <= 2 * lg + 2, "height bound 2 log n");
    printf("random: inserts=%d deletes=%d size=%d maxheight=%d\n", ins_n, del_n, mn, maxh);
    printf("rotations=%ld flips=%ld\n", rots, flips);
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        check(delete(&root, k), "drain"); mdelete(k); verify_all(root);
    }
    check(root == NULL, "empty");
    printf("drained\n");
    freet(root);
    return 0;
}
