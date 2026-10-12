/*
 * title: Burrows-Wheeler transform and inversion
 * topic: algorithms
 * covers: BWT, sentinel, sorting rotations, LF mapping, run-count effect, invalid input detection
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

/* Burrows-Wheeler transform with an EOF sentinel byte 0x01 (input must not contain it). */
static const unsigned char *g_t;
static int g_n;

static int cmp_rot(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    for (int k = 0; k < g_n; k++) {
        unsigned char cx = g_t[(x + k) % g_n], cy = g_t[(y + k) % g_n];
        if (cx != cy)
            return cx < cy ? -1 : 1;
    }
    return 0;
}

/* out has n+1 bytes; returns row index of the original string. */
static int bwt_forward(const unsigned char *s, int len, unsigned char *out) {
    int n = len + 1;
    unsigned char *t = malloc((size_t)n);
    memcpy(t, s, (size_t)len);
    t[len] = 1;
    int *rot = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++)
        rot[i] = i;
    g_t = t;
    g_n = n;
    qsort(rot, (size_t)n, sizeof(int), cmp_rot);
    int row = -1;
    for (int i = 0; i < n; i++) {
        out[i] = t[(rot[i] + n - 1) % n];
        if (rot[i] == 0)
            row = i;
    }
    free(rot);
    free(t);
    return row;
}

/* Inverse via LF-mapping. out has n-1 bytes (sentinel removed). */
static int bwt_inverse(const unsigned char *l, int n, int row, unsigned char *out) {
    int count[256] = {0}, first[256], lf[n];
    for (int i = 0; i < n; i++)
        count[l[i]]++;
    int sum = 0;
    for (int c = 0; c < 256; c++) {
        first[c] = sum;
        sum += count[c];
    }
    int seen[256] = {0};
    for (int i = 0; i < n; i++)
        lf[i] = first[l[i]] + seen[l[i]]++;
    /* walk backwards from the row that holds the original string */
    int r = row;
    for (int k = n - 1; k >= 0; k--) {
        unsigned char c = l[r];
        if (k == n - 1) {
            if (c != 1)
                return -1;
        } else {
            out[k] = c;
        }
        r = lf[r];
    }
    return r == row ? 0 : -1;
}

static void show(const unsigned char *b, int n) {
    for (int i = 0; i < n; i++)
        putchar(b[i] == 1 ? '$' : b[i]);
}

int main(void) {
    const char *words[] = {"banana", "abracadabra", "mississippi", "aaaaaaaa", "the quick brown fox", "a"};
    unsigned char l[512], back[512];
    for (int w = 0; w < 6; w++) {
        int len = (int)strlen(words[w]);
        int row = bwt_forward((const unsigned char *)words[w], len, l);
        printf("%-20s -> ", words[w]);
        show(l, len + 1);
        printf(" row=%d\n", row);
        check(bwt_inverse(l, len + 1, row, back) == 0, "inverse status");
        check(memcmp(back, words[w], (size_t)len) == 0, "inverse content");
    }
    static unsigned char src[400], enc[401], dec[400];
    int runs_gain = 0;
    for (int t = 0; t < 40; t++) {
        int n = 20 + (int)(rnd() % 300);
        for (int i = 0; i < n; i++)
            src[i] = (unsigned char)('a' + rnd() % (t % 4 + 1));
        int row = bwt_forward(src, n, enc);
        check(bwt_inverse(enc, n + 1, row, dec) == 0 && memcmp(dec, src, (size_t)n) == 0,
              "random roundtrip");
        int r0 = 1, r1 = 1;
        for (int i = 1; i < n; i++)
            r0 += src[i] != src[i - 1];
        for (int i = 1; i <= n; i++)
            r1 += enc[i] != enc[i - 1];
        if (r1 < r0)
            runs_gain++;
    }
    printf("random roundtrips ok, BWT reduced run count in %d of 40 strings\n", runs_gain);
    /* invalid: a permutation without proper sentinel cycle fails */
    unsigned char badl[4] = {'a', 'b', 1, 'c'};
    printf("bad transform rejected: %s\n", bwt_inverse(badl, 4, 0, back) < 0 ? "yes" : "no");
    return 0;
}
