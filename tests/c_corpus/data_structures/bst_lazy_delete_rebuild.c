/*
 * title: BST with tombstone deletion and threshold rebuilds
 * topic: data_structures
 * covers: lazy deletion, tombstones, revive on reinsert, rebuild when dead nodes dominate, depth-triggered rebuild, balanced rebuild from live keys, amortized cost accounting, sorted-array model
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
    int k, dead;
    struct N *l, *r;
} N;

typedef struct {
    N *root;
    int live, dead;
    long rebuilds_dead, rebuilds_depth, revived, nodes_moved;
} Tree;

static N *buf[MAXN];
static int bn;
static void flatten_live(N *t) {
    if (!t) return;
    N *r = t->r;
    flatten_live(t->l);
    if (t->dead) free(t);
    else buf[bn++] = t;
    flatten_live(r);
}
static N *build(int lo, int hi) {
    if (lo >= hi) return NULL;
    int m = (lo + hi) / 2;
    N *t = buf[m];
    t->l = build(lo, m);
    t->r = build(m + 1, hi);
    return t;
}
static void rebuild(Tree *T) {
    bn = 0;
    flatten_live(T->root);      /* frees tombstones, keeps live nodes */
    T->nodes_moved += bn;
    T->root = build(0, bn);
    T->dead = 0;
}
static int lg(int n) { int h = 0; while (n > 0) { h++; n >>= 1; } return h; }
static int insert(Tree *T, int k) {
    N **p = &T->root;
    int depth = 0;
    while (*p) {
        if (k == (*p)->k) {
            if (!(*p)->dead) return 0;
            (*p)->dead = 0;
            T->live++; T->dead--; T->revived++;
            return 1;
        }
        p = k < (*p)->k ? &(*p)->l : &(*p)->r;
        depth++;
    }
    N *n = xmalloc(sizeof *n);
    n->k = k; n->dead = 0; n->l = n->r = NULL;
    *p = n;
    T->live++;
    if (depth > 2 * lg(T->live + T->dead) + 4) { rebuild(T); T->rebuilds_depth++; }
    return 1;
}
static int delete(Tree *T, int k) {
    N *x = T->root;
    while (x && x->k != k) x = k < x->k ? x->l : x->r;
    if (!x || x->dead) return 0;
    x->dead = 1;
    T->live--; T->dead++;
    if (T->dead > T->live + 8) { rebuild(T); T->rebuilds_dead++; }
    return 1;
}
static int has(Tree *T, int k) {
    N *x = T->root;
    while (x && x->k != k) x = k < x->k ? x->l : x->r;
    return x && !x->dead;
}
static int verify_rec(N *t, long lo, long hi, int *live, int *dead) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    if (t->dead) (*dead)++; else (*live)++;
    int a = verify_rec(t->l, lo, t->k, live, dead);
    int b = verify_rec(t->r, t->k, hi, live, dead);
    return 1 + (a > b ? a : b);
}
static void inorder_live(N *t) {
    if (!t) return;
    inorder_live(t->l);
    if (!t->dead) outk[no++] = t->k;
    inorder_live(t->r);
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    Tree T = {NULL, 0, 0, 0, 0, 0, 0};
    int ins_n = 0, del_n = 0, maxh = 0, maxdead = 0;
    /* phase 1: sorted inserts trigger depth rebuilds */
    for (int i = 0; i < 400; i++) { insert(&T, i); minsert(i); }
    int live = 0, dead = 0;
    int h = verify_rec(T.root, -1, 100000, &live, &dead);
    printf("after 400 ascending inserts: height=%d depth rebuilds=%ld\n", h, T.rebuilds_depth);
    /* phase 2: mixed workload */
    for (int op = 0; op < 6000; op++) {
        int k = (int)(rnd() % 700);
        if (rnd() % 100 < 45) { int a = insert(&T, k); check(a == minsert(k), "insert result"); ins_n += a; }
        else { int a = delete(&T, k); check(a == mdelete(k), "delete result"); del_n += a; }
        check(has(&T, k) == mhas(k), "search");
        live = dead = 0;
        h = verify_rec(T.root, -1, 100000, &live, &dead);
        check(live == T.live && dead == T.dead && live == mn, "counters");
        check(T.dead <= T.live + 9, "tombstones bounded");
        if (h > maxh) maxh = h;
        if (T.dead > maxdead) maxdead = T.dead;
        if (op % 20 == 0) {
            no = 0; inorder_live(T.root);
            check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "live keys equal model");
        }
    }
    printf("mixed: inserts=%d deletes=%d live=%d dead=%d\n", ins_n, del_n, T.live, T.dead);
    printf("max height=%d max tombstones=%d revived=%ld\n", maxh, maxdead, T.revived);
    printf("rebuilds: by tombstones=%ld, by depth=%ld, nodes moved=%ld\n", T.rebuilds_dead, T.rebuilds_depth, T.nodes_moved);
    /* phase 3: delete everything */
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        check(delete(&T, k), "drain"); mdelete(k);
        check(T.live == mn, "live count");
    }
    printf("drained: live=%d dead=%d\n", T.live, T.dead);
    freet(T.root);
    return 0;
}
