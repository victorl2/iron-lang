/*
 * title: B-tree of configurable degree with bottom-up split and delete
 * topic: data_structures
 * covers: B-tree, minimum degree parameter, overflow split, borrow from siblings, merge, predecessor replacement, root collapse, key-count invariants, sorted-array model
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

#define TM 6

typedef struct N {
    int n, leaf;
    int k[2 * TM];
    struct N *c[2 * TM + 1];
} N;

static int T;
static long splits, borrows, merges;
static N *mknode(int leaf) {
    N *x = xmalloc(sizeof *x);
    memset(x, 0, sizeof *x);
    x->leaf = leaf;
    return x;
}
/* returns 1 if x overflowed and split: *up promoted, *right is the new sibling */
static int ins(N *x, int k, int *added, int *up, N **right) {
    int i = 0;
    while (i < x->n && x->k[i] < k) i++;
    if (i < x->n && x->k[i] == k) return 0;
    if (x->leaf) {
        for (int j = x->n; j > i; j--) x->k[j] = x->k[j - 1];
        x->k[i] = k;
        x->n++;
        *added = 1;
    } else {
        int u; N *r = NULL;
        if (!ins(x->c[i], k, added, &u, &r)) return 0;
        for (int j = x->n; j > i; j--) x->k[j] = x->k[j - 1];
        for (int j = x->n + 1; j > i + 1; j--) x->c[j] = x->c[j - 1];
        x->k[i] = u; x->c[i + 1] = r;
        x->n++;
    }
    if (x->n < 2 * T) return 0;
    /* 2T keys: left keeps T-1, median goes up, right gets T */
    N *r = mknode(x->leaf);
    *up = x->k[T - 1];
    r->n = x->n - T;
    for (int j = 0; j < r->n; j++) r->k[j] = x->k[T + j];
    if (!x->leaf) for (int j = 0; j <= r->n; j++) { r->c[j] = x->c[T + j]; x->c[T + j] = NULL; }
    x->n = T - 1;
    *right = r;
    splits++;
    return 1;
}
static int insert(N **root, int k) {
    int added = 0, up;
    N *r = NULL;
    if (!*root) *root = mknode(1);
    if (ins(*root, k, &added, &up, &r)) {
        N *nr = mknode(0);
        nr->n = 1; nr->k[0] = up; nr->c[0] = *root; nr->c[1] = r;
        *root = nr;
    }
    return added;
}
static void fix(N *x, int i) {
    N *c = x->c[i];
    if (i > 0 && x->c[i - 1]->n > T - 1) {
        N *l = x->c[i - 1];
        for (int j = c->n; j > 0; j--) c->k[j] = c->k[j - 1];
        if (!c->leaf) for (int j = c->n + 1; j > 0; j--) c->c[j] = c->c[j - 1];
        c->k[0] = x->k[i - 1];
        if (!c->leaf) { c->c[0] = l->c[l->n]; l->c[l->n] = NULL; }
        x->k[i - 1] = l->k[l->n - 1];
        l->n--; c->n++;
        borrows++;
    } else if (i < x->n && x->c[i + 1]->n > T - 1) {
        N *r = x->c[i + 1];
        c->k[c->n] = x->k[i];
        if (!c->leaf) c->c[c->n + 1] = r->c[0];
        x->k[i] = r->k[0];
        for (int j = 0; j < r->n - 1; j++) r->k[j] = r->k[j + 1];
        if (!r->leaf) { for (int j = 0; j < r->n; j++) r->c[j] = r->c[j + 1]; r->c[r->n] = NULL; }
        r->n--; c->n++;
        borrows++;
    } else {
        int j = i < x->n ? i : i - 1;
        N *l = x->c[j], *r = x->c[j + 1];
        l->k[l->n] = x->k[j];
        for (int t = 0; t < r->n; t++) l->k[l->n + 1 + t] = r->k[t];
        if (!l->leaf) for (int t = 0; t <= r->n; t++) l->c[l->n + 1 + t] = r->c[t];
        l->n += r->n + 1;
        for (int t = j; t < x->n - 1; t++) x->k[t] = x->k[t + 1];
        for (int t = j + 1; t < x->n; t++) x->c[t] = x->c[t + 1];
        x->c[x->n] = NULL;
        x->n--;
        free(r);
        merges++;
    }
}
static int del(N *x, int k) {
    int i = 0;
    while (i < x->n && x->k[i] < k) i++;
    int found = i < x->n && x->k[i] == k, r;
    if (x->leaf) {
        if (!found) return 0;
        for (int j = i; j < x->n - 1; j++) x->k[j] = x->k[j + 1];
        x->n--;
        return 1;
    }
    if (found) {
        N *m = x->c[i];
        while (!m->leaf) m = m->c[m->n];
        int pred = m->k[m->n - 1];
        x->k[i] = pred;
        r = del(x->c[i], pred);
    } else r = del(x->c[i], k);
    if (x->c[i]->n < T - 1) fix(x, i);
    return r;
}
static int delete(N **root, int k) {
    if (!*root) return 0;
    int r = del(*root, k);
    if ((*root)->n == 0) {
        N *old = *root;
        *root = old->leaf ? NULL : old->c[0];
        free(old);
    }
    return r;
}
static int check_node(N *x, int is_root, long lo, long hi, int *cnt) {
    check(x->n <= 2 * T - 1, "node too full");
    if (!is_root) check(x->n >= T - 1, "node too empty");
    else check(x->n >= 1, "root nonempty");
    for (int i = 1; i < x->n; i++) check(x->k[i - 1] < x->k[i], "sorted");
    check(x->k[0] > lo && x->k[x->n - 1] < hi, "bounds");
    *cnt += x->n;
    if (x->leaf) return 1;
    int d = -1;
    for (int i = 0; i <= x->n; i++) {
        check(x->c[i] != NULL, "child present");
        long l2 = i == 0 ? lo : x->k[i - 1];
        long h2 = i == x->n ? hi : x->k[i];
        int cd = check_node(x->c[i], 0, l2, h2, cnt);
        if (d < 0) d = cd;
        check(cd == d, "uniform depth");
    }
    return d + 1;
}
static void inorder(N *x) {
    if (!x) return;
    for (int i = 0; i < x->n; i++) {
        if (!x->leaf) inorder(x->c[i]);
        outk[no++] = x->k[i];
    }
    if (!x->leaf) inorder(x->c[x->n]);
}
static int has(N *x, int k) {
    while (x) {
        int i = 0;
        while (i < x->n && x->k[i] < k) i++;
        if (i < x->n && x->k[i] == k) return 1;
        if (x->leaf) return 0;
        x = x->c[i];
    }
    return 0;
}
static int verify(N *root) {
    int cnt = 0, d = 0;
    if (root) d = check_node(root, 1, -1, 1000000, &cnt);
    check(cnt == mn, "key count");
    no = 0; inorder(root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
    return d;
}
static void freet(N *x) {
    if (!x) return;
    if (!x->leaf) for (int i = 0; i <= x->n; i++) freet(x->c[i]);
    free(x);
}

int main(void) {
    static const int degrees[4] = {2, 3, 4, 6};
    for (int di = 0; di < 4; di++) {
        T = degrees[di];
        splits = borrows = merges = 0;
        mn = 0;
        N *root = NULL;
        int d, ins_n = 0, del_n = 0, maxd = 0;
        for (int i = 0; i < 250; i++) { insert(&root, i); minsert(i); verify(root); }
        int asc_d = verify(root);
        for (int op = 0; op < 3000; op++) {
            int k = (int)(rnd() % 500);
            if (rnd() % 2) {
                int a = insert(&root, k);
                check(a == minsert(k), "insert result"); ins_n += a;
            } else {
                int a = delete(&root, k);
                check(a == mdelete(k), "delete result"); del_n += a;
            }
            d = verify(root);
            check(has(root, k) == mhas(k), "search");
            if (d > maxd) maxd = d;
        }
        printf("t=%d: ascending depth=%d, random inserts=%d deletes=%d size=%d max depth=%d\n",
               T, asc_d, ins_n, del_n, mn, maxd);
        printf("     splits=%ld borrows=%ld merges=%ld\n", splits, borrows, merges);
        while (mn) {
            int k = mod[(unsigned)mn / 2];
            check(delete(&root, k), "drain"); mdelete(k); verify(root);
        }
        check(root == NULL, "empty");
        freet(root);
    }
    printf("drained all\n");
    return 0;
}
