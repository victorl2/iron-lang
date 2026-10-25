/*
 * title: Suffix tree by Ukkonen's algorithm
 * topic: data_structures
 * covers: suffix tree, ukkonen online construction, suffix links, substring counting, longest repeated substring
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AL 27
#define MAXS 700
#define INF 1000000

typedef struct { int start, end, link, next[AL]; } Node;
static Node nd[2 * MAXS + 4];
static int nn;
static char S[MAXS];
static int slen;

static unsigned long long rs = 0x1234ABCD9876FEDULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static int ci(char c) { return c == '$' ? 26 : c - 'a'; }
static int newnode(int start, int end) {
    Node *n = &nd[nn];
    n->start = start; n->end = end; n->link = 0;
    for (int i = 0; i < AL; i++) n->next[i] = 0;
    return nn++;
}
static int elen(int v, int pos) { int e = nd[v].end == INF ? pos : nd[v].end; return e - nd[v].start + 1; }

static void build(const char *str) {
    slen = (int)strlen(str);
    memcpy(S, str, (size_t)slen + 1);
    nn = 0;
    int root = newnode(-1, -1);
    int an = root, ae = 0, al = 0, rem = 0;
    for (int i = 0; i < slen; i++) {
        int last = 0;
        rem++;
        while (rem > 0) {
            if (al == 0) ae = i;
            int c = ci(S[ae]);
            int nx = nd[an].next[c];
            if (!nx) {
                int leaf = newnode(i, INF);
                nd[an].next[c] = leaf;
                if (last) { nd[last].link = an; last = 0; }
            } else {
                int el = elen(nx, i);
                if (al >= el) { ae += el; al -= el; an = nx; continue; }
                if (S[nd[nx].start + al] == S[i]) {
                    if (last && an != root) { nd[last].link = an; last = 0; }
                    al++;
                    break;
                }
                int sp = newnode(nd[nx].start, nd[nx].start + al - 1);
                nd[an].next[c] = sp;
                int leaf = newnode(i, INF);
                nd[sp].next[ci(S[i])] = leaf;
                nd[nx].start += al;
                nd[sp].next[ci(S[nd[nx].start])] = nx;
                if (last) nd[last].link = sp;
                last = sp;
            }
            rem--;
            if (an == root && al > 0) { al--; ae = i - rem + 1; }
            else if (an != root) an = nd[an].link;
        }
    }
}

static int leafcnt[2 * MAXS + 4], depth_of[2 * MAXS + 4];
static long distinct_sum;
static int lrs_depth, internal_nodes, leaves;
static void dfs(int v, int depth) {
    depth_of[v] = depth;
    int kids = 0, cnt = 0;
    for (int c = 0; c < AL; c++) {
        int w = nd[v].next[c];
        if (!w) continue;
        kids++;
        int el = elen(w, slen - 1);
        distinct_sum += el;
        dfs(w, depth + el);
        cnt += leafcnt[w];
    }
    if (!kids) { leafcnt[v] = 1; leaves++; }
    else {
        leafcnt[v] = cnt;
        if (v) { internal_nodes++; check(kids >= 2, "internal nodes branch"); if (depth > lrs_depth) { lrs_depth = depth; } }
    }
}
/* node at end of matching pattern; returns leaf count, 0 if absent */
static int count_occ(const char *p) {
    int v = 0, i = 0, m = (int)strlen(p);
    while (i < m) {
        int w = nd[v].next[ci(p[i])];
        if (!w) return 0;
        int el = elen(w, slen - 1);
        for (int k = 0; k < el && i < m; k++, i++) if (S[nd[w].start + k] != p[i]) return 0;
        v = w;
    }
    return leafcnt[v];
}
static int cmp_suf(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int brute_occ(const char *s, const char *p) {
    int c = 0, m = (int)strlen(p), n = (int)strlen(s);
    for (int i = 0; i + m <= n; i++) if (strncmp(s + i, p, (size_t)m) == 0) c++;
    return c;
}

static void run(const char *name, const char *text) {
    char buf[MAXS];
    snprintf(buf, sizeof buf, "%s$", text);
    build(buf);
    int n = slen; /* includes $ */
    check(nn <= 2 * n + 1, "node bound");
    distinct_sum = 0; lrs_depth = 0; internal_nodes = 0; leaves = 0;
    dfs(0, 0);
    check(leaves == n, "one leaf per suffix");
    /* brute force: sorted suffixes, distinct = sum len - sum lcp */
    char *suf[MAXS];
    for (int i = 0; i < n; i++) suf[i] = buf + i;
    qsort(suf, (size_t)n, sizeof *suf, cmp_suf);
    long total = 0, lcp_sum = 0; int best_lcp = 0;
    for (int i = 0; i < n; i++) {
        total += (long)strlen(suf[i]);
        if (i) { int l = 0; while (suf[i][l] && suf[i][l] == suf[i - 1][l]) l++; lcp_sum += l; if (l > best_lcp) best_lcp = l; }
    }
    check(distinct_sum == total - lcp_sum, "distinct substring count");
    check(lrs_depth == best_lcp, "longest repeated substring depth");
    /* every suffix is spelled to a leaf whose depth = suffix length */
    for (int i = 0; i < n; i++) {
        int v = 0, pos = i;
        while (pos < n) {
            int w = nd[v].next[ci(buf[pos])];
            check(w != 0, "suffix path exists");
            int el = elen(w, n - 1);
            for (int k = 0; k < el; k++) check(S[nd[w].start + k] == buf[pos + k], "edge label matches");
            pos += el; v = w;
        }
        check(pos == n && depth_of[v] == n - i && leafcnt[v] == 1, "suffix ends at leaf");
    }
    /* suffix links: label(link(v)) == label(v) minus first char */
    int linked = 0;
    for (int v = 1; v < nn; v++) {
        if (nd[v].end == INF) continue;
        int kids = 0; for (int c = 0; c < AL; c++) kids += nd[v].next[c] != 0;
        if (!kids) continue;
        int L = nd[v].link;
        check(depth_of[L] == depth_of[v] - 1, "suffix link depth");
        linked++;
    }
    /* pattern queries */
    int qs = 0, hit = 0;
    for (int q = 0; q < 60; q++) {
        char p[8]; int st = (int)(rnd() % (unsigned)(n - 1)), len = 1 + (int)(rnd() % 5);
        if (q % 2) { for (int k = 0; k < len; k++) p[k] = (char)('a' + rnd() % 3); }
        else { for (int k = 0; k < len; k++) p[k] = buf[(st + k) % (n - 1)]; }
        p[len] = 0;
        int a = count_occ(p), b = brute_occ(text, p);
        check(a == b, "occurrence count vs brute force");
        qs++; hit += a > 0;
    }
    printf("%-9s n=%3d nodes=%3d internal=%3d distinct=%4ld longest-repeat=%d links=%d queries=%d/%d hit\n",
           name, n - 1, nn, internal_nodes, distinct_sum - n, lrs_depth, linked, hit, qs);
}

int main(void) {
    run("banana", "banana");
    run("mississ", "mississippi");
    run("aaaaaa", "aaaaaaaaaaaa");
    run("abab", "abababababab");
    run("xabxac", "xabxacxabxab");
    run("abcabx", "abcabxabcd");
    for (int t = 0; t < 4; t++) {
        char s[MAXS - 2]; int n = 20 + t * 90;
        int alpha = 2 + t;
        for (int i = 0; i < n; i++) s[i] = (char)('a' + rnd() % (unsigned)alpha);
        s[n] = 0;
        char name[16]; snprintf(name, sizeof name, "rand%d", alpha);
        run(name, s);
    }
    return 0;
}
