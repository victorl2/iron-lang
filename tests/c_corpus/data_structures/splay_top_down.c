/*
 * title: Top-down splay tree
 * topic: data_structures
 * covers: splay tree, top-down splaying, header node, split/join delete, sequential access bound, sorted-array model
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
    int k;
    struct N *l, *r;
} N;

static long steps;
static N *splay(N *t, int k) {
    N hdr, *l = &hdr, *r = &hdr, *y;
    if (!t) return NULL;
    hdr.l = hdr.r = NULL;
    for (;;) {
        steps++;
        if (k < t->k) {
            if (!t->l) break;
            if (k < t->l->k) {
                y = t->l; t->l = y->r; y->r = t; t = y;
                if (!t->l) break;
            }
            r->l = t; r = t; t = t->l;
        } else if (k > t->k) {
            if (!t->r) break;
            if (k > t->r->k) {
                y = t->r; t->r = y->l; y->l = t; t = y;
                if (!t->r) break;
            }
            l->r = t; l = t; t = t->r;
        } else break;
    }
    l->r = t->l; r->l = t->r;
    t->l = hdr.r; t->r = hdr.l;
    return t;
}
static N *insert(N *t, int k, int *added) {
    N *n;
    *added = 0;
    if (!t) {
        n = xmalloc(sizeof *n);
        n->k = k; n->l = n->r = NULL; *added = 1;
        return n;
    }
    t = splay(t, k);
    if (k == t->k) return t;
    n = xmalloc(sizeof *n);
    n->k = k; *added = 1;
    if (k < t->k) { n->l = t->l; n->r = t; t->l = NULL; }
    else { n->r = t->r; n->l = t; t->r = NULL; }
    return n;
}
static N *delete(N *t, int k, int *removed) {
    *removed = 0;
    if (!t) return NULL;
    t = splay(t, k);
    if (k != t->k) return t;
    N *x;
    if (!t->l) x = t->r;
    else { x = splay(t->l, k); x->r = t->r; }
    free(t);
    *removed = 1;
    return x;
}
static N *find(N *t, int k, int *found) {
    if (!t) { *found = 0; return NULL; }
    t = splay(t, k);
    *found = t->k == k;
    return t;
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    return 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
}
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    N *root = NULL;
    int flag, ins = 0, del = 0, found_n = 0;
    /* sequential inserts then sequential access: amortized O(1) per access */
    for (int i = 0; i < 300; i++) { root = insert(root, i, &flag); minsert(i); }
    printf("after ascending inserts: root=%d height=%d\n", root->k, height(root));
    steps = 0;
    for (int i = 0; i < 300; i++) {
        root = find(root, i, &flag);
        check(flag && root->k == i, "sequential find");
    }
    printf("sequential scan of 300 keys: steps=%ld\n", steps);
    check(steps < 4 * 300, "sequential access is linear");
    printf("root after scan=%d height=%d\n", root->k, height(root));

    steps = 0;
    for (int op = 0; op < 5000; op++) {
        int k = (int)(rnd() % 500);
        unsigned c = rnd() % 3;
        if (c == 0) {
            root = insert(root, k, &flag);
            check(flag == minsert(k), "insert"); ins += flag;
        } else if (c == 1) {
            root = delete(root, k, &flag);
            check(flag == mdelete(k), "delete"); del += flag;
        } else {
            root = find(root, k, &flag);
            check(flag == mhas(k), "find");
            if (flag) { check(root->k == k, "found key at root"); found_n++; }
        }
        check(verify(root, -1, 100000) == mn, "size");
        if (op % 25 == 0) {
            no = 0; inorder(root);
            check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
        }
    }
    no = 0; inorder(root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "final inorder model");
    printf("random: inserts=%d deletes=%d hits=%d size=%d steps=%ld\n", ins, del, found_n, mn, steps);
    /* working-set behaviour: repeatedly access a few hot keys */
    steps = 0;
    int hot[4] = {mod[3], mod[mn / 2], mod[mn - 4], mod[mn / 4]};
    for (int r = 0; r < 1000; r++) root = find(root, hot[r & 3], &flag);
    printf("1000 accesses over 4 hot keys: steps=%ld\n", steps);
    while (mn) {
        int k = mod[(unsigned)mn / 3];
        root = delete(root, k, &flag);
        check(flag, "drain"); mdelete(k);
        check(verify(root, -1, 100000) == mn, "drain size");
    }
    printf("drained\n");
    freet(root);
    return 0;
}
