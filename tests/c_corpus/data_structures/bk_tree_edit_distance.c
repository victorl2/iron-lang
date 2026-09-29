/*
 * title: BK-tree for edit-distance lookup
 * topic: data_structures
 * covers: bk-tree, levenshtein distance, metric tree, triangle-inequality pruning, range search, duplicates
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXW 400
#define MAXD 16

typedef struct BK {
    char *word;
    struct BK *child[MAXD];
} BK;

static unsigned long long rs = 0xBEEF00D5ULL * 4099;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static long dcalls;
static int lev(const char *a, const char *b) {
    dcalls++;
    int n = (int)strlen(a), m = (int)strlen(b);
    int prev[32], cur[32];
    for (int j = 0; j <= m; j++) prev[j] = j;
    for (int i = 1; i <= n; i++) {
        cur[0] = i;
        for (int j = 1; j <= m; j++) {
            int c = prev[j - 1] + (a[i - 1] != b[j - 1]);
            if (prev[j] + 1 < c) c = prev[j] + 1;
            if (cur[j - 1] + 1 < c) c = cur[j - 1] + 1;
            cur[j] = c;
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(m + 1));
    }
    return prev[m];
}
static int insert(BK **root, const char *w) {
    BK **slot = root;
    while (*slot) {
        int d = lev((*slot)->word, w);
        if (d == 0) return 0;
        if (d >= MAXD) d = MAXD - 1;
        slot = &(*slot)->child[d];
    }
    *slot = calloc(1, sizeof **slot);
    (*slot)->word = strdup(w);
    return 1;
}
static void search(const BK *n, const char *q, int r, const char **out, int *cnt) {
    if (!n) return;
    int d = lev(n->word, q);
    if (d <= r) out[(*cnt)++] = n->word;
    int lo = d - r < 1 ? 1 : d - r, hi = d + r >= MAXD ? MAXD - 1 : d + r;
    for (int k = lo; k <= hi; k++) search(n->child[k], q, r, out, cnt);
}
static void destroy(BK *n) { if (!n) return; for (int i = 0; i < MAXD; i++) destroy(n->child[i]); free(n->word); free(n); }
static int height(const BK *n) { int h = 0; if (!n) return 0; for (int i = 0; i < MAXD; i++) { int c = height(n->child[i]); if (c > h) h = c; } return h + 1; }
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void rand_word(char *o, int minl, int maxl) {
    int len = minl + (int)(rnd() % (unsigned)(maxl - minl + 1));
    for (int i = 0; i < len; i++) o[i] = (char)('a' + rnd() % 5);
    o[len] = 0;
}
/* perturb a word with k random edits */
static void mutate(char *w, int k) {
    for (int e = 0; e < k; e++) {
        int len = (int)strlen(w), op = (int)(rnd() % 3);
        if (op == 0 && len > 1) { int p = (int)(rnd() % (unsigned)len); memmove(w + p, w + p + 1, (size_t)(len - p)); }
        else if (op == 1 && len < 11) { int p = (int)(rnd() % (unsigned)(len + 1)); memmove(w + p + 1, w + p, (size_t)(len - p + 1)); w[p] = (char)('a' + rnd() % 5); }
        else if (len > 0) { w[rnd() % (unsigned)len] = (char)('a' + rnd() % 5); }
    }
}

int main(void) {
    /* known distances */
    check(lev("kitten", "sitting") == 3 && lev("", "abc") == 3 && lev("flaw", "lawn") == 2 && lev("same", "same") == 0, "levenshtein known values");
    BK *root = NULL;
    static char words[MAXW][16];
    int nw = 0, dups = 0;
    for (int i = 0; i < 350; i++) {
        char w[16];
        if (nw > 5 && i % 3 == 0) { strcpy(w, words[rnd() % (unsigned)nw]); mutate(w, 1 + (int)(rnd() % 2)); }
        else rand_word(w, 3, 9);
        if (insert(&root, w)) strcpy(words[nw++], w); else dups++;
    }
    long build_calls = dcalls;
    printf("words %d duplicates skipped %d, tree height %d, distance calls while building %ld\n", nw, dups, height(root), build_calls);
    for (int r = 0; r <= 3; r++) {
        long tree_calls = 0, brute_calls = 0; int results = 0, empty = 0;
        for (int q = 0; q < 60; q++) {
            char qw[16];
            if (q % 4 == 0) rand_word(qw, 3, 9); else { strcpy(qw, words[rnd() % (unsigned)nw]); mutate(qw, (int)(rnd() % 3)); }
            const char *got[MAXW]; int ng = 0;
            dcalls = 0;
            search(root, qw, r, got, &ng);
            tree_calls += dcalls;
            /* brute force */
            const char *want[MAXW]; int nb = 0;
            dcalls = 0;
            for (int i = 0; i < nw; i++) if (lev(words[i], qw) <= r) want[nb++] = words[i];
            brute_calls += dcalls;
            check(ng == nb, "result count equals brute force");
            qsort(got, (size_t)ng, sizeof *got, cmp);
            qsort(want, (size_t)nb, sizeof *want, cmp);
            for (int i = 0; i < ng; i++) check(strcmp(got[i], want[i]) == 0, "same result set");
            results += ng; empty += ng == 0;
        }
        printf("radius %d: 60 queries, %d results (%d empty), tree distance calls %ld vs brute %ld (%.1f%%)\n",
               r, results, empty, tree_calls, brute_calls, 100.0 * (double)tree_calls / (double)brute_calls);
    }
    /* nearest neighbour by increasing radius */
    int agree = 0;
    for (int q = 0; q < 40; q++) {
        char qw[16]; rand_word(qw, 4, 8);
        int bestd = 99;
        for (int i = 0; i < nw; i++) { int d = lev(words[i], qw); if (d < bestd) bestd = d; }
        int r = 0; const char *got[MAXW]; int ng = 0;
        for (;;) { ng = 0; search(root, qw, r, got, &ng); if (ng) break; r++; }
        check(r == bestd, "nearest by widening radius");
        agree++;
    }
    printf("nearest-neighbour searches agreeing with brute force: %d\n", agree);
    destroy(root);
    return 0;
}
