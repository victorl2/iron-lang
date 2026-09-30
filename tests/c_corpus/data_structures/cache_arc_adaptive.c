/*
 * title: ARC adaptive replacement cache with ghost lists
 * topic: data_structures
 * covers: ARC, T1/T2/B1/B2 lists, adaptive target p, ghost hits, key-indexed linked lists, array-list oracle, scan resistance vs LRU
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C 24
#define KEYS 400

static unsigned long long rs = 0xA2C0FFEE11ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

enum { NONE, T1, T2, B1, B2 };

/* ---- fast version: one doubly linked list per class threaded through key-indexed arrays ---- */
static int where_[KEYS], pv[KEYS], nx[KEYS], hd[5], tl[5], len[5], p;
static void lpush(int l, int k) {   /* MRU at head */
    pv[k] = -1; nx[k] = hd[l];
    if (hd[l] >= 0) pv[hd[l]] = k; else tl[l] = k;
    hd[l] = k; where_[k] = l; len[l]++;
}
static void lrem(int k) {
    int l = where_[k];
    if (pv[k] >= 0) nx[pv[k]] = nx[k]; else hd[l] = nx[k];
    if (nx[k] >= 0) pv[nx[k]] = pv[k]; else tl[l] = pv[k];
    len[l]--; where_[k] = NONE;
}
static void replace(int x) {
    if (len[T1] >= 1 && ((where_[x] == B2 && len[T1] == p) || len[T1] > p)) { int v = tl[T1]; lrem(v); lpush(B1, v); }
    else { int v = tl[T2]; lrem(v); lpush(B2, v); }
}
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int arc(int x) {
    if (where_[x] == T1 || where_[x] == T2) { lrem(x); lpush(T2, x); return 1; }
    if (where_[x] == B1) {
        p = imin(C, p + imax(len[B2] / len[B1], 1));
        replace(x); lrem(x); lpush(T2, x); return 0;
    }
    if (where_[x] == B2) {
        p = imax(0, p - imax(len[B1] / len[B2], 1));
        replace(x); lrem(x); lpush(T2, x); return 0;
    }
    if (len[T1] + len[B1] == C) {
        if (len[T1] < C) { lrem(tl[B1]); replace(x); }
        else lrem(tl[T1]);
    } else {
        int total = len[T1] + len[T2] + len[B1] + len[B2];
        if (total >= C) { if (total == 2 * C) lrem(tl[B2]); replace(x); }
    }
    lpush(T1, x);
    return 0;
}

/* ---- oracle: plain arrays, index 0 = MRU, linear search everywhere ---- */
static int La[5][2 * C + 2], Ln[5], rp;
static int find_in(int l, int x) { for (int i = 0; i < Ln[l]; i++) if (La[l][i] == x) return i; return -1; }
static void rdel(int l, int i) { memmove(&La[l][i], &La[l][i + 1], (size_t)(Ln[l] - i - 1) * sizeof(int)); Ln[l]--; }
static void radd(int l, int x) { memmove(&La[l][1], &La[l][0], (size_t)Ln[l] * sizeof(int)); La[l][0] = x; Ln[l]++; }
static void rreplace(int x_in_b2) {
    if (Ln[T1] >= 1 && ((x_in_b2 && Ln[T1] == rp) || Ln[T1] > rp)) { int v = La[T1][Ln[T1] - 1]; rdel(T1, Ln[T1] - 1); radd(B1, v); }
    else { int v = La[T2][Ln[T2] - 1]; rdel(T2, Ln[T2] - 1); radd(B2, v); }
}
static int rarc(int x) {
    int i;
    if ((i = find_in(T1, x)) >= 0) { rdel(T1, i); radd(T2, x); return 1; }
    if ((i = find_in(T2, x)) >= 0) { rdel(T2, i); radd(T2, x); return 1; }
    if ((i = find_in(B1, x)) >= 0) { rp = imin(C, rp + imax(Ln[B2] / Ln[B1], 1)); rreplace(0); i = find_in(B1, x); rdel(B1, i); radd(T2, x); return 0; }
    if ((i = find_in(B2, x)) >= 0) { rp = imax(0, rp - imax(Ln[B1] / Ln[B2], 1)); rreplace(1); i = find_in(B2, x); rdel(B2, i); radd(T2, x); return 0; }
    if (Ln[T1] + Ln[B1] == C) {
        if (Ln[T1] < C) { rdel(B1, Ln[B1] - 1); rreplace(0); } else rdel(T1, Ln[T1] - 1);
    } else {
        int total = Ln[T1] + Ln[T2] + Ln[B1] + Ln[B2];
        if (total >= C) { if (total == 2 * C) rdel(B2, Ln[B2] - 1); rreplace(0); }
    }
    radd(T1, x);
    return 0;
}

/* ---- plain LRU for comparison ---- */
static int lru[C], ln;
static int lru_access(int x) {
    for (int i = 0; i < ln; i++) if (lru[i] == x) { memmove(&lru[1], &lru[0], (size_t)i * sizeof(int)); lru[0] = x; return 1; }
    if (ln < C) ln++;
    memmove(&lru[1], &lru[0], (size_t)(ln - 1) * sizeof(int)); lru[0] = x;
    return 0;
}

static int gen(int phase) {
    unsigned r = rnd() % 100;
    switch (phase) {
    case 0: return r < 85 ? (int)(rnd() % 16) : 16 + (int)(rnd() % 40);        /* recency-friendly hot set */
    case 1: return (int)(rnd() % 300) + 50;                                     /* uniform over big set */
    case 2: return r < 50 ? (int)(rnd() % 10) : 100 + (int)((rnd() + 0u) % 250); /* frequent keys plus scans */
    default: return (int)(rnd() % 30);
    }
}

int main(void) {
    for (int l = 0; l < 5; l++) hd[l] = tl[l] = -1;
    long ah = 0, lh = 0, n = 0;
    for (int phase = 0; phase < 4; phase++) {
        long pa = 0, pl = 0;
        for (int i = 0; i < 6000; i++) {
            int x = gen(phase);
            int a = arc(x), b = rarc(x), c = lru_access(x);
            check(a == b, "ARC hit agrees with oracle");
            check(p == rp, "adaptive target p agrees");
            for (int l = T1; l <= B2; l++) check(len[l] == Ln[l], "list lengths");
            check(len[T1] + len[T2] <= C && len[T1] + len[B1] <= C && len[T1] + len[T2] + len[B1] + len[B2] <= 2 * C, "ARC invariants");
            pa += a; pl += c; n++;
        }
        ah += pa; lh += pl;
        printf("phase %d: ARC hits=%4ld LRU hits=%4ld p=%2d |T1|=%2d |T2|=%2d |B1|=%2d |B2|=%2d\n", phase, pa, pl, p, len[T1], len[T2], len[B1], len[B2]);
    }
    /* list contents agree */
    for (int l = T1; l <= B2; l++) { int i = 0; for (int k = hd[l]; k >= 0; k = nx[k], i++) check(La[l][i] == k, "list order"); }
    printf("total ARC hits=%ld LRU hits=%ld of %ld\n", ah, lh, n);
    return 0;
}
