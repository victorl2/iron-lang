/*
 * title: Binary trie for maximum XOR queries
 * topic: data_structures
 * covers: binary trie, maximum xor pair, deletion with counts, prefix xor subarray, counting pairs below a bound
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define BITS 30
#define MAXNODES 400000

typedef struct { int ch[2]; int cnt; } TN;
static TN tr[MAXNODES];
static int ntn;

static uint64_t rs = 0x60D1CA7ULL * 2731;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static void reset(void) { ntn = 1; memset(&tr[0], 0, sizeof tr[0]); }
static int newnode(void) { check(ntn < MAXNODES, "node pool"); memset(&tr[ntn], 0, sizeof tr[ntn]); return ntn++; }
static void insert(uint32_t x) {
    int u = 0;
    tr[0].cnt++;
    for (int b = BITS - 1; b >= 0; b--) {
        int bit = (int)((x >> b) & 1u);
        if (!tr[u].ch[bit]) { int v = newnode(); tr[u].ch[bit] = v; }
        u = tr[u].ch[bit];
        tr[u].cnt++;
    }
}
static void erase(uint32_t x) { /* x must be present */
    int u = 0;
    tr[0].cnt--;
    for (int b = BITS - 1; b >= 0; b--) {
        int bit = (int)((x >> b) & 1u);
        int v = tr[u].ch[bit];
        check(v && tr[v].cnt > 0, "erase present element");
        tr[v].cnt--;
        u = v;
    }
}
/* maximum of x ^ y over stored y; also returns the argmax value */
static uint32_t max_xor(uint32_t x, uint32_t *arg) {
    int u = 0; uint32_t res = 0, y = 0;
    check(tr[0].cnt > 0, "non-empty trie");
    for (int b = BITS - 1; b >= 0; b--) {
        int bit = (int)((x >> b) & 1u), want = bit ^ 1;
        int v = tr[u].ch[want];
        if (v && tr[v].cnt > 0) { res |= 1u << b; y |= (uint32_t)want << b; u = v; }
        else { u = tr[u].ch[bit]; y |= (uint32_t)bit << b; }
    }
    if (arg) *arg = y;
    return res;
}
/* number of stored y with (x ^ y) < k */
static long count_less(uint32_t x, uint32_t k) {
    int u = 0; long res = 0;
    for (int b = BITS - 1; b >= 0 && u >= 0; b--) {
        int xb = (int)((x >> b) & 1u), kb = (int)((k >> b) & 1u);
        if (kb) {
            int same = tr[u].ch[xb];        /* xor bit 0 < 1: all of those qualify */
            if (same) res += tr[same].cnt;
            int diff = tr[u].ch[xb ^ 1];    /* xor bit 1 equals kb: continue */
            u = diff ? diff : -1;
        } else {
            int same = tr[u].ch[xb];
            u = same ? same : -1;
        }
        if (u >= 0 && tr[u].cnt == 0) u = -1;
    }
    return res;
}

int main(void) {
    /* 1. classic problem: maximum XOR of two numbers in an array */
    long total = 0;
    for (int t = 0; t < 40; t++) {
        int n = 2 + (int)(rnd() % 300);
        uint32_t a[400];
        uint32_t mask = t % 3 == 0 ? 0xffu : (t % 3 == 1 ? 0xfffffu : (1u << BITS) - 1);
        for (int i = 0; i < n; i++) a[i] = rnd() & mask;
        reset();
        uint32_t best = 0;
        insert(a[0]);
        for (int i = 1; i < n; i++) { uint32_t v = max_xor(a[i], NULL); if (v > best) best = v; insert(a[i]); }
        uint32_t brute = 0;
        for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if ((a[i] ^ a[j]) > brute) brute = a[i] ^ a[j];
        check(best == brute, "max xor pair");
        total += best % 1000;
    }
    printf("40 arrays: sum of (max xor mod 1000) = %ld\n", total);
    /* 2. dynamic set with deletions and argmax */
    reset();
    static uint32_t set[600]; int ns = 0; long qsum = 0; int ins = 0, del = 0, qs = 0;
    for (int step = 0; step < 6000; step++) {
        unsigned op = rnd() % 8;
        if (op < 3 && ns < 600) { uint32_t v = rnd() & ((1u << BITS) - 1); insert(v); set[ns++] = v; ins++; }
        else if (op < 5 && ns > 0) { int i = (int)(rnd() % (unsigned)ns); erase(set[i]); set[i] = set[--ns]; del++; }
        else if (ns > 0) {
            uint32_t x = rnd() & ((1u << BITS) - 1), arg;
            uint32_t got = max_xor(x, &arg);
            uint32_t want = 0;
            for (int i = 0; i < ns; i++) if ((set[i] ^ x) > want) want = set[i] ^ x;
            check(got == want && (arg ^ x) == got, "dynamic max xor");
            int present = 0; for (int i = 0; i < ns; i++) if (set[i] == arg) present = 1;
            check(present, "argmax is a member");
            qsum += got % 997; qs++;
        }
        check(tr[0].cnt == ns, "root count");
    }
    printf("dynamic set: %d inserts, %d deletes, %d queries, checksum %ld, trie nodes %d\n", ins, del, qs, qsum, ntn);
    /* 3. maximum XOR subarray via prefix xors */
    long ss = 0;
    for (int t = 0; t < 25; t++) {
        int n = 1 + (int)(rnd() % 200); uint32_t a[200], p[201];
        p[0] = 0;
        for (int i = 0; i < n; i++) { a[i] = rnd() & 0xffffffu; p[i + 1] = p[i] ^ a[i]; }
        reset(); insert(0);
        uint32_t best = 0;
        for (int i = 1; i <= n; i++) { uint32_t v = max_xor(p[i], NULL); if (v > best) best = v; insert(p[i]); }
        uint32_t brute = 0;
        for (int i = 0; i < n; i++) { uint32_t x = 0; for (int j = i; j < n; j++) { x ^= a[j]; if (x > brute) brute = x; } }
        check(best == brute, "max xor subarray");
        ss += best % 1013;
    }
    printf("25 subarray problems: checksum %ld\n", ss);
    /* 4. count pairs with xor below K */
    long cs = 0;
    for (int t = 0; t < 20; t++) {
        int n = 1 + (int)(rnd() % 250); uint32_t a[250];
        uint32_t K = rnd() & 0xfffffu;
        for (int i = 0; i < n; i++) a[i] = rnd() & 0xfffffu;
        reset();
        long pairs = 0;
        for (int i = 0; i < n; i++) { pairs += count_less(a[i], K); insert(a[i]); }
        long brute = 0;
        for (int i = 0; i < n; i++) for (int j = 0; j < i; j++) if ((a[i] ^ a[j]) < K) brute++;
        check(pairs == brute, "pairs with xor below K");
        cs += pairs;
    }
    printf("20 counting problems: total pairs %ld\n", cs);
    return 0;
}
