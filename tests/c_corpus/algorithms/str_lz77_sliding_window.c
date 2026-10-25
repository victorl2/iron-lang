/*
 * title: LZ77 sliding-window compression
 * topic: algorithms
 * covers: LZ77, back-reference tokens, overlapping copies, window search, invalid stream rejection
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

/* LZ77 with a sliding window; tokens are either literals or (distance, length) pairs.
 * Overlapping copies (distance < length) are allowed and exercised by runs. */
typedef struct {
    unsigned short dist; /* 0 for a literal */
    unsigned short len;  /* literal byte when dist == 0 */
} Tok;

enum { WINDOW = 255, MAXLEN = 40, MINMATCH = 3 };

static int lz77_encode(const unsigned char *s, int n, Tok *out, long *probes) {
    int nt = 0;
    for (int i = 0; i < n;) {
        int best_len = 0, best_d = 0;
        int lo = i > WINDOW ? i - WINDOW : 0;
        for (int j = i - 1; j >= lo; j--) {
            (*probes)++;
            int k = 0;
            while (i + k < n && k < MAXLEN && s[j + k] == s[i + k])
                k++;
            if (k > best_len) {
                best_len = k;
                best_d = i - j;
            }
        }
        if (best_len >= MINMATCH) {
            out[nt].dist = (unsigned short)best_d;
            out[nt].len = (unsigned short)best_len;
            i += best_len;
        } else {
            out[nt].dist = 0;
            out[nt].len = s[i];
            i++;
        }
        nt++;
    }
    return nt;
}

static int lz77_decode(const Tok *t, int nt, unsigned char *out, int cap) {
    int n = 0;
    for (int i = 0; i < nt; i++) {
        if (t[i].dist == 0) {
            if (n >= cap)
                return -1;
            out[n++] = (unsigned char)t[i].len;
        } else {
            if (t[i].dist > n || n + t[i].len > cap)
                return -1;
            for (int k = 0; k < t[i].len; k++, n++)
                out[n] = out[n - t[i].dist];
        }
    }
    return n;
}

static void gen_text(unsigned char *s, int n, int mode) {
    static const char *words[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog "};
    int i = 0;
    while (i < n) {
        if (mode == 0) {
            const char *w = words[rnd() % 8];
            for (; *w && i < n; w++)
                s[i++] = (unsigned char)*w;
        } else if (mode == 1) {
            s[i++] = (unsigned char)('a' + rnd() % 26);
        } else {
            int run = 1 + (int)(rnd() % 30);
            unsigned char c = (unsigned char)('a' + rnd() % 3);
            while (run-- && i < n)
                s[i++] = c;
        }
    }
}

int main(void) {
    const char *abra = "abracadabra abracadabra";
    Tok toks[64];
    unsigned char dec[64];
    long probes = 0;
    int nt = lz77_encode((const unsigned char *)abra, (int)strlen(abra), toks, &probes);
    printf("%s ->", abra);
    for (int i = 0; i < nt; i++) {
        if (toks[i].dist)
            printf(" <%d,%d>", toks[i].dist, toks[i].len);
        else
            printf(" %c", toks[i].len);
    }
    printf("\n");
    int dn = lz77_decode(toks, nt, dec, 64);
    check(dn == (int)strlen(abra) && memcmp(dec, abra, (size_t)dn) == 0, "demo roundtrip");

    static unsigned char src[3000], out[3000];
    static Tok tk[3000];
    const char *names[] = {"word text", "random letters", "runs"};
    for (int mode = 0; mode < 3; mode++) {
        int n = 3000;
        gen_text(src, n, mode);
        probes = 0;
        nt = lz77_encode(src, n, tk, &probes);
        int matches = 0, overlap = 0, covered = 0;
        for (int i = 0; i < nt; i++)
            if (tk[i].dist) {
                matches++;
                covered += tk[i].len;
                overlap += tk[i].dist < tk[i].len;
            }
        int dn2 = lz77_decode(tk, nt, out, n);
        check(dn2 == n && memcmp(out, src, (size_t)n) == 0, "roundtrip");
        printf("%-15s tokens=%4d matches=%4d overlapping=%3d covered=%4d probes=%ld\n", names[mode], nt,
               matches, overlap, covered, probes);
    }
    Tok bad1[1] = {{5, 3}};
    Tok bad2[2] = {{0, 'a'}, {1, 200}};
    check(lz77_decode(bad1, 1, out, 100) < 0, "distance before start");
    check(lz77_decode(bad2, 2, out, 100) < 0, "overflow");
    printf("invalid token streams rejected\n");
    return 0;
}
