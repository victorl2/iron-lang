/*
 * title: Catalan structures: Dyck words, ranking and binary trees
 * topic: algorithms
 * covers: Dyck path enumeration, ballot table ranking, unranking, Catalan bijection with binary trees, parenthesizations, stack-sortable permutations
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

#define MAXN 16
/* ways[i][h]: number of ways to finish with i pairs remaining characters left and height h */
static u64 ways[2 * MAXN + 2][MAXN + 2];
static u64 cat[MAXN + 1];

static void build(void) {
    memset(ways, 0, sizeof ways);
    ways[0][0] = 1;
    for (int len = 1; len <= 2 * MAXN; len++)
        for (int h = 0; h <= MAXN; h++) {
            u64 v = ways[len - 1][h + 1];               /* place ')' , height becomes h+1 -> h ... */
            if (h > 0) v += ways[len - 1][h - 1];       /* place '(' */
            ways[len][h] = v;
        }
    cat[0] = 1;
    for (int n = 1; n <= MAXN; n++) {
        cat[n] = 0;
        for (int i = 0; i < n; i++) cat[n] += cat[i] * cat[n - 1 - i];
    }
}

/* rank of a Dyck word among all Dyck words of the same length in order '(' < ')' */
static u64 dyck_rank(const char *w, int n) {
    u64 r = 0;
    int h = 0, len = 2 * n;
    for (int i = 0; i < len; i++) {
        int rem = len - i - 1;
        if (w[i] == ')') {
            /* words with '(' here come first */
            r += ways[rem][h + 1];
            h--;
        } else h++;
    }
    return r;
}

static void dyck_unrank(u64 r, int n, char *w) {
    int h = 0, len = 2 * n;
    for (int i = 0; i < len; i++) {
        int rem = len - i - 1;
        u64 open_cnt = ways[rem][h + 1];
        if (r < open_cnt) { w[i] = '('; h++; }
        else { r -= open_cnt; w[i] = ')'; h--; }
    }
    w[len] = 0;
}

typedef struct Node { struct Node *l, *r; } Node;

static Node *tree_from_dyck(const char *w, int *pos) {
    if (w[*pos] != '(') return NULL;
    (*pos)++;
    Node *nd = malloc(sizeof *nd);
    if (!nd) exit(2);
    nd->l = tree_from_dyck(w, pos);
    (*pos)++; /* ')' */
    nd->r = tree_from_dyck(w, pos);
    return nd;
}
static int height(const Node *t) { if (!t) return 0; int a = height(t->l), b = height(t->r); return 1 + (a > b ? a : b); }
static int size(const Node *t) { return t ? 1 + size(t->l) + size(t->r) : 0; }
static void destroy(Node *t) { if (t) { destroy(t->l); destroy(t->r); free(t); } }

/* stack-sortable check: 231-avoiding, simulated with a stack */
static int stack_sortable(const int *p, int n) {
    int stack[16], sp = 0, next = 1;
    for (int i = 0; i < n; i++) {
        while (sp && stack[sp - 1] == next) { sp--; next++; }
        if (sp && stack[sp - 1] < p[i]) return 0;
        stack[sp++] = p[i];
    }
    return 1;
}

static int next_perm(int *a, int n) {
    int i = n - 2;
    while (i >= 0 && a[i] >= a[i + 1]) i--;
    if (i < 0) return 0;
    int j = n - 1;
    while (a[j] <= a[i]) j--;
    int t = a[i]; a[i] = a[j]; a[j] = t;
    for (int lo = i + 1, hi = n - 1; lo < hi; lo++, hi--) { t = a[lo]; a[lo] = a[hi]; a[hi] = t; }
    return 1;
}

int main(void) {
    build();
    printf("Catalan:");
    for (int n = 0; n <= 12; n++) printf(" %llu", cat[n]);
    printf("\n");
    for (int n = 0; n <= MAXN; n++) if (ways[2 * n][0] != cat[n]) { fprintf(stderr, "ballot table != catalan\n"); return 1; }
    char w[2 * MAXN + 1];
    printf("Dyck words n=3:");
    for (u64 r = 0; r < cat[3]; r++) { dyck_unrank(r, 3, w); printf(" %s", w); }
    printf("\n");
    for (int n = 1; n <= 9; n++) {
        char prev[2 * MAXN + 1] = "";
        for (u64 r = 0; r < cat[n]; r++) {
            dyck_unrank(r, n, w);
            if (dyck_rank(w, n) != r) { fprintf(stderr, "rank mismatch\n"); return 1; }
            if (r && strcmp(prev, w) >= 0) { fprintf(stderr, "not sorted\n"); return 1; }
            int h = 0;
            for (int i = 0; w[i]; i++) { h += w[i] == '(' ? 1 : -1; if (h < 0) return 1; }
            if (h) return 1;
            strcpy(prev, w);
        }
    }
    printf("n<=9: all Dyck words round-trip through rank/unrank in sorted order\n");
    dyck_unrank(cat[16] - 1, 16, w);
    printf("last Dyck word n=16: %s\n", w);
    dyck_unrank(cat[16] / 3, 16, w);
    printf("word #%llu n=16: %s\n", cat[16] / 3, w);

    /* binary trees via bijection: size and height distribution for n=8 */
    int hist[10] = {0};
    for (u64 r = 0; r < cat[8]; r++) {
        dyck_unrank(r, 8, w);
        int pos = 0;
        Node *t = tree_from_dyck(w, &pos);
        if (size(t) != 8 || pos != 16) { fprintf(stderr, "tree shape\n"); return 1; }
        hist[height(t)]++;
        destroy(t);
    }
    printf("binary trees with 8 nodes by height:");
    for (int h = 1; h <= 8; h++) printf(" h%d:%d", h, hist[h]);
    printf("\n");
    /* 231-avoiding permutations counted by Catalan too */
    for (int n = 1; n <= 8; n++) {
        int p[8];
        for (int i = 0; i < n; i++) p[i] = i + 1;
        u64 c = 0;
        do c += (u64)stack_sortable(p, n); while (next_perm(p, n));
        if (c != cat[n]) { fprintf(stderr, "sortable count %llu != %llu\n", c, cat[n]); return 1; }
    }
    printf("stack-sortable permutations counted for n=1..8 match Catalan\n");
    /* number of full parenthesizations of a product of n+1 factors */
    printf("parenthesizations of 6 factors: %llu\n", cat[5]);
    return 0;
}
