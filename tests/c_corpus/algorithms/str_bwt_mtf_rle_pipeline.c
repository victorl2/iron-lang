/*
 * title: BWT, move-to-front and zero-run coding pipeline
 * topic: algorithms
 * covers: Burrows-Wheeler, move-to-front, bijective base-2 zero-run coding, staged inverse, run statistics
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

/* A bzip2-style pipeline: BWT (via suffix array of the doubled string) -> MTF -> zero-run
 * RLE (RUNA/RUNB style bijective base 2) -> inverse. Reports the size at each stage. */
typedef struct {
    int *v;
    int n;
} Vec;

static const unsigned char *g_t;
static int g_n;

static int cmp_rot(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    for (int k = 0; k < g_n; k++) {
        unsigned char cx = g_t[(x + k) % g_n], cy = g_t[(y + k) % g_n];
        if (cx != cy)
            return cx < cy ? -1 : 1;
    }
    return x - y;
}

static int bwt(const unsigned char *s, int n, unsigned char *out) {
    int *rot = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++)
        rot[i] = i;
    g_t = s;
    g_n = n;
    qsort(rot, (size_t)n, sizeof(int), cmp_rot);
    int row = 0;
    for (int i = 0; i < n; i++) {
        out[i] = s[(rot[i] + n - 1) % n];
        if (rot[i] == 0)
            row = i;
    }
    free(rot);
    return row;
}

static void unbwt(const unsigned char *l, int n, int row, unsigned char *out) {
    int cnt[256] = {0}, first[256], *lf = malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++)
        cnt[l[i]]++;
    for (int c = 0, s = 0; c < 256; c++) {
        first[c] = s;
        s += cnt[c];
    }
    int seen[256] = {0};
    for (int i = 0; i < n; i++)
        lf[i] = first[l[i]] + seen[l[i]]++;
    int r = row;
    for (int k = n - 1; k >= 0; k--) {
        out[k] = l[r];
        r = lf[r];
    }
    free(lf);
}

static void mtf(const unsigned char *s, int n, unsigned char *out) {
    unsigned char list[256];
    for (int i = 0; i < 256; i++)
        list[i] = (unsigned char)i;
    for (int i = 0; i < n; i++) {
        int p = 0;
        while (list[p] != s[i])
            p++;
        out[i] = (unsigned char)p;
        memmove(list + 1, list, (size_t)p);
        list[0] = s[i];
    }
}

static void unmtf(const unsigned char *in, int n, unsigned char *out) {
    unsigned char list[256];
    for (int i = 0; i < 256; i++)
        list[i] = (unsigned char)i;
    for (int i = 0; i < n; i++) {
        int p = in[i];
        unsigned char c = list[p];
        memmove(list + 1, list, (size_t)p);
        list[0] = c;
        out[i] = c;
    }
}

/* zero runs of length r are written as the bijective base-2 digits of r using symbols 0 and 1
 * (0 = RUNA worth 1, 1 = RUNB worth 2), non-zero values v are shifted up by one to v+1. */
static int rle0_encode(const unsigned char *in, int n, unsigned short *out) {
    int o = 0;
    for (int i = 0; i < n;) {
        if (in[i] == 0) {
            int run = 0;
            while (i < n && in[i] == 0) {
                run++;
                i++;
            }
            while (run > 0) {
                if (run & 1) {
                    out[o++] = 0; /* RUNA */
                    run = (run - 1) / 2;
                } else {
                    out[o++] = 1; /* RUNB */
                    run = (run - 2) / 2;
                }
            }
        } else {
            out[o++] = (unsigned short)(in[i] + 1);
            i++;
        }
    }
    return o;
}

static int rle0_decode(const unsigned short *in, int n, unsigned char *out) {
    int o = 0;
    for (int i = 0; i < n;) {
        if (in[i] <= 1) {
            long run = 0, weight = 1;
            while (i < n && in[i] <= 1) {
                run += weight * (in[i] + 1);
                weight *= 2;
                i++;
            }
            for (long k = 0; k < run; k++)
                out[o++] = 0;
        } else {
            out[o++] = (unsigned char)(in[i] - 1);
            i++;
        }
    }
    return o;
}

static int stage_check(const char *name, const unsigned char *src, int n) {
    unsigned char *b = malloc((size_t)(unsigned)n), *m = malloc((size_t)(unsigned)n), *m2 = malloc((size_t)(unsigned)n),
                  *b2 = calloc((size_t)(unsigned)n, 1), *o = malloc((size_t)(unsigned)n);
    unsigned short *r = malloc(sizeof(unsigned short) * (size_t)(n + 8));
    int row = bwt(src, n, b);
    mtf(b, n, m);
    int rn = rle0_encode(m, n, r);
    int mn = rle0_decode(r, rn, m2);
    check(mn == n && memcmp(m, m2, (size_t)(unsigned)n) == 0, "rle0 roundtrip");
    unmtf(m2, n, b2);
    check(memcmp(b, b2, (size_t)(unsigned)n) == 0, "mtf roundtrip");
    unbwt(b2, n, row, o);
    check(memcmp(o, src, (size_t)(unsigned)n) == 0, "bwt roundtrip");
    int zeros = 0, runs0 = 0, runs_src = 1, runs_bwt = 1;
    for (int i = 0; i < n; i++)
        zeros += m[i] == 0;
    for (int i = 1; i < n; i++) {
        runs_src += src[i] != src[i - 1];
        runs_bwt += b[i] != b[i - 1];
    }
    runs0 = rn;
    printf("%-18s n=%4d runs(src)=%4d runs(bwt)=%4d mtf zeros=%4d rle0 symbols=%4d\n", name, n,
           runs_src, runs_bwt, zeros, runs0);
    free(b);
    free(m);
    free(m2);
    free(b2);
    free(o);
    free(r);
    return rn;
}

int main(void) {
    static unsigned char src[1500];
    const char *text = "she sells sea shells by the sea shore, the shells she sells are sea shells for sure. ";
    int tl = (int)strlen(text);
    int n = 0;
    for (int rep = 0; rep < 4; rep++)
        for (int i = 0; i < tl; i++)
            src[n++] = (unsigned char)text[i];
    stage_check("tongue twister x4", src, n);

    n = 1000;
    for (int i = 0; i < n; i++)
        src[i] = (unsigned char)('a' + rnd() % 4);
    stage_check("random 4 letters", src, n);

    n = 1200;
    for (int i = 0; i < n; i++)
        src[i] = (unsigned char)(i % 13 < 9 ? 'x' : 'a' + i % 13);
    stage_check("periodic pattern", src, n);

    memset(src, 'q', 700);
    stage_check("all identical", src, 700);

    /* RUNA/RUNB digits for run lengths 1..10 */
    unsigned char zeros[16] = {0};
    unsigned short code[32];
    printf("zero-run codes:");
    for (int r = 1; r <= 10; r++) {
        int k = rle0_encode(zeros, r, code);
        unsigned char back[16];
        check(rle0_decode(code, k, back) == r, "run decode");
        printf(" %d=", r);
        for (int i = 0; i < k; i++)
            putchar(code[i] == 0 ? 'A' : 'B');
    }
    printf("\n");
    return 0;
}
