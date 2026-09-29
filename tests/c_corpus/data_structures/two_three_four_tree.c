/*
 * title: 2-3-4 tree with top-down splitting and merging
 * topic: data_structures
 * covers: 2-3-4 tree, preemptive split of 4-nodes, top-down delete with borrow and merge, red-black equivalence counts, invariant checker, sorted-array model
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
    int n;              /* 1..3 keys */
    int k[3];
    struct N *c[4];
} N;

static long splits, merges, borrows;
static N *mknode(void) {
    N *x = xmalloc(sizeof *x);
    x->n = 0;
    x->k[0] = x->k[1] = x->k[2] = 0;
    x->c[0] = x->c[1] = x->c[2] = x->c[3] = NULL;
    return x;
}
static void split_child(N *p, int i) {
    N *y = p->c[i], *z = mknode();
    int mid = y->k[1];
    z->n = 1; z->k[0] = y->k[2];
    z->c[0] = y->c[2]; z->c[1] = y->c[3];
    y->n = 1; y->c[2] = y->c[3] = NULL;
    for (int j = p->n; j > i; j--) p->k[j] = p->k[j - 1];
    for (int j = p->n + 1; j > i + 1; j--) p->c[j] = p->c[j - 1];
    p->k[i] = mid; p->c[i + 1] = z;
    p->n++;
    splits++;
}
static int insert(N **root, int k) {
    if (!*root) {
        *root = mknode();
        (*root)->n = 1; (*root)->k[0] = k;
        return 1;
    }
    if ((*root)->n == 3) {
        N *s = mknode();
        s->c[0] = *root;
        split_child(s, 0);
        *root = s;
    }
    N *x = *root;
    for (;;) {
        int i = 0;
        while (i < x->n && x->k[i] < k) i++;
        if (i < x->n && x->k[i] == k) return 0;
        if (!x->c[0]) {
            for (int j = x->n; j > i; j--) x->k[j] = x->k[j - 1];
            x->k[i] = k;
            x->n++;
            return 1;
        }
        if (x->c[i]->n == 3) {
            split_child(x, i);
            if (k == x->k[i]) return 0;
            if (k > x->k[i]) i++;
        }
        x = x->c[i];
    }
}
static void merge(N *x, int j) {
    N *l = x->c[j], *r = x->c[j + 1];
    l->k[l->n] = x->k[j];
    for (int t = 0; t < r->n; t++) l->k[l->n + 1 + t] = r->k[t];
    for (int t = 0; t <= r->n; t++) l->c[l->n + 1 + t] = r->c[t];
    l->n += r->n + 1;
    for (int t = j; t < x->n - 1; t++) x->k[t] = x->k[t + 1];
    for (int t = j + 1; t < x->n; t++) x->c[t] = x->c[t + 1];
    x->c[x->n] = NULL;
    x->n--;
    free(r);
    merges++;
}
/* make sure child i has at least 2 keys; returns the (possibly shifted) child index */
static int ensure(N *x, int i) {
    N *c = x->c[i];
    if (i > 0 && x->c[i - 1]->n >= 2) {
        N *l = x->c[i - 1];
        for (int t = c->n; t > 0; t--) c->k[t] = c->k[t - 1];
        for (int t = c->n + 1; t > 0; t--) c->c[t] = c->c[t - 1];
        c->k[0] = x->k[i - 1];
        c->c[0] = l->c[l->n];
        x->k[i - 1] = l->k[l->n - 1];
        l->c[l->n] = NULL;
        l->n--; c->n++;
        borrows++;
        return i;
    }
    if (i < x->n && x->c[i + 1]->n >= 2) {
        N *r = x->c[i + 1];
        c->k[c->n] = x->k[i];
        c->c[c->n + 1] = r->c[0];
        x->k[i] = r->k[0];
        for (int t = 0; t < r->n - 1; t++) r->k[t] = r->k[t + 1];
        for (int t = 0; t < r->n; t++) r->c[t] = r->c[t + 1];
        r->c[r->n] = NULL;
        r->n--; c->n++;
        borrows++;
        return i;
    }
    if (i < x->n) { merge(x, i); return i; }
    merge(x, i - 1);
    return i - 1;
}
static int del(N *x, int k) {
    int i = 0;
    while (i < x->n && x->k[i] < k) i++;
    int found = i < x->n && x->k[i] == k;
    if (!x->c[0]) {
        if (!found) return 0;
        for (int t = i; t < x->n - 1; t++) x->k[t] = x->k[t + 1];
        x->n--;
        return 1;
    }
    if (found) {
        if (x->c[i]->n >= 2) {
            N *m = x->c[i];
            while (m->c[0]) m = m->c[m->n];
            int pred = m->k[m->n - 1];
            x->k[i] = pred;
            return del(x->c[i], pred);
        }
        if (x->c[i + 1]->n >= 2) {
            N *m = x->c[i + 1];
            while (m->c[0]) m = m->c[0];
            int succ = m->k[0];
            x->k[i] = succ;
            return del(x->c[i + 1], succ);
        }
        merge(x, i);
        return del(x->c[i], k);
    }
    if (x->c[i]->n == 1) i = ensure(x, i);
    return del(x->c[i], k);
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
static int check_node(N *x, long lo, long hi, int *cnt, int *nodes) {
    if (!x) return 0;
    check(x->n >= 1 && x->n <= 3, "node size");
    for (int i = 1; i < x->n; i++) check(x->k[i - 1] < x->k[i], "sorted");
    check(x->k[0] > lo && x->k[x->n - 1] < hi, "bounds");
    *cnt += x->n; (*nodes)++;
    int d = -1;
    for (int i = 0; i <= x->n; i++) {
        long l2 = i == 0 ? lo : x->k[i - 1];
        long h2 = i == x->n ? hi : x->k[i];
        int cd = check_node(x->c[i], l2, h2, cnt, nodes);
        if (d < 0) d = cd;
        check(cd == d, "uniform depth");
    }
    for (int i = x->n + 1; i < 4; i++) check(x->c[i] == NULL, "unused slot");
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
static int verify(N *root, int *nodes) {
    int cnt = 0;
    *nodes = 0;
    int d = check_node(root, -1, 1000000, &cnt, nodes);
    check(cnt == mn, "key count");
    no = 0; inorder(root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder model");
    return d;
}
static void freet(N *x) {
    if (!x) return;
    for (int i = 0; i <= x->n; i++) freet(x->c[i]);
    free(x);
}

int main(void) {
    N *root = NULL;
    int nodes, d, ins_n = 0, del_n = 0, maxd = 0;
    for (int i = 0; i < 300; i++) { insert(&root, i); minsert(i); verify(root, &nodes); }
    d = verify(root, &nodes);
    printf("ascending 300: depth=%d nodes=%d splits=%ld\n", d, nodes, splits);
    printf("red-black view: %d black, %d red\n", nodes, mn - nodes);
    for (int op = 0; op < 6000; op++) {
        int k = (int)(rnd() % 500);
        if (rnd() % 2) {
            int a = insert(&root, k);
            check(a == minsert(k), "insert result"); ins_n += a;
        } else {
            int a = delete(&root, k);
            check(a == mdelete(k), "delete result"); del_n += a;
        }
        d = verify(root, &nodes);
        check(has(root, k) == mhas(k), "search");
        if (d > maxd) maxd = d;
    }
    printf("random: inserts=%d deletes=%d size=%d max depth=%d\n", ins_n, del_n, mn, maxd);
    printf("splits=%ld borrows=%ld merges=%ld\n", splits, borrows, merges);
    d = verify(root, &nodes);
    printf("final: depth=%d nodes=%d (%d black, %d red)\n", d, nodes, nodes, mn - nodes);
    while (mn) {
        int k = mod[(unsigned)mn / 3];
        check(delete(&root, k), "drain"); mdelete(k); verify(root, &nodes);
    }
    check(root == NULL, "empty");
    printf("drained\n");
    freet(root);
    return 0;
}
