/*
 * title: ID3 decision tree on synthetic data
 * topic: data_structures
 * covers: decision tree, id3, entropy and information gain, categorical splits, majority leaves, train and test accuracy
 * deps: libc, libm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NA 5     /* attributes */
#define NV 3     /* values per attribute */
#define MAXS 800

typedef struct { int a[NA]; int y; } Sample;
typedef struct DT {
    int attr;                /* -1 for leaf */
    int label;               /* majority class */
    int n, pos;              /* training samples reaching the node, and how many positive */
    struct DT *kid[NV];
} DT;

static unsigned long long rs = 0x1D3C0DEULL * 6151;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static const char *aname[NA] = { "outlook", "temp", "humidity", "wind", "season" };

static double entropy(int pos, int n) {
    if (n == 0 || pos == 0 || pos == n) return 0.0;
    double p = (double)pos / n, q = 1.0 - p;
    return -(p * log2(p) + q * log2(q));
}
/* independent formulation with natural logs, used to cross-check the choice of split */
static double entropy_ln(int pos, int n) {
    if (n == 0 || pos == 0 || pos == n) return 0.0;
    double p = (double)pos / n, q = 1.0 - p;
    return -(p * log(p) + q * log(q)) / log(2.0);
}
static double gain(const Sample *s, int n, int attr, double (*H)(int, int)) {
    int cnt[NV] = {0}, pos[NV] = {0}, tp = 0;
    for (int i = 0; i < n; i++) { cnt[s[i].a[attr]]++; pos[s[i].a[attr]] += s[i].y; tp += s[i].y; }
    double g = H(tp, n);
    for (int v = 0; v < NV; v++) if (cnt[v]) g -= (double)cnt[v] / n * H(pos[v], cnt[v]);
    return g;
}
static DT *build(Sample *s, int n, int used, int depth, int maxdepth, int minsplit) {
    DT *t = calloc(1, sizeof *t);
    t->attr = -1; t->n = n;
    for (int i = 0; i < n; i++) t->pos += s[i].y;
    t->label = 2 * t->pos >= n && n > 0 ? 1 : 0;
    if (t->pos == 0 || t->pos == n || n < minsplit || depth >= maxdepth || used == (1 << NA) - 1) return t;
    int best = -1; double bg = 1e-9;
    for (int a = 0; a < NA; a++) {
        if (used & (1 << a)) continue;
        double g = gain(s, n, a, entropy);
        if (g > bg + 1e-9) { bg = g; best = a; } /* ties keep the lowest attribute index */
    }
    if (best < 0) return t;
    t->attr = best;
    int cnt[NV] = {0};
    for (int i = 0; i < n; i++) cnt[s[i].a[best]]++;
    for (int v = 0; v < NV; v++) {
        if (!cnt[v]) continue;
        Sample *sub = malloc(sizeof(Sample) * (size_t)cnt[v]); int m = 0;
        for (int i = 0; i < n; i++) if (s[i].a[best] == v) sub[m++] = s[i];
        t->kid[v] = build(sub, m, used | (1 << best), depth + 1, maxdepth, minsplit);
        free(sub);
    }
    return t;
}
static int predict(const DT *t, const Sample *x) {
    while (t->attr >= 0) {
        const DT *k = t->kid[x->a[t->attr]];
        if (!k) return t->label;
        t = k;
    }
    return t->label;
}
static void destroy(DT *t) { for (int v = 0; v < NV; v++) if (t->kid[v]) destroy(t->kid[v]); free(t); }
static int count_nodes(const DT *t) { int c = 1; for (int v = 0; v < NV; v++) if (t->kid[v]) c += count_nodes(t->kid[v]); return c; }
static int count_leaves(const DT *t) { if (t->attr < 0) return 1; int c = 0; for (int v = 0; v < NV; v++) if (t->kid[v]) c += count_leaves(t->kid[v]); return c; }
static int depth_of(const DT *t) { int d = 0; for (int v = 0; v < NV; v++) if (t->kid[v]) { int x = depth_of(t->kid[v]) + 1; if (x > d) d = x; } return d; }
static void print(const DT *t, int ind) {
    if (t->attr < 0) { printf("%*s-> class %d (%d/%d)\n", ind, "", t->label, t->pos, t->n); return; }
    for (int v = 0; v < NV; v++) if (t->kid[v]) {
        printf("%*s%s=%d?\n", ind, "", aname[t->attr], v);
        print(t->kid[v], ind + 2);
    }
}
/* hidden concept: (outlook==0 and humidity!=2) or wind==1 */
static int concept(const Sample *x) { return (x->a[0] == 0 && x->a[2] != 2) || x->a[3] == 1; }
static void gen(Sample *s, int n, int noise_pct) {
    for (int i = 0; i < n; i++) {
        for (int a = 0; a < NA; a++) s[i].a[a] = (int)(rnd() % NV);
        s[i].y = concept(&s[i]);
        if ((int)(rnd() % 100) < noise_pct) s[i].y ^= 1;
    }
}
static double accuracy(const DT *t, const Sample *s, int n) {
    int ok = 0;
    for (int i = 0; i < n; i++) ok += predict(t, &s[i]) == s[i].y;
    return 100.0 * ok / n;
}
static int check_leaf_purity(const DT *t) { /* returns the number of leaves that are not pure */
    if (t->attr < 0) return !(t->pos == 0 || t->pos == t->n);
    int c = 0; for (int v = 0; v < NV; v++) if (t->kid[v]) c += check_leaf_purity(t->kid[v]); return c;
}

int main(void) {
    check(fabs(entropy(1, 2) - 1.0) < 1e-12 && entropy(0, 5) == 0.0 && entropy(5, 5) == 0.0, "entropy known values");
    check(fabs(entropy(9, 14) - 0.9402859586706309) < 1e-9, "entropy of 9/14 (classic play-tennis example)");
    static Sample train[MAXS], test[MAXS];
    int noises[] = { 0, 5, 15 };
    for (int ni = 0; ni < 3; ni++) {
        int noise = noises[ni];
        gen(train, 400, noise);
        gen(test, 400, 0); /* test labels are noise free */
        /* attribute ranking at the root, both formulations must agree on the argmax */
        int best1 = 0, best2 = 0; double g1 = -1, g2 = -1;
        printf("noise %2d%%: root gains", noise);
        for (int a = 0; a < NA; a++) {
            double x = gain(train, 400, a, entropy), y = gain(train, 400, a, entropy_ln);
            check(fabs(x - y) < 1e-9, "log2 and ln formulations agree");
            if (x > g1 + 1e-9) { g1 = x; best1 = a; }
            if (y > g2 + 1e-9) { g2 = y; best2 = a; }
            printf(" %s=%.4f", aname[a], x);
        }
        printf("\n");
        check(best1 == best2, "same best attribute");
        DT *full = build(train, 400, 0, 0, NA, 2);
        DT *small = build(train, 400, 0, 0, 3, 20);
        double tr = accuracy(full, train, 400), te = accuracy(full, test, 400), te2 = accuracy(small, test, 400);
        printf("  full tree: nodes %d leaves %d depth %d, train %.1f%% test %.1f%%; pruned(depth<=3,min 20): nodes %d, test %.1f%%\n",
               count_nodes(full), count_leaves(full), depth_of(full), tr, te, count_nodes(small), te2);
        if (noise == 0) {
            check(tr == 100.0, "noise-free training data is fit exactly");
            check(check_leaf_purity(full) == 0, "leaves are pure on noise-free data");
            check(te == 100.0, "noise-free concept recovered on test data");
            print(full, 2);
        } else {
            check(te > 70.0, "reasonable accuracy under noise");
        }
        destroy(full); destroy(small);
    }
    return 0;
}
