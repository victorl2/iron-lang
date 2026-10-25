/*
 * title: Recovering a BST with swapped or corrupted values
 * topic: data_structures
 * covers: BST validation, iterative inorder, two out-of-order nodes, adjacent and distant swaps, repair by swapping back, repair of arbitrary corruption by re-sorting values, stack-based traversal
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
    struct N *l, *r;
} N;

static N *insert(N *t, int k) {
    N **p = &t;
    while (*p) {
        if (k == (*p)->k) return t;
        p = k < (*p)->k ? &(*p)->l : &(*p)->r;
    }
    N *n = xmalloc(sizeof *n);
    n->k = k; n->l = n->r = NULL;
    *p = n;
    return t;
}
static void freet(N *t) { if (!t) return; freet(t->l); freet(t->r); free(t); }
static int count(N *t) { return t ? 1 + count(t->l) + count(t->r) : 0; }
static N *nodes[MAXN];
static int nn;
static void collect(N *t) { if (!t) return; collect(t->l); nodes[nn++] = t; collect(t->r); }

static int is_bst(N *t) {
    N *st[MAXN]; int sp = 0, have = 0, prev = 0;
    N *c = t;
    while (c || sp) {
        while (c) { st[sp++] = c; c = c->l; }
        c = st[--sp];
        if (have && c->k <= prev) return 0;
        prev = c->k; have = 1;
        c = c->r;
    }
    return 1;
}
/* find the two misplaced nodes in one iterative inorder pass; returns number of violations seen */
static int find_swapped(N *t, N **first, N **second) {
    N *st[MAXN], *prev = NULL;
    int sp = 0, violations = 0;
    N *c = t;
    *first = *second = NULL;
    while (c || sp) {
        while (c) { st[sp++] = c; c = c->l; }
        c = st[--sp];
        if (prev && prev->k > c->k) {
            violations++;
            if (!*first) *first = prev;
            *second = c;
        }
        prev = c;
        c = c->r;
    }
    return violations;
}
static void swap_vals(N *a, N *b) { int t = a->k; a->k = b->k; b->k = t; }
static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}
/* repair any corruption of values: sort the inorder values and write them back */
static void repair_by_sorting(N *t) {
    int vals[MAXN];
    nn = 0; collect(t);
    for (int i = 0; i < nn; i++) vals[i] = nodes[i]->k;
    qsort(vals, (size_t)nn, sizeof(int), cmp_int);
    for (int i = 0; i < nn; i++) nodes[i]->k = vals[i];
}

int main(void) {
    int fixed_adjacent = 0, fixed_distant = 0, sorted_fixed = 0;
    long total_nodes = 0;
    for (int round = 0; round < 80; round++) {
        int n = 2 + (int)(rnd() % 150);
        N *t = NULL;
        mn = 0;
        while (mn < n) {
            int k = (int)(rnd() % 5000);
            if (minsert(k)) t = insert(t, k);
        }
        check(is_bst(t), "starts valid");
        nn = 0; collect(t);
        total_nodes += nn;
        int i = (int)(rnd() % (unsigned)nn), j;
        int adjacent = round % 2 == 0;
        if (adjacent) j = (i + 1 < nn) ? i + 1 : i - 1;
        else { do { j = (int)(rnd() % (unsigned)nn); } while (j == i || j == i + 1 || j == i - 1 || nn < 4); }
        if (nn < 4 && !adjacent) { adjacent = 1; j = (i + 1 < nn) ? i + 1 : i - 1; }
        swap_vals(nodes[i], nodes[j]);
        check(!is_bst(t), "corrupted tree is invalid");
        N *a, *b;
        int v = find_swapped(t, &a, &b);
        check(v == 1 || v == 2, "one or two violations");
        check((v == 1) == (abs(i - j) == 1), "adjacent swap gives one violation");
        swap_vals(a, b);
        check(is_bst(t), "repaired");
        no = 0;
        nn = 0; collect(t);
        for (int q = 0; q < nn; q++) check(nodes[q]->k == mod[q], "values restored exactly");
        if (v == 1) fixed_adjacent++; else fixed_distant++;
        /* heavier corruption: shuffle several values, repair by sorting */
        for (int r = 0; r < 5; r++) {
            int p1 = (int)(rnd() % (unsigned)nn), p2 = (int)(rnd() % (unsigned)nn);
            swap_vals(nodes[p1], nodes[p2]);
        }
        repair_by_sorting(t);
        check(is_bst(t), "sort repair");
        nn = 0; collect(t);
        for (int q = 0; q < nn; q++) check(nodes[q]->k == mod[q], "sort repair values");
        sorted_fixed++;
        if (round < 3) printf("round %d: n=%d swapped positions %d and %d (%s), first=%d second=%d\n", round, nn, i, j, v == 1 ? "adjacent" : "distant", a->k, b->k);
        check(count(t) == n, "count");
        freet(t);
    }
    printf("swap repairs: %d adjacent, %d distant; sort repairs: %d; nodes processed=%ld\n", fixed_adjacent, fixed_distant, sorted_fixed, total_nodes);
    /* no violations in a valid tree */
    N *t = NULL;
    for (int i = 0; i < 20; i++) t = insert(t, (i * 7) % 20);
    N *a, *b;
    printf("violations in a valid tree: %d\n", find_swapped(t, &a, &b));
    freet(t);
    return 0;
}
