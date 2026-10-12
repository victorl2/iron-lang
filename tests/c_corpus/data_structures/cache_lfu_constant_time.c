/*
 * title: O(1) LFU cache with frequency buckets
 * topic: data_structures
 * covers: LFU cache, frequency list of key lists, min-frequency pointer, LRU tie-break within a frequency, O(1) get/put, brute-force oracle
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAP 20
#define KEYS 120
#define MAXF 4096

static unsigned long long rs = 0x1FF0ACE5ULL * 8191;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* node per key; each frequency has a doubly linked list (front = most recent). Lists are
 * threaded through arrays: head[f], tail[f]. */
static int in[KEYS], val[KEYS], freq[KEYS], prv[KEYS], nxt[KEYS];
static int head[MAXF], tail[MAXF];
static int size, minf;
static long evictions;

static void list_push_front(int f, int k) {
    prv[k] = -1; nxt[k] = head[f];
    if (head[f] >= 0) prv[head[f]] = k; else tail[f] = k;
    head[f] = k;
}
static void list_remove(int f, int k) {
    if (prv[k] >= 0) nxt[prv[k]] = nxt[k]; else head[f] = nxt[k];
    if (nxt[k] >= 0) prv[nxt[k]] = prv[k]; else tail[f] = prv[k];
}
static void bump(int k) {
    int f = freq[k];
    list_remove(f, k);
    if (head[f] < 0 && minf == f) minf = f + 1;
    freq[k] = f + 1;
    list_push_front(f + 1, k);
}
static int get(int k, int *v) {
    if (!in[k]) return 0;
    bump(k); *v = val[k]; return 1;
}
static void put(int k, int v) {
    if (in[k]) { val[k] = v; bump(k); return; }
    if (size == CAP) {
        int victim = tail[minf];          /* LFU, oldest among equals */
        list_remove(minf, victim); in[victim] = 0; size--; evictions++;
    }
    in[k] = 1; val[k] = v; freq[k] = 1; minf = 1; size++;
    list_push_front(1, k);
}

/* oracle: linear scan with (freq, last-use tick) */
static int rin[KEYS], rval[KEYS], rfreq[KEYS]; static long rtick[KEYS], t; static int rsize;
static int rget(int k, int *v) { if (!rin[k]) return 0; rfreq[k]++; rtick[k] = ++t; *v = rval[k]; return 1; }
static void rput(int k, int v) {
    if (rin[k]) { rval[k] = v; rfreq[k]++; rtick[k] = ++t; return; }
    if (rsize == CAP) {
        int b = -1;
        for (int i = 0; i < KEYS; i++) if (rin[i] && (b < 0 || rfreq[i] < rfreq[b] || (rfreq[i] == rfreq[b] && rtick[i] < rtick[b]))) b = i;
        rin[b] = 0; rsize--;
    }
    rin[k] = 1; rval[k] = v; rfreq[k] = 1; rtick[k] = ++t; rsize++;
}

int main(void) {
    for (int i = 0; i < MAXF; i++) head[i] = tail[i] = -1;
    long hits = 0, misses = 0;
    /* Phase 1: skewed workload. Phase 2: scan pollution that LFU should survive. */
    for (int step = 0; step < 40000; step++) {
        int k;
        if (step < 20000) k = (rnd() % 100 < 80) ? (int)(rnd() % 12) : (int)(rnd() % KEYS);
        else k = (step % 3 == 0) ? (int)(rnd() % 12) : (int)(step % KEYS);
        int v = 0, r = 0, got = get(k, &v);
        int rg = rget(k, &r);
        check(got == rg, "presence");
        if (got) { check(v == r, "value"); hits++; }
        else { misses++; put(k, step); rput(k, step); }
        check(size == rsize, "size");
        if (step % 5000 == 0) {
            int min_seen = MAXF, cnt = 0;
            for (int i = 0; i < KEYS; i++) if (in[i]) { cnt++; check(rin[i], "same key set"); check(freq[i] == rfreq[i], "frequencies"); if (freq[i] < min_seen) min_seen = freq[i]; }
            check(cnt == size && min_seen == minf, "min frequency pointer");
        }
        if (step == 19999) printf("phase1 hits=%ld misses=%ld\n", hits, misses);
    }
    printf("phase2 total hits=%ld misses=%ld evictions=%ld\n", hits, misses, evictions);
    int hot = 0; for (int i = 0; i < 12; i++) hot += in[i];
    int maxf = 0, maxk = -1; for (int i = 0; i < KEYS; i++) if (in[i] && freq[i] > maxf) { maxf = freq[i]; maxk = i; }
    printf("hot keys resident=%d/12 size=%d max freq=%d (key %d) minf=%d\n", hot, size, maxf, maxk, minf);
    return 0;
}
