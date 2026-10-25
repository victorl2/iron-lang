/*
 * title: Winner tree and loser tree k-way merge
 * topic: data_structures
 * covers: tournament tree, winner tree, loser tree, k-way merge, comparison counting, replay path
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXK 32
#define INF 0x7fffffff

typedef struct {
    int *data;
    int len, pos;
} Run;

static unsigned long long rs = 0x123456789ABCDEFULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static long cmps;
/* key of run i's current head; INF when exhausted. Ties broken by run index for stability */
static int head(const Run *r, int i) { return r[i].pos < r[i].len ? r[i].data[r[i].pos] : INF; }
static int beats(const Run *r, int a, int b) { /* a wins (is smaller) over b */
    cmps++;
    int ka = head(r, a), kb = head(r, b);
    return ka != kb ? ka < kb : a < b;
}

/* ---- winner tree: internal node holds the winner's run index ---- */
static int wt[2 * MAXK];
static int wk; /* leaves count, power of two */
static void wt_build(const Run *r, int k) {
    wk = 1; while (wk < k) wk <<= 1;
    for (int i = 0; i < wk; i++) wt[wk + i] = i < k ? i : -1;
    for (int i = wk - 1; i >= 1; i--) {
        int a = wt[2 * i], b = wt[2 * i + 1];
        if (b < 0) wt[i] = a; else if (a < 0) wt[i] = b; else wt[i] = beats(r, a, b) ? a : b;
    }
}
static void wt_replay(const Run *r, int leaf) {
    for (int i = (wk + leaf) / 2; i >= 1; i /= 2) {
        int a = wt[2 * i], b = wt[2 * i + 1];
        if (b < 0) wt[i] = a; else if (a < 0) wt[i] = b; else wt[i] = beats(r, a, b) ? a : b;
    }
}
static int merge_winner(Run *r, int k, int *out) {
    for (int i = 0; i < k; i++) r[i].pos = 0;
    wt_build(r, k);
    int n = 0;
    for (;;) {
        int w = wt[1];
        if (head(r, w) == INF) break;
        out[n++] = head(r, w);
        r[w].pos++;
        wt_replay(r, w);
    }
    return n;
}

/* ---- loser tree: internal nodes hold losers, ls[0] holds overall winner ---- */
static int ls[MAXK];
static int lk;
static int lt_winner_of(const Run *r, int node) {
    /* recursive helper for build: returns winner of subtree and stores loser */
    if (node >= lk) { int idx = node - lk; return idx < 0 ? -1 : idx; }
    int a = lt_winner_of(r, 2 * node), b = lt_winner_of(r, 2 * node + 1);
    int win, lose;
    if (a >= 0 && b >= 0 && a < MAXK * 2 && b < MAXK * 2) {
        if (beats(r, a, b)) { win = a; lose = b; } else { win = b; lose = a; }
    } else { win = a >= 0 ? a : b; lose = a >= 0 ? b : a; }
    ls[node] = lose;
    return win;
}
static int merge_loser(Run *r, int k, int *out, int *padded_out_k) {
    lk = 1; while (lk < k) lk <<= 1;
    /* pad with virtual exhausted runs: use dummy runs beyond k as index >= k, head() must be INF */
    static Run pad[MAXK];
    for (int i = 0; i < lk; i++) {
        if (i < k) { r[i].pos = 0; pad[i] = r[i]; }
        else { pad[i].data = NULL; pad[i].len = 0; pad[i].pos = 0; }
    }
    *padded_out_k = lk;
    int win = lt_winner_of(pad, 1);
    int n = 0;
    for (;;) {
        if (head(pad, win) == INF) break;
        out[n++] = head(pad, win);
        pad[win].pos++;
        /* replay from leaf to root: only compare against the stored loser */
        int cand = win;
        for (int node = (lk + win) / 2; node >= 1; node /= 2) {
            if (beats(pad, ls[node], cand)) { int t = ls[node]; ls[node] = cand; cand = t; }
        }
        win = cand;
    }
    return n;
}

static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }

int main(void) {
    int ks[] = { 1, 2, 3, 5, 8, 13, 16, 20 };
    for (unsigned ki = 0; ki < sizeof ks / sizeof ks[0]; ki++) {
        int k = ks[ki];
        Run runs[MAXK];
        int total = 0;
        for (int i = 0; i < k; i++) {
            runs[i].len = (int)(rnd() % 60) + (i == 2 ? 0 : 1);
            if (i == 1) runs[i].len = 0;
            if (k == 1) runs[i].len = 40;
            runs[i].data = malloc(sizeof(int) * (size_t)(runs[i].len + 1));
            int v = (int)(rnd() % 10);
            for (int j = 0; j < runs[i].len; j++) { runs[i].data[j] = v; v += (int)(rnd() % 7); }
            runs[i].pos = 0;
            total += runs[i].len;
        }
        int *all = malloc(sizeof(int) * (size_t)(total + 1));
        int n = 0;
        for (int i = 0; i < k; i++) for (int j = 0; j < runs[i].len; j++) all[n++] = runs[i].data[j];
        qsort(all, (size_t)n, sizeof(int), cmp_int);
        int *o1 = malloc(sizeof(int) * (size_t)(total + 1)), *o2 = malloc(sizeof(int) * (size_t)(total + 1));
        cmps = 0;
        int n1 = merge_winner(runs, k, o1);
        long c1 = cmps;
        cmps = 0;
        int pk;
        int n2 = merge_loser(runs, k, o2, &pk);
        long c2 = cmps;
        check(n1 == total && n2 == total, "merged length");
        check(memcmp(o1, all, sizeof(int) * (size_t)total) == 0, "winner tree merge sorted");
        check(memcmp(o2, all, sizeof(int) * (size_t)total) == 0, "loser tree merge sorted");
        /* naive linear-scan merge as third reference */
        long c3 = 0;
        for (int i = 0; i < k; i++) runs[i].pos = 0;
        for (int m = 0; m < total; m++) {
            int b = -1;
            for (int i = 0; i < k; i++) {
                if (runs[i].pos >= runs[i].len) continue;
                if (b < 0) b = i; else { c3++; if (runs[i].data[runs[i].pos] < runs[b].data[runs[b].pos]) b = i; }
            }
            check(runs[b].data[runs[b].pos] == all[m], "linear merge");
            runs[b].pos++;
        }
        printf("k=%2d (tree leaves %2d) total=%4d  cmps: winner=%5ld loser=%5ld linear=%5ld  first=%d last=%d\n",
               k, pk, total, c1, c2, c3, total ? all[0] : -1, total ? all[total - 1] : -1);
        int lg = 0; while ((1 << lg) < pk) lg++;
        check(c2 <= (pk - 1) + (long)(total + 1) * lg, "loser tree comparison bound");
        check(c1 <= (pk - 1) + (long)(total + 1) * lg, "winner tree comparison bound");
        for (int i = 0; i < k; i++) free(runs[i].data);
        free(all); free(o1); free(o2);
    }
    return 0;
}
