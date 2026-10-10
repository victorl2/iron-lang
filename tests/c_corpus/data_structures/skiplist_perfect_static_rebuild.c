/*
 * title: Deterministic perfect skip list with buffered inserts and rebuild
 * topic: data_structures
 * covers: deterministic skip list, level from trailing zeros of index, static towers, search cost bound, insert buffer, merge rebuild, tombstones
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXK 30000
#define LV 16

static unsigned long long rs = 0x9E27F001ULL * 2654435761ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* Static perfect skip list over a sorted array: element i has tower height 1 + ctz(i+1).
 * Level l links connect every 2^l-th element, so a search takes O(log n) hops. */
typedef struct {
    int n;
    int *key;
    unsigned char *dead;
    int *nxt[LV];       /* nxt[l][i]: index of next element with height > l, or n */
    int top;
} Perfect;

static int ctz(unsigned x) { int c = 0; while (!(x & 1u)) { x >>= 1; c++; } return c; }

static void pf_build(Perfect *p, const int *keys, int n) {
    p->n = n; p->key = malloc((size_t)(n ? n : 1) * sizeof(int)); if (n) memcpy(p->key, keys, (size_t)n * sizeof(int));
    p->dead = calloc((size_t)(n ? n : 1), 1);
    p->top = 0;
    for (int l = 0; l < LV; l++) p->nxt[l] = malloc((size_t)(n + 1) * sizeof(int));
    int last[LV];
    for (int l = 0; l < LV; l++) last[l] = -1;
    for (int i = 0; i < n; i++) { int h = 1 + ctz((unsigned)(i + 1)); if (h > LV) h = LV; if (h > p->top) p->top = h; }
    for (int l = 0; l < LV; l++) for (int i = 0; i <= n; i++) p->nxt[l][i] = n;
    for (int i = 0; i < n; i++) {
        int h = 1 + ctz((unsigned)(i + 1)); if (h > LV) h = LV;
        for (int l = 0; l < h; l++) { if (last[l] >= 0) p->nxt[l][last[l]] = i; last[l] = i; }
    }
}
static void pf_free(Perfect *p) { free(p->key); free(p->dead); for (int l = 0; l < LV; l++) free(p->nxt[l]); }
/* index of first element >= key (n if none); hops counts pointer moves */
static int pf_lb(const Perfect *p, int key, int *hops) {
    int pos = -1;   /* virtual head before element 0, which has full height */
    for (int l = p->top - 1; l >= 0; l--) {
        for (;;) {
            int nx = pos < 0 ? -1 : p->nxt[l][pos];
            if (pos < 0) {
                /* head's level-l successor: first element with height > l, i.e. index 2^l - 1 */
                int first = (1 << l) - 1;
                nx = first < p->n ? first : p->n;
            }
            if (nx < p->n && p->key[nx] < key) { pos = nx; (*hops)++; } else break;
        }
    }
    return pos + 1;
}

/* Dynamic wrapper: perfect base + small sorted buffer; rebuild when buffer exceeds sqrt-ish threshold. */
static Perfect base; static int buf[MAXK], nbuf, present[MAXK * 2];
static int rebuilds;
static int contains(int k, int *hops) {
    int i = pf_lb(&base, k, hops);
    if (i < base.n && base.key[i] == k && !base.dead[i]) return 1;
    int lo = 0, hi = nbuf;
    while (lo < hi) { int m = (lo + hi) / 2; if (buf[m] < k) lo = m + 1; else hi = m; }
    return lo < nbuf && buf[lo] == k;
}
static void rebuild(void) {
    int *out = malloc((size_t)(base.n + nbuf + 1) * sizeof(int)); int n = 0, i = 0, j = 0;
    while (i < base.n || j < nbuf) {
        if (i < base.n && base.dead[i]) { i++; continue; }
        if (j >= nbuf || (i < base.n && base.key[i] < buf[j])) out[n++] = base.key[i++];
        else out[n++] = buf[j++];
    }
    pf_free(&base); pf_build(&base, out, n); free(out); nbuf = 0; rebuilds++;
}
static void insert(int k) {
    int h = 0;
    if (contains(k, &h)) return;
    int lo = 0, hi = nbuf;
    while (lo < hi) { int m = (lo + hi) / 2; if (buf[m] < k) lo = m + 1; else hi = m; }
    /* key may be tombstoned in the base: resurrect it instead of buffering */
    int i = pf_lb(&base, k, &h);
    if (i < base.n && base.key[i] == k) { base.dead[i] = 0; return; }
    memmove(buf + lo + 1, buf + lo, (size_t)(nbuf - lo) * sizeof(int)); buf[lo] = k; nbuf++;
    if (nbuf > 64 + base.n / 8) rebuild();
}
static void erase(int k) {
    int h = 0, i = pf_lb(&base, k, &h);
    if (i < base.n && base.key[i] == k && !base.dead[i]) { base.dead[i] = 1; return; }
    int lo = 0, hi = nbuf;
    while (lo < hi) { int m = (lo + hi) / 2; if (buf[m] < k) lo = m + 1; else hi = m; }
    if (lo < nbuf && buf[lo] == k) { memmove(buf + lo, buf + lo + 1, (size_t)(nbuf - lo - 1) * sizeof(int)); nbuf--; }
}

int main(void) {
    /* static search cost */
    int n = 20000; int *keys = malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) keys[i] = i * 3;
    Perfect p; pf_build(&p, keys, n);
    long worst = 0, total = 0;
    for (int q = 0; q < 5000; q++) {
        int k = (int)(rnd() % (unsigned)(3 * n + 5)), hops = 0;
        int i = pf_lb(&p, k, &hops);
        int expect = (k + 2) / 3; if (expect > n) expect = n;
        check(i == expect, "static lower bound");
        if (hops > worst) worst = hops;
        total += hops;
    }
    printf("static n=%d height=%d worst hops=%ld avg hops=%ld\n", n, p.top, worst, total / 5000);
    check(worst <= 2 * 15 + 2, "logarithmic search");
    pf_free(&p); free(keys);

    /* dynamic use */
    pf_build(&base, NULL, 0);
    int ins = 0, del = 0;
    for (int step = 0; step < 40000; step++) {
        int k = (int)(rnd() % (MAXK * 2)); unsigned op = rnd() % 10;
        if (op < 5) { insert(k); if (!present[k]) ins++; present[k] = 1; }
        else if (op < 7) { erase(k); if (present[k]) del++; present[k] = 0; }
        else { int h = 0; check(contains(k, &h) == present[k], "contains"); }
    }
    rebuild();
    int cnt = 0; for (int i = 0; i < MAXK * 2; i++) cnt += present[i];
    check(cnt == base.n, "size after final rebuild");
    for (int i = 1; i < base.n; i++) check(base.key[i - 1] < base.key[i], "sorted");
    printf("dynamic inserts=%d deletes=%d size=%d rebuilds=%d height=%d\n", ins, del, base.n, rebuilds, base.top);
    pf_free(&base);
    return 0;
}
