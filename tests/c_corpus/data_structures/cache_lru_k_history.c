/*
 * title: LRU-K cache with retained reference history and an indexed heap
 * topic: data_structures
 * covers: LRU-K, backward K-distance, reference history ring per key, indexed binary heap with key updates, retained information after eviction, linear-scan oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C 24
#define KEYS 300
#define MAXK 3

static unsigned long long rs = 0x1A2B3C4DULL * 0x9E37ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* history[k][0] = most recent reference time ... history[k][K-1] = K-th most recent (0 = none) */
static long history[KEYS][MAXK];
static int K;
static long clk;

/* eviction priority: smaller evicts first. Pages with fewer than K references have kth == 0. */
static int prio_less(int a, int b) {
    long ka = history[a][K - 1], kb = history[b][K - 1];
    if (ka != kb) return ka < kb;
    if (history[a][0] != history[b][0]) return history[a][0] < history[b][0];
    return a < b;
}
static void record(int k) {
    for (int i = K - 1; i > 0; i--) history[k][i] = history[k][i - 1];
    history[k][0] = ++clk;
}

/* ---- fast: indexed heap over resident keys ---- */
static int heap[C], hpos[KEYS], hn;
static void sift_up(int i) {
    while (i > 0) { int p = (i - 1) / 2; if (!prio_less(heap[i], heap[p])) break; int t = heap[i]; heap[i] = heap[p]; heap[p] = t; hpos[heap[i]] = i; hpos[heap[p]] = p; i = p; }
}
static void sift_down(int i) {
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < hn && prio_less(heap[l], heap[m])) m = l;
        if (r < hn && prio_less(heap[r], heap[m])) m = r;
        if (m == i) break;
        int t = heap[i]; heap[i] = heap[m]; heap[m] = t; hpos[heap[i]] = i; hpos[heap[m]] = m; i = m;
    }
}
static int access_fast(int k) {
    if (hpos[k] >= 0) { record(k); sift_down(hpos[k]); sift_up(hpos[k]); return 1; }
    if (hn == C) {
        int v = heap[0]; hpos[v] = -1;
        heap[0] = heap[--hn]; hpos[heap[0]] = 0; sift_down(0);
    }
    record(k); heap[hn] = k; hpos[k] = hn; hn++; sift_up(hn - 1);
    return 0;
}

/* ---- oracle: resident flag array and a full scan for the victim, own copy of history ---- */
static long h2[KEYS][MAXK]; static int res[KEYS], rn; static long clk2;
static int less2(int a, int b) {
    long ka = h2[a][K - 1], kb = h2[b][K - 1];
    if (ka != kb) return ka < kb;
    if (h2[a][0] != h2[b][0]) return h2[a][0] < h2[b][0];
    return a < b;
}
static void record2(int k) { for (int i = K - 1; i > 0; i--) h2[k][i] = h2[k][i - 1]; h2[k][0] = ++clk2; }
static int access_ref(int k) {
    if (res[k]) { record2(k); return 1; }
    if (rn == C) {
        int v = -1;
        for (int i = 0; i < KEYS; i++) if (res[i] && (v < 0 || less2(i, v))) v = i;
        res[v] = 0; rn--;
    }
    record2(k); res[k] = 1; rn++;
    return 0;
}

static int gen(int phase, int *scan) {
    unsigned r = rnd() % 100;
    if (phase == 0) return r < 60 ? (int)(rnd() % 16) : 16 + (int)(rnd() % 200);
    if (phase == 1) return r < 40 ? (int)(rnd() % 12) : 100 + ((*scan)++ % 190);     /* hot set plus sequential scan */
    return (int)(rnd() % 40);
}

int main(void) {
    printf("%-6s %8s %8s %8s\n", "K", "skewed", "scan", "small");
    for (K = 1; K <= 3; K++) {
        memset(history, 0, sizeof history); memset(h2, 0, sizeof h2); memset(res, 0, sizeof res);
        clk = clk2 = 0; hn = rn = 0;
        for (int i = 0; i < KEYS; i++) hpos[i] = -1;
        rs = 0x1A2B3C4DULL * 0x9E37ULL * 5;
        long hits[3] = {0, 0, 0}; int scan = 0;
        for (int phase = 0; phase < 3; phase++) {
            for (int i = 0; i < 12000; i++) {
                int k = gen(phase, &scan);
                int a = access_fast(k), b = access_ref(k);
                check(a == b, "LRU-K agrees with scan oracle");
                hits[phase] += a;
            }
        }
        for (int i = 0; i < KEYS; i++) check((hpos[i] >= 0) == res[i], "resident sets equal");
        for (int i = 0; i < hn; i++) { int l = 2 * i + 1, r = 2 * i + 2; check(l >= hn || !prio_less(heap[l], heap[i]), "heap order left"); check(r >= hn || !prio_less(heap[r], heap[i]), "heap order right"); }
        printf("LRU-%d  %8ld %8ld %8ld\n", K, hits[0], hits[1], hits[2]);
    }
    return 0;
}
