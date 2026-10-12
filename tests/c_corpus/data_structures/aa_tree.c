/*
 * title: AA tree with skew, split and level invariants
 * topic: data_structures
 * covers: AA tree, skew and split, level field, decrease-level on delete, 2-3 tree simulation, invariant checker, sorted-array model
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
    int k, level;
    struct N *l, *r;
} N;

static long skews, splits;
static int lvl(N *t) { return t ? t->level : 0; }
static N *skew(N *t) {
    if (t && t->l && t->l->level == t->level) {
        N *l = t->l;
        t->l = l->r; l->r = t;
        skews++;
        return l;
    }
    return t;
}
static N *split(N *t) {
    if (t && t->r && t->r->r && t->r->r->level == t->level) {
        N *r = t->r;
        t->r = r->l; r->l = t;
        r->level++;
        splits++;
        return r;
    }
    return t;
}
static N *ins(N *t, int k, int *added) {
    if (!t) {
        N *n = xmalloc(sizeof *n);
        n->k = k; n->level = 1; n->l = n->r = NULL;
        *added = 1;
        return n;
    }
    if (k < t->k) t->l = ins(t->l, k, added);
    else if (k > t->k) t->r = ins(t->r, k, added);
    else return t;
    t = skew(t);
    return split(t);
}
static void decrease_level(N *t) {
    int a = lvl(t->l), b = lvl(t->r);
    int should = (a < b ? a : b) + 1;
    if (should < t->level) {
        t->level = should;
        if (t->r && should < t->r->level) t->r->level = should;
    }
}
static N *del(N *t, int k, int *removed) {
    if (!t) return NULL;
    if (k < t->k) t->l = del(t->l, k, removed);
    else if (k > t->k) t->r = del(t->r, k, removed);
    else {
        *removed = 1;
        if (!t->l && !t->r) { free(t); return NULL; }
        int dummy = 0;
        if (!t->l) {
            N *s = t->r;
            while (s->l) s = s->l;
            t->k = s->k;
            t->r = del(t->r, s->k, &dummy);
        } else {
            N *p = t->l;
            while (p->r) p = p->r;
            t->k = p->k;
            t->l = del(t->l, p->k, &dummy);
        }
    }
    decrease_level(t);
    t = skew(t);
    t->r = skew(t->r);
    if (t->r) t->r->r = skew(t->r->r);
    t = split(t);
    t->r = split(t->r);
    return t;
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    if (!t->l && !t->r) check(t->level == 1, "leaf level 1");
    check(lvl(t->l) == t->level - 1, "left child level");
    check(lvl(t->r) == t->level || lvl(t->r) == t->level - 1, "right child level");
    if (t->r && t->r->r) check(t->r->r->level < t->level, "no double right horizontal");
    return 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int has(N *t, int k) {
    while (t && t->k != k) t = k < t->k ? t->l : t->r;
    return t != NULL;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    N *root = NULL;
    int added, ins_n = 0, del_n = 0, maxh = 0;
    for (int i = 0; i < 400; i++) {
        added = 0; root = ins(root, i, &added); minsert(i);
        check(verify(root, -1, 100000) == mn, "asc");
    }
    printf("ascending 400: height=%d root level=%d skews=%ld splits=%ld\n", height(root), root->level, skews, splits);
    for (int op = 0; op < 5000; op++) {
        int k = (int)(rnd() % 600);
        added = 0;
        if (rnd() % 2) {
            root = ins(root, k, &added);
            check(added == minsert(k), "insert result"); ins_n += added;
        } else {
            root = del(root, k, &added);
            check(added == mdelete(k), "delete result"); del_n += added;
        }
        check(verify(root, -1, 100000) == mn, "size");
        check(has(root, k) == mhas(k), "search");
        if (op % 10 == 0) {
            no = 0; inorder(root);
            check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
        }
        int h = height(root);
        if (h > maxh) maxh = h;
        if (root) check(h <= 2 * root->level, "height <= 2 * level");
    }
    printf("random: inserts=%d deletes=%d size=%d maxheight=%d root level=%d\n", ins_n, del_n, mn, maxh, lvl(root));
    printf("skews=%ld splits=%ld\n", skews, splits);
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        added = 0; root = del(root, k, &added); check(added, "drain"); mdelete(k);
        check(verify(root, -1, 100000) == mn, "drain size");
    }
    printf("drained\n");
    freet(root);
    return 0;
}
