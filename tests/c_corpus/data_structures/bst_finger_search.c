/*
 * title: Finger search in a BST with subtree bounds
 * topic: data_structures
 * covers: finger search, parent pointers, subtree min and max bounds, climb then descend, locality of reference, search step counts versus root search, sorted-array model
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
    int k, lo, hi;              /* subtree key range */
    struct N *l, *r, *p;
} N;

static N *pool[MAXN];
static int pn;
static N *build(const int *a, int lo, int hi, N *par) {
    if (lo >= hi) return NULL;
    int m = (lo + hi) / 2;
    N *t = xmalloc(sizeof *t);
    pool[pn++] = t;
    t->k = a[m]; t->p = par;
    t->l = build(a, lo, m, t);
    t->r = build(a, m + 1, hi, t);
    t->lo = t->l ? t->l->lo : t->k;
    t->hi = t->r ? t->r->hi : t->k;
    return t;
}
static long steps;
/* plain search from the root */
static N *search_root(N *root, int k) {
    N *x = root;
    while (x && x->k != k) { steps++; x = k < x->k ? x->l : x->r; }
    return x;
}
/* finger search: climb until the subtree range covers k, then descend */
static N *search_finger(N *finger, int k, N **new_finger) {
    N *x = finger;
    while (x->p && (k < x->lo || k > x->hi)) { steps++; x = x->p; }
    N *last = x;
    while (x && x->k != k) { steps++; last = x; x = k < x->k ? x->l : x->r; }
    *new_finger = x ? x : last;
    return x;
}
static int depth(N *x) { int d = 0; while (x->p) { d++; x = x->p; } return d; }
static void freeall(void) { for (int i = 0; i < pn; i++) free(pool[i]); pn = 0; }

int main(void) {
    static int keys[MAXN];
    int n = 1500;
    for (int i = 0; i < n; i++) keys[i] = i * 4 + 1;      /* present keys are 1 mod 4 */
    for (int i = 0; i < n; i++) { mod[i] = keys[i]; }
    mn = n;
    N *root = build(keys, 0, n, NULL);
    int dmax = 0;
    for (int i = 0; i < pn; i++) { int d = depth(pool[i]); if (d > dmax) dmax = d; }
    printf("tree: %d nodes, depth %d\n", n, dmax + 1);

    /* pattern 1: local walk (each query near the previous one) */
    long root_steps, finger_steps;
    N *f = root;
    int cur = 2000;
    steps = 0;
    for (int q = 0; q < 3000; q++) {
        cur += (int)(rnd() % 41) - 20;
        if (cur < 0) cur = 0;
        if (cur > n * 4) cur = n * 4;
        N *a = search_finger(f, cur, &f);
        check((a != NULL) == mhas(cur), "finger local");
    }
    finger_steps = steps;
    printf("local walk of 3000 queries: finger steps=%ld\n", finger_steps);
    /* pattern 2: sequential sweep with a moving finger vs restarting from the root */
    f = root; steps = 0;
    for (int k = 0; k <= n * 4; k += 2) {
        N *a = search_finger(f, k, &f);
        check((a != NULL) == mhas(k), "finger sweep");
    }
    finger_steps = steps;
    steps = 0;
    for (int k = 0; k <= n * 4; k += 2) {
        N *a = search_root(root, k);
        check((a != NULL) == mhas(k), "root sweep");
    }
    root_steps = steps;
    printf("sequential sweep of %d queries: finger steps=%ld root steps=%ld\n", n * 2 + 1, finger_steps, root_steps);
    check(finger_steps < root_steps, "finger wins on a sweep");
    /* pattern 3: uniformly random queries, finger has no advantage */
    f = root; steps = 0;
    unsigned long long save = rs;
    for (int q = 0; q < 3000; q++) {
        int k = (int)(rnd() % (unsigned)(n * 4));
        N *a = search_finger(f, k, &f);
        check((a != NULL) == mhas(k), "finger random");
    }
    finger_steps = steps;
    rs = save; steps = 0;
    for (int q = 0; q < 3000; q++) {
        int k = (int)(rnd() % (unsigned)(n * 4));
        N *a = search_root(root, k);
        check((a != NULL) == mhas(k), "root random");
    }
    root_steps = steps;
    printf("random queries: finger steps=%ld root steps=%ld\n", finger_steps, root_steps);
    /* pattern 4: distance-sensitive cost: search cost grows with log of the key gap */
    for (int gap = 1; gap <= 1024; gap *= 4) {
        long tot = 0;
        int trials = 400;
        for (int t = 0; t < trials; t++) {
            int i = (int)(rnd() % (unsigned)(n - 1100));
            N *start = search_root(root, keys[i]);
            steps = 0;
            N *tmp;
            N *a = search_finger(start, keys[i + gap], &tmp);
            check(a && a->k == keys[i + gap], "gap search");
            tot += steps;
        }
        printf("gap %4d keys: average steps x100 = %ld\n", gap, tot * 100 / trials);
    }
    freeall();
    return 0;
}
