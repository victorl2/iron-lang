/*
 * title: B+ tree with leaf chaining and range scan
 * topic: data_structures
 * covers: B+ tree, data only in leaves, separator keys, leaf split copies key, linked leaves, range scan, borrow and merge on delete, key-count invariants, sorted-array model
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

#define M 5      /* max keys per node */
#define MIN 2    /* min keys per non-root node */

typedef struct N {
    int leaf, n;
    int k[M + 1];
    int v[M + 1];            /* payload, leaves only */
    struct N *c[M + 2];
    struct N *next;          /* leaf chain */
} N;

static long splits, borrows, merges;
static int payload(int k) { return k * 7 + 3; }
static N *mknode(int leaf) {
    N *x = xmalloc(sizeof *x);
    memset(x, 0, sizeof *x);
    x->leaf = leaf;
    return x;
}
static int child_index(N *x, int k) {
    int i = 0;
    while (i < x->n && k >= x->k[i]) i++;
    return i;
}
static int ins(N *x, int k, int *added, int *up, N **right) {
    if (x->leaf) {
        int i = 0;
        while (i < x->n && x->k[i] < k) i++;
        if (i < x->n && x->k[i] == k) return 0;
        for (int j = x->n; j > i; j--) { x->k[j] = x->k[j - 1]; x->v[j] = x->v[j - 1]; }
        x->k[i] = k; x->v[i] = payload(k);
        x->n++;
        *added = 1;
        if (x->n <= M) return 0;
        N *r = mknode(1);
        int keep = x->n / 2;
        r->n = x->n - keep;
        for (int j = 0; j < r->n; j++) { r->k[j] = x->k[keep + j]; r->v[j] = x->v[keep + j]; }
        x->n = keep;
        r->next = x->next; x->next = r;
        *up = r->k[0]; *right = r;
        splits++;
        return 1;
    }
    int i = child_index(x, k), u;
    N *r = NULL;
    if (!ins(x->c[i], k, added, &u, &r)) return 0;
    for (int j = x->n; j > i; j--) x->k[j] = x->k[j - 1];
    for (int j = x->n + 1; j > i + 1; j--) x->c[j] = x->c[j - 1];
    x->k[i] = u; x->c[i + 1] = r;
    x->n++;
    if (x->n <= M) return 0;
    int L = (x->n - 1) / 2;
    N *s = mknode(0);
    *up = x->k[L];
    s->n = x->n - L - 1;
    for (int j = 0; j < s->n; j++) s->k[j] = x->k[L + 1 + j];
    for (int j = 0; j <= s->n; j++) { s->c[j] = x->c[L + 1 + j]; x->c[L + 1 + j] = NULL; }
    x->n = L;
    *right = s;
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
    if (i > 0 && x->c[i - 1]->n > MIN) {
        N *l = x->c[i - 1];
        for (int j = c->n; j > 0; j--) { c->k[j] = c->k[j - 1]; c->v[j] = c->v[j - 1]; }
        if (c->leaf) {
            c->k[0] = l->k[l->n - 1]; c->v[0] = l->v[l->n - 1];
            x->k[i - 1] = c->k[0];
        } else {
            for (int j = c->n + 1; j > 0; j--) c->c[j] = c->c[j - 1];
            c->k[0] = x->k[i - 1];
            c->c[0] = l->c[l->n]; l->c[l->n] = NULL;
            x->k[i - 1] = l->k[l->n - 1];
        }
        l->n--; c->n++;
        borrows++;
    } else if (i < x->n && x->c[i + 1]->n > MIN) {
        N *r = x->c[i + 1];
        if (c->leaf) {
            c->k[c->n] = r->k[0]; c->v[c->n] = r->v[0];
            for (int j = 0; j < r->n - 1; j++) { r->k[j] = r->k[j + 1]; r->v[j] = r->v[j + 1]; }
            x->k[i] = r->k[0];
        } else {
            c->k[c->n] = x->k[i];
            c->c[c->n + 1] = r->c[0];
            x->k[i] = r->k[0];
            for (int j = 0; j < r->n - 1; j++) r->k[j] = r->k[j + 1];
            for (int j = 0; j < r->n; j++) r->c[j] = r->c[j + 1];
            r->c[r->n] = NULL;
        }
        r->n--; c->n++;
        borrows++;
    } else {
        int j = i < x->n ? i : i - 1;
        N *l = x->c[j], *r = x->c[j + 1];
        if (l->leaf) {
            for (int t = 0; t < r->n; t++) { l->k[l->n + t] = r->k[t]; l->v[l->n + t] = r->v[t]; }
            l->n += r->n;
            l->next = r->next;
        } else {
            l->k[l->n] = x->k[j];
            for (int t = 0; t < r->n; t++) l->k[l->n + 1 + t] = r->k[t];
            for (int t = 0; t <= r->n; t++) l->c[l->n + 1 + t] = r->c[t];
            l->n += r->n + 1;
        }
        for (int t = j; t < x->n - 1; t++) x->k[t] = x->k[t + 1];
        for (int t = j + 1; t < x->n; t++) x->c[t] = x->c[t + 1];
        x->c[x->n] = NULL;
        x->n--;
        free(r);
        merges++;
    }
}
static int del(N *x, int k) {
    if (x->leaf) {
        int i = 0;
        while (i < x->n && x->k[i] < k) i++;
        if (i >= x->n || x->k[i] != k) return 0;
        for (int j = i; j < x->n - 1; j++) { x->k[j] = x->k[j + 1]; x->v[j] = x->v[j + 1]; }
        x->n--;
        return 1;
    }
    int i = child_index(x, k);
    int r = del(x->c[i], k);
    if (x->c[i]->n < MIN) fix(x, i);
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
static N *leftmost(N *x) { while (x && !x->leaf) x = x->c[0]; return x; }
static int check_node(N *x, int is_root, long lo, long hi, int *cnt) {
    check(x->n <= M, "overfull");
    if (!is_root) check(x->n >= MIN, "underfull");
    else check(x->n >= 1, "root nonempty");
    for (int i = 1; i < x->n; i++) check(x->k[i - 1] < x->k[i], "sorted");
    if (x->leaf) {
        for (int i = 0; i < x->n; i++) {
            check(x->k[i] >= lo && x->k[i] < hi, "leaf bounds");
            check(x->v[i] == payload(x->k[i]), "payload");
        }
        *cnt += x->n;
        return 1;
    }
    int d = -1;
    for (int i = 0; i <= x->n; i++) {
        long l2 = i == 0 ? lo : x->k[i - 1];
        long h2 = i == x->n ? hi : x->k[i];
        int cd = check_node(x->c[i], 0, l2, h2, cnt);
        if (d < 0) d = cd;
        check(cd == d, "uniform depth");
    }
    return d + 1;
}
static int chain_scan(N *root) {
    int n = 0;
    for (N *l = leftmost(root); l; l = l->next)
        for (int i = 0; i < l->n; i++) outk[n++] = l->k[i];
    return n;
}
/* keys in [lo, hi] via the leaf chain; returns count and number of leaves touched */
static int range_scan(N *root, int lo, int hi, int *out, int *leaves) {
    N *x = root;
    int n = 0;
    *leaves = 0;
    if (!x) return 0;
    while (!x->leaf) x = x->c[child_index(x, lo)];
    for (; x; x = x->next) {
        (*leaves)++;
        for (int i = 0; i < x->n; i++) {
            if (x->k[i] > hi) return n;
            if (x->k[i] >= lo) out[n++] = x->k[i];
        }
    }
    return n;
}
static int find(N *x, int k) {
    if (!x) return 0;
    while (!x->leaf) x = x->c[child_index(x, k)];
    for (int i = 0; i < x->n; i++) if (x->k[i] == k) return x->v[i] == payload(k);
    return 0;
}
static int verify(N *root) {
    int cnt = 0, d = 0;
    if (root) d = check_node(root, 1, -1000000, 1000000, &cnt);
    check(cnt == mn, "key count");
    no = chain_scan(root);
    check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "leaf chain equals model");
    return d;
}
static void freet(N *x) {
    if (!x) return;
    if (!x->leaf) for (int i = 0; i <= x->n; i++) freet(x->c[i]);
    free(x);
}

int main(void) {
    N *root = NULL;
    int d, ins_n = 0, del_n = 0, maxd = 0;
    static int got[MAXN];
    long scanned = 0, leaves_total = 0;
    for (int i = 0; i < 200; i++) { insert(&root, i * 2); minsert(i * 2); verify(root); }
    printf("even keys 0..398: depth=%d splits=%ld\n", verify(root), splits);
    for (int op = 0; op < 4000; op++) {
        int k = (int)(rnd() % 500);
        if (rnd() % 100 < 50) {
            int a = insert(&root, k);
            check(a == minsert(k), "insert result"); ins_n += a;
        } else {
            int a = delete(&root, k);
            check(a == mdelete(k), "delete result"); del_n += a;
        }
        d = verify(root);
        check(find(root, k) == mhas(k), "find");
        if (d > maxd) maxd = d;
        if (op % 4 == 0) {
            int lo = (int)(rnd() % 500), hi = lo + (int)(rnd() % 80), leaves;
            int n = range_scan(root, lo, hi, got, &leaves);
            int p = mfind(lo), q = 0;
            while (p + q < mn && mod[p + q] <= hi) q++;
            check(n == q && !memcmp(got, mod + p, sizeof(int) * (size_t)q), "range scan");
            scanned += n; leaves_total += leaves;
        }
    }
    printf("random: inserts=%d deletes=%d size=%d max depth=%d\n", ins_n, del_n, mn, maxd);
    printf("splits=%ld borrows=%ld merges=%ld\n", splits, borrows, merges);
    printf("range scans: %ld keys returned over %ld leaf visits\n", scanned, leaves_total);
    int leaves;
    int n = range_scan(root, 100, 140, got, &leaves);
    printf("scan [100,140]: %d keys, first=%d last=%d, %d leaves\n", n, n ? got[0] : -1, n ? got[n - 1] : -1, leaves);
    while (mn) {
        int k = mod[(unsigned)mn / 2];
        check(delete(&root, k), "drain"); mdelete(k); verify(root);
    }
    check(root == NULL, "empty");
    printf("drained\n");
    freet(root);
    return 0;
}
