/*
 * title: Ternary search tree for string sets
 * topic: data_structures
 * covers: ternary search tree, prefix completion, wildcard matching, hamming-neighbour search, longest prefix, insertion order and balance
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TN {
    char c;
    int end; /* 1 if a word ends here */
    struct TN *lo, *eq, *hi;
} TN;

static unsigned long long rs = 0x7E57ULL * 100003;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static TN *mk(char c) { TN *n = calloc(1, sizeof *n); n->c = c; return n; }
static int insert(TN **root, const char *w) {
    TN **p = root;
    for (;;) {
        if (!*p) *p = mk(*w);
        if (*w < (*p)->c) p = &(*p)->lo;
        else if (*w > (*p)->c) p = &(*p)->hi;
        else {
            if (!w[1]) { int fresh = !(*p)->end; (*p)->end = 1; return fresh; }
            w++; p = &(*p)->eq;
        }
    }
}
static const TN *find_node(const TN *n, const char *w) { /* node of the last char of w, or NULL */
    while (n) {
        if (*w < n->c) n = n->lo;
        else if (*w > n->c) n = n->hi;
        else { if (!w[1]) return n; w++; n = n->eq; }
    }
    return NULL;
}
static int contains(const TN *root, const char *w) { if (!*w) return 0; const TN *n = find_node(root, w); return n && n->end; }

typedef struct { char (*w)[24]; int n; } Out;
static void collect(const TN *n, char *buf, int len, Out *o) { /* in-order: lo, node, eq, hi gives sorted output */
    if (!n) return;
    collect(n->lo, buf, len, o);
    buf[len] = n->c;
    if (n->end) { buf[len + 1] = 0; strcpy(o->w[o->n++], buf); }
    collect(n->eq, buf, len + 1, o);
    collect(n->hi, buf, len, o);
}
static void with_prefix(const TN *root, const char *pre, Out *o) {
    char buf[32];
    if (!*pre) { collect(root, buf, 0, o); return; }
    const TN *n = find_node(root, pre);
    if (!n) return;
    strcpy(buf, pre);
    if (n->end) strcpy(o->w[o->n++], pre);
    collect(n->eq, buf, (int)strlen(pre), o);
}
/* '.' matches any single character; returns match count */
static int wildcard(const TN *n, const char *pat, char *buf, int len, Out *o) {
    if (!n) return 0;
    int cnt = 0;
    if (*pat == '.' || *pat < n->c) cnt += wildcard(n->lo, pat, buf, len, o);
    if (*pat == '.' || *pat == n->c) {
        buf[len] = n->c;
        if (!pat[1]) { if (n->end) { buf[len + 1] = 0; if (o) strcpy(o->w[o->n], buf); o->n++; cnt++; } }
        else cnt += wildcard(n->eq, pat + 1, buf, len + 1, o);
    }
    if (*pat == '.' || *pat > n->c) cnt += wildcard(n->hi, pat, buf, len, o);
    return cnt;
}
/* words of the same length within Hamming distance d */
static void near(const TN *n, const char *w, int d, char *buf, int len, Out *o) {
    if (!n || d < 0) return;
    near(n->lo, w, d, buf, len, o);
    near(n->hi, w, d, buf, len, o);
    buf[len] = n->c;
    int nd = d - (n->c != *w);
    if (nd < 0) return;
    if (!w[1]) { if (n->end && d >= 0) { buf[len + 1] = 0; strcpy(o->w[o->n++], buf); } return; }
    near(n->eq, w + 1, nd, buf, len + 1, o);
}
/* longest stored word that is a prefix of s */
static int longest_prefix(const TN *n, const char *s) {
    int best = 0, len = 0;
    while (n && *s) {
        if (*s < n->c) n = n->lo;
        else if (*s > n->c) n = n->hi;
        else { len++; if (n->end) best = len; s++; n = n->eq; }
    }
    return best;
}
static int height(const TN *n) { if (!n) return 0; int a = height(n->lo), b = height(n->eq), c = height(n->hi); int m = a > b ? a : b; if (c > m) m = c; return 1 + m; }
static int nodes(const TN *n) { return n ? 1 + nodes(n->lo) + nodes(n->eq) + nodes(n->hi) : 0; }
static void destroy(TN *n) { if (n) { destroy(n->lo); destroy(n->eq); destroy(n->hi); free(n); } }
static int cmp_w(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int wmatch(const char *w, const char *pat) { if (strlen(w) != strlen(pat)) return 0; for (int i = 0; pat[i]; i++) if (pat[i] != '.' && pat[i] != w[i]) return 0; return 1; }
/* median-balanced build from a sorted list, to compare tree height with random and sorted insertion */
static void insert_balanced(TN **root, char (*w)[24], int lo, int hi) {
    if (lo >= hi) return;
    int mid = (lo + hi) / 2;
    insert(root, w[mid]);
    insert_balanced(root, w, lo, mid);
    insert_balanced(root, w, mid + 1, hi);
}

int main(void) {
    static char words[1500][24]; int nw = 0;
    static const char *seedwords[] = { "tree", "trie", "try", "trip", "triple", "tribe", "tea", "ten", "team", "tent", "to", "toe", "top", "topic", "a", "an", "and", "ant", "any", "banana", "band", "bandana", "ban" };
    for (unsigned i = 0; i < sizeof seedwords / sizeof seedwords[0]; i++) strcpy(words[nw++], seedwords[i]);
    while (nw < 1500) {
        int len = 2 + (int)(rnd() % 7);
        for (int i = 0; i < len; i++) words[nw][i] = (char)('a' + rnd() % 6);
        words[nw][len] = 0; nw++;
    }
    TN *root = NULL;
    static char uniq[1500][24]; int nu = 0;
    for (int i = 0; i < nw; i++) {
        int f = insert(&root, words[i]);
        int dup = 0; for (int j = 0; j < nu && !dup; j++) if (!strcmp(uniq[j], words[i])) dup = 1;
        check(f == !dup, "insert reports freshness");
        if (f) strcpy(uniq[nu++], words[i]);
    }
    qsort(uniq, (size_t)nu, 24, cmp_w);
    for (int i = 0; i < nu; i++) check(contains(root, uniq[i]), "member present");
    check(!contains(root, "zzz") && !contains(root, "") && !contains(root, "tre") , "non-members absent");
    /* full traversal is sorted */
    static char all[1500][24]; Out o = { all, 0 };
    with_prefix(root, "", &o);
    check(o.n == nu, "traversal count");
    for (int i = 0; i < nu; i++) check(!strcmp(all[i], uniq[i]), "traversal is lexicographic");
    printf("words inserted %d, distinct %d, nodes %d, height %d\n", nw, nu, nodes(root), height(root));
    /* prefix completion */
    const char *prefs[] = { "tr", "ban", "a", "ab", "fff", "top", "e" };
    for (unsigned p = 0; p < sizeof prefs / sizeof prefs[0]; p++) {
        static char res[1500][24]; Out r = { res, 0 };
        with_prefix(root, prefs[p], &r);
        int want = 0;
        for (int i = 0; i < nu; i++) if (!strncmp(uniq[i], prefs[p], strlen(prefs[p]))) { check(want < r.n && !strcmp(res[want], uniq[i]), "prefix result order"); want++; }
        check(want == r.n, "prefix count");
        printf("prefix %-4s -> %3d words:", prefs[p], r.n);
        for (int i = 0; i < r.n && i < 5; i++) printf(" %s", res[i]);
        printf("%s\n", r.n > 5 ? " ..." : "");
    }
    /* wildcard patterns */
    const char *pats[] = { "t...", "..", "b.n", ".a.a.", "....", "a", ".", "ab.d" };
    for (unsigned p = 0; p < sizeof pats / sizeof pats[0]; p++) {
        static char res[1500][24]; Out r = { res, 0 }; char buf[32];
        int c = wildcard(root, pats[p], buf, 0, &r);
        int want = 0;
        for (int i = 0; i < nu; i++) if (wmatch(uniq[i], pats[p])) want++;
        check(c == want && r.n == want, "wildcard count");
        printf("pattern %-6s -> %3d matches\n", pats[p], c);
    }
    /* Hamming neighbours */
    long tot = 0;
    for (int q = 0; q < 60; q++) {
        char w[24]; int len = 3 + (int)(rnd() % 4);
        for (int i = 0; i < len; i++) w[i] = (char)('a' + rnd() % 6);
        w[len] = 0;
        int d = (int)(rnd() % 3);
        static char res[1500][24]; Out r = { res, 0 }; char buf[32];
        near(root, w, d, buf, 0, &r);
        int want = 0;
        for (int i = 0; i < nu; i++) if (strlen(uniq[i]) == (size_t)len) { int h = 0; for (int k = 0; k < len; k++) h += uniq[i][k] != w[k]; if (h <= d) want++; }
        check(r.n == want, "hamming neighbours");
        tot += r.n;
    }
    printf("60 hamming-neighbour queries, %ld results in total\n", tot);
    /* longest stored prefix of a text */
    check(longest_prefix(root, "triplet") == 6 && longest_prefix(root, "trap") == 0 && longest_prefix(root, "toes") == 3, "longest prefix");
    { int lp = longest_prefix(root, "bandanas"); check(lp == 7, "longest prefix bandana"); printf("longest stored prefix of bandanas: %d chars\n", lp); }
    /* balance versus insertion order, on the same distinct set */
    TN *sorted_t = NULL, *bal = NULL, *rev = NULL;
    for (int i = 0; i < nu; i++) insert(&sorted_t, uniq[i]);
    for (int i = nu - 1; i >= 0; i--) insert(&rev, uniq[i]);
    insert_balanced(&bal, uniq, 0, nu);
    check(nodes(sorted_t) == nodes(rev) && nodes(rev) == nodes(bal), "node count is independent of insertion order");
    printf("height: random order %d, sorted order %d, reverse sorted %d, median-first %d; node counts %d %d %d %d\n",
           height(root), height(sorted_t), height(rev), height(bal), nodes(root), nodes(sorted_t), nodes(rev), nodes(bal));
    destroy(root); destroy(sorted_t); destroy(rev); destroy(bal);
    return 0;
}
