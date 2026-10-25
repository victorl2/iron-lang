/*
 * title: B*-tree with sibling redistribution and 2-to-3 splits
 * topic: data_structures
 * covers: b-star tree, b-tree comparison, sibling redistribution, two-to-three split, fill factor, ordered traversal
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M 8 /* max keys per node */

typedef struct BN {
    int leaf, n;
    int k[M + 2];
    struct BN *c[M + 3];
} BN;

typedef struct { BN *root; int star; long splits, redistributions, threeway; } Tree;

static unsigned long long rs = 0xB57A2ULL * 8191;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static BN *mk(int leaf) { BN *n = calloc(1, sizeof *n); n->leaf = leaf; return n; }
static int search(const BN *n, int key) {
    while (n) {
        int i = 0;
        while (i < n->n && key > n->k[i]) i++;
        if (i < n->n && n->k[i] == key) return 1;
        n = n->leaf ? NULL : n->c[i];
    }
    return 0;
}
/* Rebalance children [l, l+cnt) of p into r nodes (r == cnt: redistribute, r == cnt+1: split) */
static void rebalance(Tree *t, BN *p, int l, int cnt, int r) {
    int keys[3 * (M + 2)], nk = 0;
    BN *kids[3 * (M + 3)]; int nc = 0;
    int leaf = p->c[l]->leaf;
    for (int j = 0; j < cnt; j++) {
        BN *c = p->c[l + j];
        for (int i = 0; i < c->n; i++) keys[nk++] = c->k[i];
        if (!leaf) for (int i = 0; i <= c->n; i++) kids[nc++] = c->c[i];
        if (j < cnt - 1) keys[nk++] = p->k[l + j];
    }
    BN *nodes[3];
    for (int j = 0; j < cnt; j++) nodes[j] = p->c[l + j];
    if (r > cnt) nodes[cnt] = mk(leaf);
    if (r > cnt) { /* make room in the parent: one more child and one more key */
        for (int i = p->n; i > l + cnt - 1; i--) { p->k[i] = p->k[i - 1]; }
        for (int i = p->n + 1; i > l + cnt; i--) p->c[i] = p->c[i - 1];
        p->n++;
        p->c[l + cnt] = nodes[cnt];
    }
    int per = (nk - (r - 1)) / r, extra = (nk - (r - 1)) % r;
    int ki = 0, ci = 0;
    for (int j = 0; j < r; j++) {
        BN *c = nodes[j];
        int sz = per + (j < extra ? 1 : 0);
        c->n = sz;
        for (int i = 0; i < sz; i++) c->k[i] = keys[ki++];
        if (!leaf) for (int i = 0; i <= sz; i++) c->c[i] = kids[ci++];
        if (j < r - 1) p->k[l + j] = keys[ki++];
        p->c[l + j] = c;
    }
    check(ki == nk, "all keys redistributed");
}
static void fix_overflow(Tree *t, BN *p, int i) {
    if (!t->star) { t->splits++; rebalance(t, p, i, 1, 2); return; }
    if (i > 0 && p->c[i - 1]->n < M) { t->redistributions++; rebalance(t, p, i - 1, 2, 2); return; }
    if (i < p->n && p->c[i + 1]->n < M) { t->redistributions++; rebalance(t, p, i, 2, 2); return; }
    if (i < p->n) { t->threeway++; rebalance(t, p, i, 2, 3); return; }
    if (i > 0) { t->threeway++; rebalance(t, p, i - 1, 2, 3); return; }
    t->splits++; rebalance(t, p, i, 1, 2);
}
static void insert_rec(Tree *t, BN *n, int key) {
    int i = 0;
    while (i < n->n && key > n->k[i]) i++;
    if (n->leaf) {
        for (int j = n->n; j > i; j--) n->k[j] = n->k[j - 1];
        n->k[i] = key; n->n++;
        return;
    }
    insert_rec(t, n->c[i], key);
    if (n->c[i]->n > M) fix_overflow(t, n, i);
}
static int insert(Tree *t, int key) {
    if (search(t->root, key)) return 0;
    insert_rec(t, t->root, key);
    if (t->root->n > M) {
        BN *nr = mk(0); nr->c[0] = t->root; t->root = nr;
        t->splits++;
        rebalance(t, nr, 0, 1, 2);
    }
    return 1;
}
static void destroy(BN *n) { if (!n->leaf) for (int i = 0; i <= n->n; i++) destroy(n->c[i]); free(n); }

static int walk(const BN *n, int *out, int cnt) {
    for (int i = 0; i < n->n; i++) { if (!n->leaf) cnt = walk(n->c[i], out, cnt); out[cnt++] = n->k[i]; }
    if (!n->leaf) cnt = walk(n->c[n->n], out, cnt);
    return cnt;
}
typedef struct { int nodes, keys, height, minfill, leafd; } Stats;
static void stats(const BN *n, int depth, int root, Stats *s) {
    s->nodes++; s->keys += n->n;
    check(n->n <= M, "max fill");
    if (!root) { if (n->n < s->minfill) s->minfill = n->n; }
    if (n->leaf) { if (s->leafd < 0) s->leafd = depth; check(s->leafd == depth, "leaves at equal depth"); if (depth + 1 > s->height) s->height = depth + 1; return; }
    for (int i = 0; i < n->n; i++) check(n->k[i] > (n->c[i]->n ? n->c[i]->k[n->c[i]->n - 1] : -(1 << 30)), "separator above left child");
    for (int i = 0; i <= n->n; i++) stats(n->c[i], depth + 1, 0, s);
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(void) {
    const char *order[] = { "ascending", "random", "descending" };
    for (int o = 0; o < 3; o++) {
        enum { N = 4000 };
        static int keys[N];
        for (int i = 0; i < N; i++) keys[i] = o == 0 ? i * 3 : o == 2 ? (N - i) * 3 : (int)(rnd() % 12000);
        for (int star = 0; star < 2; star++) {
            Tree t = { mk(1), star, 0, 0, 0 };
            static int uniq[N]; static unsigned char seen[12002]; int nu = 0;
            memset(seen, 0, sizeof seen);
            for (int i = 0; i < N; i++) {
                int fresh = insert(&t, keys[i]);
                check(fresh == !seen[keys[i]], "insert result vs reference");
                seen[keys[i]] = 1;
                if (fresh) uniq[nu++] = keys[i];
                if (i == 200 || i == N - 1) for (int j = 0; j < nu; j += 37) check(search(t.root, uniq[j]), "member found");
            }
            static int seq[N + 1];
            int n = walk(t.root, seq, 0);
            qsort(uniq, (size_t)nu, sizeof(int), cmp_int);
            check(n == nu, "walk count");
            for (int i = 0; i < n; i++) check(seq[i] == uniq[i], "in-order equals sorted keys");
            check(!search(t.root, -5) && !search(t.root, 12001), "absent keys");
            Stats s = { 0, 0, 0, 1 << 30, -1 };
            stats(t.root, 0, 1, &s);
            check(s.keys == nu, "key count");
            check(s.minfill >= M / 2, "minimum fill at least half");
            printf("%-10s %-6s keys=%4d nodes=%4d height=%d fill=%2d%% min-keys=%d splits=%ld redistributions=%ld three-way=%ld\n",
                   order[o], star ? "B*" : "B", nu, s.nodes, s.height, s.keys * 100 / (s.nodes * M), s.minfill, t.splits, t.redistributions, t.threeway);
            destroy(t.root);
        }
    }
    return 0;
}
