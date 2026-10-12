/*
 * title: Day-Stout-Warren rebalancing of a BST
 * topic: data_structures
 * covers: DSW algorithm, tree to vine by right rotations, vine to balanced tree by compressions, perfect balance for every size, rotation counts, level occupancy, sorted-array model
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

static long rots_vine, rots_compress;
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
static int height(N *t) {
    if (!t) return 0;
    int a = height(t->l), b = height(t->r);
    return 1 + (a > b ? a : b);
}
static int count(N *t) { return t ? 1 + count(t->l) + count(t->r) : 0; }
static void inorder(N *t) { if (!t) return; inorder(t->l); outk[no++] = t->k; inorder(t->r); }
static void level_counts(N *t, int d, int *lv) { if (!t) return; lv[d]++; level_counts(t->l, d + 1, lv); level_counts(t->r, d + 1, lv); }

static int tree_to_vine(N *pseudo) {
    N *tail = pseudo, *rest = tail->r;
    int size = 0;
    while (rest) {
        if (!rest->l) { tail = rest; rest = rest->r; size++; }
        else {
            N *temp = rest->l;
            rest->l = temp->r;
            temp->r = rest;
            rest = temp;
            tail->r = temp;
            rots_vine++;
        }
    }
    return size;
}
static void compress(N *pseudo, int count_) {
    N *scanner = pseudo;
    for (int i = 0; i < count_; i++) {
        N *child = scanner->r;
        scanner->r = child->r;
        scanner = scanner->r;
        child->r = scanner->l;
        scanner->l = child;
        rots_compress++;
    }
}
static int floor_log2(int x) { int r = 0; while (x > 1) { x >>= 1; r++; } return r; }
static void vine_to_tree(N *pseudo, int size) {
    int leaves = size + 1 - (1 << floor_log2(size + 1));
    compress(pseudo, leaves);
    size -= leaves;
    while (size > 1) {
        size /= 2;
        compress(pseudo, size);
    }
}
static N *dsw(N *root) {
    N pseudo;
    pseudo.k = 0; pseudo.l = NULL; pseudo.r = root;
    int size = tree_to_vine(&pseudo);
    vine_to_tree(&pseudo, size);
    return pseudo.r;
}

int main(void) {
    int checked = 0;
    for (int n = 1; n <= 130; n++) {
        for (int shape = 0; shape < 3; shape++) {
            N *t = NULL;
            mn = 0;
            for (int i = 0; i < n; i++) {
                int k;
                if (shape == 0) k = i;
                else if (shape == 1) k = (i & 1) ? n - i / 2 : i / 2 + 1;   /* zig-zag inward */
                else k = (int)(rnd() % 100000);
                t = insert(t, k);
                minsert(k);
            }
            int nn = count(t);
            check(nn == mn, "count");
            int before = height(t);
            t = dsw(t);
            no = 0; inorder(t);
            check(no == mn && !memcmp(outk, mod, sizeof(int) * (size_t)mn), "inorder preserved");
            int minh = floor_log2(mn) + 1;
            check(height(t) == minh, "minimum height");
            int lv[32] = {0};
            level_counts(t, 0, lv);
            for (int d = 0; d < minh - 1; d++) check(lv[d] == (1 << d), "levels above the last are full");
            checked++;
            if (shape == 0 && (n == 1 || n == 7 || n == 12 || n == 100 || n == 130))
                printf("n=%3d ascending: height %3d -> %d, last level holds %d\n", n, before, height(t), lv[minh - 1]);
            freet(t);
        }
    }
    printf("%d trees rebalanced to minimum height\n", checked);
    printf("rotations: to-vine=%ld compress=%ld\n", rots_vine, rots_compress);
    /* idempotence: rebalancing a balanced tree rotates nothing in the compress phase beyond the fixed schedule */
    N *t = NULL;
    for (int i = 0; i < 1000; i++) t = insert(t, i);
    long before = rots_vine;
    t = dsw(t);
    printf("1000 ascending keys: vine rotations=%ld, height=%d\n", rots_vine - before, height(t));
    before = rots_vine;
    t = dsw(t);
    printf("second pass on the balanced tree: vine rotations=%ld, height=%d\n", rots_vine - before, height(t));
    freet(t);
    return 0;
}
