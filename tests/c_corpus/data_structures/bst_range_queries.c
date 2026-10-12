/*
 * title: BST range queries with size and sum augmentation
 * topic: data_structures
 * covers: binary search tree, subtree size and sum augmentation, range count, range sum, pruned range enumeration, k nearest keys, range delete, floor and ceiling, sorted-array model
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
    int k;
    int size;
    long sum;
    struct N *l, *r;
} N;

static int sz(N *t) { return t ? t->size : 0; }
static long sm(N *t) { return t ? t->sum : 0; }
static void upd(N *t) { t->size = 1 + sz(t->l) + sz(t->r); t->sum = t->k + sm(t->l) + sm(t->r); }
static N *ins(N *t, int k, int *added) {
    if (!t) {
        N *n = xmalloc(sizeof *n);
        n->k = k; n->l = n->r = NULL;
        upd(n);
        *added = 1;
        return n;
    }
    if (k < t->k) t->l = ins(t->l, k, added);
    else if (k > t->k) t->r = ins(t->r, k, added);
    else return t;
    upd(t);
    return t;
}
/* number of keys < k */
static int count_less(N *t, int k) {
    int c = 0;
    while (t) {
        if (k <= t->k) t = t->l;
        else { c += sz(t->l) + 1; t = t->r; }
    }
    return c;
}
static long sum_less(N *t, int k) {
    long s = 0;
    while (t) {
        if (k <= t->k) t = t->l;
        else { s += sm(t->l) + t->k; t = t->r; }
    }
    return s;
}
static long visited;
/* enumerate [lo, hi], pruning subtrees that cannot intersect */
static int collect(N *t, int lo, int hi, int *out, int n) {
    if (!t) return n;
    visited++;
    if (lo < t->k) n = collect(t->l, lo, hi, out, n);
    if (lo <= t->k && t->k <= hi) out[n++] = t->k;
    if (t->k < hi) n = collect(t->r, lo, hi, out, n);
    return n;
}
static int floor_key(N *t, int k, int *out) {
    int f = 0;
    while (t) {
        if (t->k <= k) { *out = t->k; f = 1; t = t->r; } else t = t->l;
    }
    return f;
}
static int ceil_key(N *t, int k, int *out) {
    int f = 0;
    while (t) {
        if (t->k >= k) { *out = t->k; f = 1; t = t->l; } else t = t->r;
    }
    return f;
}
static int kth(N *t, int i) {
    for (;;) {
        int ls = sz(t->l);
        if (i < ls) t = t->l;
        else if (i == ls) return t->k;
        else { i -= ls + 1; t = t->r; }
    }
}
/* k nearest keys to x (ties prefer the smaller key): expand outward from the rank of x */
static int nearest(N *t, int x, int k, int *out) {
    int total = sz(t), r = count_less(t, x);
    int lo = r - 1, hi = r, n = 0;
    while (n < k && (lo >= 0 || hi < total)) {
        if (hi >= total) out[n++] = kth(t, lo--);
        else if (lo < 0) out[n++] = kth(t, hi++);
        else {
            int a = kth(t, lo), b = kth(t, hi);
            if (x - a <= b - x) { out[n++] = a; lo--; }
            else { out[n++] = b; hi++; }
        }
    }
    return n;
}
static N *attach_min(N *a, N *b) {   /* all of a < all of b; result keeps BST order */
    if (!a) return b;
    if (!b) return a;
    N *m = b;
    while (m->l) m = m->l;
    /* hang a below the leftmost node of b, fixing sizes along the left spine */
    N *stack[MAXN]; int sp = 0;
    for (N *x = b; x != m; x = x->l) stack[sp++] = x;
    m->l = a;
    upd(m);
    while (sp) upd(stack[--sp]);
    return b;
}
static N *range_delete(N *t, int lo, int hi, int *removed) {
    if (!t) return NULL;
    if (t->k < lo) t->r = range_delete(t->r, lo, hi, removed);
    else if (t->k > hi) t->l = range_delete(t->l, lo, hi, removed);
    else {
        N *l = range_delete(t->l, lo, hi, removed);
        N *r = range_delete(t->r, lo, hi, removed);
        free(t);
        (*removed)++;
        return attach_min(l, r);
    }
    upd(t);
    return t;
}
static int verify(N *t, long lo, long hi) {
    if (!t) return 0;
    check(t->k > lo && t->k < hi, "order");
    int c = 1 + verify(t->l, lo, t->k) + verify(t->r, t->k, hi);
    check(c == t->size, "size field");
    check(t->sum == t->k + sm(t->l) + sm(t->r), "sum field");
    return c;
}
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }

int main(void) {
    N *root = NULL;
    int added;
    static int out[MAXN];
    for (int i = 0; i < 700; i++) {
        int k = (int)(rnd() % 5000);
        added = 0; root = ins(root, k, &added);
        if (added) minsert(k);
    }
    check(verify(root, -1, 100000) == mn, "built");
    printf("built tree: size=%d height=%d min=%d max=%d\n", mn, height(root), mod[0], mod[mn - 1]);
    long checked = 0, enumerated = 0, node_visits = 0;
    for (int q = 0; q < 800; q++) {
        int lo = (int)(rnd() % 5200) - 100;
        int hi = lo + (int)(rnd() % 600);
        int p = mfind(lo), qq = mfind(hi + 1);
        int cnt = count_less(root, hi + 1) - count_less(root, lo);
        check(cnt == qq - p, "range count");
        long s = 0;
        for (int i = p; i < qq; i++) s += mod[i];
        check(sum_less(root, hi + 1) - sum_less(root, lo) == s, "range sum");
        visited = 0;
        int n = collect(root, lo, hi, out, 0);
        check(n == cnt && !memcmp(out, mod + p, sizeof(int) * (size_t)n), "range enumeration");
        check(visited <= 2L * height(root) + 2L * n + 2, "pruned traversal is O(h + k)");
        enumerated += n; node_visits += visited; checked++;
        int f, c, fk = 0, ck = 0;
        f = floor_key(root, lo, &fk); c = ceil_key(root, lo, &ck);
        int mf = p < mn && mod[p] == lo ? 1 : p > 0;
        int mfv = p < mn && mod[p] == lo ? lo : (p > 0 ? mod[p - 1] : 0);
        check(f == mf && (!f || fk == mfv), "floor");
        check(c == (p < mn) && (!c || ck == mod[p]), "ceil");
    }
    printf("%ld range queries: %ld keys enumerated with %ld node visits\n", checked, enumerated, node_visits);
    /* k nearest neighbours against a brute-force scan */
    long nn_sum = 0;
    for (int q = 0; q < 100; q++) {
        int x = (int)(rnd() % 5000), k = 1 + (int)(rnd() % 8);
        int n = nearest(root, x, k, out);
        int used[MAXN] = {0};
        for (int i = 0; i < n; i++) {
            int best = -1;
            for (int j = 0; j < mn; j++) {
                if (used[j]) continue;
                int dj = abs(mod[j] - x);
                if (best < 0 || dj < abs(mod[best] - x) || (dj == abs(mod[best] - x) && mod[j] < mod[best])) best = j;
            }
            used[best] = 1;
            check(mod[best] == out[i], "nearest neighbour");
            nn_sum += out[i];
        }
    }
    printf("nearest-neighbour checksum=%ld\n", nn_sum);
    /* range deletes */
    int total_removed = 0;
    for (int r = 0; r < 6; r++) {
        int lo = (int)(rnd() % 4500), hi = lo + 100 + (int)(rnd() % 400), removed = 0;
        root = range_delete(root, lo, hi, &removed);
        int p = mfind(lo), q2 = mfind(hi + 1);
        check(removed == q2 - p, "range delete count");
        memmove(mod + p, mod + q2, (size_t)(mn - q2) * sizeof(int));
        mn -= q2 - p;
        check(verify(root, -1, 100000) == mn, "after range delete");
        total_removed += removed;
        printf("delete [%d,%d]: removed %d, size now %d, height %d\n", lo, hi, removed, mn, height(root));
    }
    check(total_removed >= 0, "sanity");
    freet(root);
    return 0;
}
