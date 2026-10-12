/*
 * title: Rabin-Karp multi-pattern search
 * topic: algorithms
 * covers: rolling hash, modular arithmetic, sorted hash table, binary search, spurious hit verification
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 0x9e3779b97f4a7c15ULL;

static inline unsigned rnd(void) {
    unsigned long long z = (rng_s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (unsigned)((z ^ (z >> 31)) >> 16);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline void rand_str(char *s, int n, int alpha) {
    for (int i = 0; i < n; i++)
        s[i] = (char)('a' + rnd() % (unsigned)alpha);
    s[n] = 0;
}

#define MOD 1000000007ULL
#define BASE 257ULL

typedef struct {
    unsigned long long h;
    int idx;
} PatHash;

static int cmp_ph(const void *a, const void *b) {
    const PatHash *x = a, *y = b;
    if (x->h != y->h)
        return x->h < y->h ? -1 : 1;
    return x->idx - y->idx;
}

static unsigned long long hash_of(const char *s, int m) {
    unsigned long long h = 0;
    for (int i = 0; i < m; i++)
        h = (h * BASE + (unsigned char)s[i]) % MOD;
    return h;
}

int main(void) {
    enum { NP = 6, M = 5, N = 6000 };
    static char text[N + 1];
    rand_str(text, N, 3);
    char pats[NP][M + 1];
    for (int i = 0; i < NP; i++)
        rand_str(pats[i], M, 3);
    /* plant one pattern so at least a known hit exists */
    memcpy(text + 1234, pats[2], M);

    PatHash ph[NP];
    for (int i = 0; i < NP; i++) {
        ph[i].h = hash_of(pats[i], M);
        ph[i].idx = i;
    }
    qsort(ph, NP, sizeof(PatHash), cmp_ph);

    unsigned long long pw = 1;
    for (int i = 0; i < M - 1; i++)
        pw = pw * BASE % MOD;

    int hits[NP] = {0}, false_pos[NP] = {0}, brute[NP] = {0};
    int first[NP];
    for (int i = 0; i < NP; i++)
        first[i] = -1;
    unsigned long long h = hash_of(text, M);
    for (int i = 0; i + M <= N; i++) {
        if (i > 0) {
            unsigned long long out = (unsigned char)text[i - 1] * pw % MOD;
            h = (h + MOD - out) % MOD;
            h = (h * BASE + (unsigned char)text[i + M - 1]) % MOD;
        }
        /* binary search first pattern with hash >= h */
        int lo = 0, hi = NP;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (ph[mid].h < h)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (int k = lo; k < NP && ph[k].h == h; k++) {
            int id = ph[k].idx;
            if (memcmp(text + i, pats[id], M) == 0) {
                hits[id]++;
                if (first[id] < 0)
                    first[id] = i;
            } else {
                false_pos[id]++;
            }
        }
    }
    for (int p = 0; p < NP; p++)
        for (int i = 0; i + M <= N; i++)
            if (memcmp(text + i, pats[p], M) == 0)
                brute[p]++;
    int fp_total = 0;
    for (int p = 0; p < NP; p++) {
        check(hits[p] == brute[p], "hits equal brute force");
        printf("%s: %d hits, first %d\n", pats[p], hits[p], first[p]);
        fp_total += false_pos[p];
    }
    check(hits[2] >= 1, "planted hit found");
    printf("hash false positives: %d\n", fp_total);
    return 0;
}
