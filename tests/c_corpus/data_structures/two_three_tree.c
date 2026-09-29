/*
 * title: 2-3 tree with insert splits and delete holes
 * topic: data_structures
 * covers: 2-3 tree, 2-nodes and 3-nodes, node splitting, borrow and merge on delete, uniform leaf depth, sorted-array model
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
    int n;              /* 1 or 2 keys (0 only transiently: a hole) */
    int k[2];
    struct N *c[3];
} N;

static long splits, borrows, merges;
static N *mknode(void) {
    N *x = xmalloc(sizeof *x);
    x->n = 0; x->k[0] = x->k[1] = 0;
    x->c[0] = x->c[1] = x->c[2] = NULL;
    return x;
}
/* insert; returns 1 if x split: *up promoted, *right the new right sibling */
static int ins(N *x, int k, int *added, int *up, N **right) {
    int kk[3], i;
    N *cc[4] = {NULL, NULL, NULL, NULL};
    if (k == x->k[0] || (x->n == 2 && k == x->k[1])) return 0;
    i = 0;
    while (i < x->n && x->k[i] < k) i++;
    for (int j = 0; j < x->n; j++) kk[j] = x->k[j];
    for (int j = 0; j <= x->n; j++) cc[j] = x->c[j];
    if (x->c[0]) {
        int u; N *r = NULL;
        if (!ins(x->c[i], k, added, &u, &r)) return 0;
        for (int j = x->n; j > i; j--) { kk[j] = kk[j - 1]; }
        for (int j = x->n + 1; j > i + 1; j--) { cc[j] = cc[j - 1]; }
        kk[i] = u; cc[i + 1] = r;
    } else {
        for (int j = x->n; j > i; j--) kk[j] = kk[j - 1];
        kk[i] = k;
        *added = 1;
    }
    if (x->n == 1) {
        x->n = 2;
        x->k[0] = kk[0]; x->k[1] = kk[1];
        for (int j = 0; j < 3; j++) x->c[j] = cc[j];
        return 0;
    }
    /* overflow: 3 keys, 4 children -> split */
    N *r = mknode();
    x->n = 1; x->k[0] = kk[0];
    x->c[0] = cc[0]; x->c[1] = cc[1]; x->c[2] = NULL;
    r->n = 1; r->k[0] = kk[2];
    r->c[0] = cc[2]; r->c[1] = cc[3];
    *up = kk[1]; *right = r;
    splits++;
    return 1;
}
static int insert(N **root, int k) {
    int added = 0, up;
    N *r = NULL;
    if (!*root) {
        *root = mknode();
        (*root)->n = 1; (*root)->k[0] = k;
        return 1;
    }
    if (ins(*root, k, &added, &up, &r)) {
        N *nr = mknode();
        nr->n = 1; nr->k[0] = up; nr->c[0] = *root; nr->c[1] = r;
        *root = nr;
    }
    return added;
}
/* child i of p is a hole (n == 0, its only subtree in c[0]) */
static void fix(N *p, int i) {
    N *h = p->c[i];
    if (i < p->n && p->c[i + 1]->n == 2) {           /* borrow from right */
        N *s = p->c[i + 1];
        h->n = 1; h->k[0] = p->k[i];
        h->c[1] = s->c[0];
        p->k[i] = s->k[0];
        s->k[0] = s->k[1];
        s->c[0] = s->c[1]; s->c[1] = s->c[2]; s->c[2] = NULL;
        s->n = 1;
        borrows++;
    } else if (i > 0 && p->c[i - 1]->n == 2) {       /* borrow from left */
        N *s = p->c[i - 1];
        h->n = 1; h->k[0] = p->k[i - 1];
        h->c[1] = h->c[0];
        h->c[0] = s->c[2];
        p->k[i - 1] = s->k[1];
        s->c[2] = NULL;
        s->n = 1;
        borrows++;
    } else if (i < p->n) {                           /* merge with right */
        N *s = p->c[i + 1];
        s->k[1] = s->k[0];
        s->k[0] = p->k[i];
        s->c[2] = s->c[1]; s->c[1] = s->c[0]; s->c[0] = h->c[0];
        s->n = 2;
        free(h);
        for (int j = i; j < p->n - 1; j++) p->k[j] = p->k[j + 1];
        for (int j = i; j < p->n; j++) p->c[j] = p->c[j + 1];
        p->c[p->n] = NULL;
        p->n--;
        merges++;
    } else {                                         /* merge with left */
        N *s = p->c[i - 1];
        s->k[1] = p->k[i - 1];
        s->c[2] = h->c[0];
        s->n = 2;
        free(h);
        p->c[i] = NULL;
        p->n--;
        merges++;
    }
}
static int del(N *x, int k) {
    int i = 0, found = 0;
    while (i < x->n && x->k[i] < k) i++;
    if (i < x->n && x->k[i] == k) found = 1;
    if (!x->c[0]) {
        if (!found) return 0;
        for (int j = i; j < x->n - 1; j++) x->k[j] = x->k[j + 1];
        x->n--;
        return 1;
    }
    int r;
    if (found) {
        N *m = x->c[i];
        while (m->c[0]) m = m->c[m->n];
        int pred = m->k[m->n - 1];
        x->k[i] = pred;
        r = del(x->c[i], pred);
    } else r = del(x->c[i], k);
    if (x->c[i]->n == 0) fix(x, i);
    return r;
}
static int delete(N **root, int k) {
    if (!*root) return 0;
    int r = del(*root, k);
    if ((*root)->n == 0) {
        N *old = *root;
        *root = old->c[0];
        free(old);
    }
    return r;
}
static int depth_check(N *x, long lo, long hi, int *cnt) {
    if (!x) return 0;
    check(x->n == 1 || x->n == 2, "2-3 node key count");
    if (x->n == 2) check(x->k[0] < x->k[1], "keys sorted");
    check(x->k[0] > lo && x->k[x->n - 1] < hi, "bounds");
    *cnt += x->n;
    int d = -1;
    for (int i = 0; i <= x->n; i++) {
        long l2 = i == 0 ? lo : x->k[i - 1];
        long h2 = i == x->n ? hi : x->k[i];
        int cd = depth_check(x->c[i], l2, h2, cnt);
        if (d < 0) d = cd;
        check(cd == d, "uniform leaf depth");
    }
    for (int i = x->n + 1; i < 3; i++) check(x->c[i] == NULL, "unused child slot");
    return d + 1;
}
static void inorder(N *x) {
    if (!x) return;
    for (int i = 0; i < x->n; i++) { inorder(x->c[i]); outk[no++] = x->k[i]; }
    inorder(x->c[x->n]);
}
static int has(N *x, int k) {
    while (x) {
        int i = 0;
        while (i < x->n && x->k[i] < k) i++;
        if (i < x->n && x->k[i] == k) return 1;
        x = x->c[i];
    }
    return 0;
}
static void verify(N *root, int *depth) {
    int cnt = 0;
    *depth = depth_check(root, -1, 1000000, &cnt);
    check(cnt == mn, "key count");
    no = 0; inorder(root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
}
static void freet(N *x) {
    if (!x) return;
    for (int i = 0; i <= x->n; i++) freet(x->c[i]);
    free(x);
}

int main(void) {
    N *root = NULL;
    int d, ins_n = 0, del_n = 0, maxd = 0;
    for (int i = 0; i < 200; i++) { insert(&root, i); minsert(i); verify(root, &d); }
    printf("ascending 200: depth=%d splits=%ld\n", d, splits);
    for (int i = 199; i >= 100; i--) {
        check(delete(&root, i) == 1, "delete present"); mdelete(i); verify(root, &d);
    }
    printf("deleted 100 from the top: size=%d depth=%d borrows=%ld merges=%ld\n", mn, d, borrows, merges);
    for (int op = 0; op < 5000; op++) {
        int k = (int)(rnd() % 400);
        if (rnd() % 2) {
            int a = insert(&root, k);
            check(a == minsert(k), "insert result"); ins_n += a;
        } else {
            int a = delete(&root, k);
            check(a == mdelete(k), "delete result"); del_n += a;
        }
        verify(root, &d);
        check(has(root, k) == mhas(k), "search");
        if (d > maxd) maxd = d;
    }
    printf("random: inserts=%d deletes=%d size=%d max depth=%d\n", ins_n, del_n, mn, maxd);
    printf("splits=%ld borrows=%ld merges=%ld\n", splits, borrows, merges);
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        check(delete(&root, k), "drain"); mdelete(k); verify(root, &d);
    }
    check(root == NULL, "empty");
    printf("drained\n");
    freet(root);
    return 0;
}
