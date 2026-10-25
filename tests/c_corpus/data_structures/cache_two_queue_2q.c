/*
 * title: Full 2Q cache with A1in, A1out and Am queues
 * topic: data_structures
 * covers: 2Q, FIFO admission queue, ghost queue, main LRU, promotion on second touch, key-indexed lists, oracle with arrays, scan resistance
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C 32
#define KIN (C / 4)      /* A1in size */
#define KOUT (C / 2)     /* A1out ghost size */
#define KEYS 500

static unsigned long long rs = 0x2020202ULL * 0x9E37ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

enum { NONE, AIN, AOUT, AM };

/* ---- fast: linked lists in key-indexed arrays ---- */
static int loc[KEYS], pv[KEYS], nx[KEYS], hd[4], tl[4], ln[4];
static void push(int l, int k) { pv[k] = -1; nx[k] = hd[l]; if (hd[l] >= 0) pv[hd[l]] = k; else tl[l] = k; hd[l] = k; loc[k] = l; ln[l]++; }
static void rem(int k) {
    int l = loc[k];
    if (pv[k] >= 0) nx[pv[k]] = nx[k]; else hd[l] = nx[k];
    if (nx[k] >= 0) pv[nx[k]] = pv[k]; else tl[l] = pv[k];
    ln[l]--; loc[k] = NONE;
}
static void reclaim(void) {
    if (ln[AM] + ln[AIN] < C) return;
    if (ln[AIN] > KIN || ln[AM] == 0) {
        int v = tl[AIN]; rem(v); push(AOUT, v);
        if (ln[AOUT] > KOUT) rem(tl[AOUT]);
    } else rem(tl[AM]);
}
static int access2q(int x) {
    if (loc[x] == AM) { rem(x); push(AM, x); return 1; }
    if (loc[x] == AIN) return 1;                      /* stays in FIFO position */
    if (loc[x] == AOUT) { rem(x); reclaim(); push(AM, x); return 0; }
    reclaim(); push(AIN, x);
    return 0;
}

/* ---- oracle: arrays, front = newest ---- */
static int A[4][C + KOUT + 2], An[4];
static int fidx(int l, int x) { for (int i = 0; i < An[l]; i++) if (A[l][i] == x) return i; return -1; }
static void adel(int l, int i) { memmove(&A[l][i], &A[l][i + 1], (size_t)(An[l] - i - 1) * sizeof(int)); An[l]--; }
static void aadd(int l, int x) { memmove(&A[l][1], &A[l][0], (size_t)An[l] * sizeof(int)); A[l][0] = x; An[l]++; }
static void oreclaim(void) {
    if (An[AM] + An[AIN] < C) return;
    if (An[AIN] > KIN || An[AM] == 0) {
        int v = A[AIN][An[AIN] - 1]; adel(AIN, An[AIN] - 1); aadd(AOUT, v);
        if (An[AOUT] > KOUT) adel(AOUT, An[AOUT] - 1);
    } else adel(AM, An[AM] - 1);
}
static int oaccess(int x) {
    int i;
    if ((i = fidx(AM, x)) >= 0) { adel(AM, i); aadd(AM, x); return 1; }
    if (fidx(AIN, x) >= 0) return 1;
    if ((i = fidx(AOUT, x)) >= 0) { adel(AOUT, i); oreclaim(); aadd(AM, x); return 0; }
    oreclaim(); aadd(AIN, x);
    return 0;
}

/* LRU baseline */
static int lru[C], lnn;
static int lru_access(int x) {
    for (int i = 0; i < lnn; i++) if (lru[i] == x) { memmove(&lru[1], &lru[0], (size_t)i * sizeof(int)); lru[0] = x; return 1; }
    if (lnn < C) lnn++;
    memmove(&lru[1], &lru[0], (size_t)(lnn - 1) * sizeof(int)); lru[0] = x;
    return 0;
}

int main(void) {
    for (int l = 0; l < 4; l++) hd[l] = tl[l] = -1;
    long hits2q = 0, hitslru = 0, n = 0;
    int scan = 0;
    for (int phase = 0; phase < 3; phase++) {
        long h2 = 0, hl = 0;
        for (int i = 0; i < 9000; i++) {
            int x;
            unsigned r = rnd() % 100;
            if (phase == 0) x = r < 90 ? (int)(rnd() % 20) : 20 + (int)(rnd() % 200);
            else if (phase == 1) { x = r < 55 ? (int)(rnd() % 20) : 100 + (scan++ % 400); }   /* hot set diluted by a sequential scan */
            else x = (int)(rnd() % 60);
            int a = access2q(x), b = oaccess(x), c = lru_access(x);
            check(a == b, "2Q agrees with oracle");
            for (int l = AIN; l <= AM; l++) check(ln[l] == An[l], "queue sizes");
            check(ln[AIN] + ln[AM] <= C && ln[AOUT] <= KOUT, "2Q bounds");
            h2 += a; hl += c; n++;
        }
        hits2q += h2; hitslru += hl;
        printf("phase %d: 2Q=%4ld LRU=%4ld  |A1in|=%2d |A1out|=%2d |Am|=%2d\n", phase, h2, hl, ln[AIN], ln[AOUT], ln[AM]);
    }
    for (int l = AIN; l <= AM; l++) { int i = 0; for (int k = hd[l]; k >= 0; k = nx[k], i++) check(A[l][i] == k, "queue order"); }
    printf("total 2Q=%ld LRU=%ld of %ld\n", hits2q, hitslru, n);
    return 0;
}
