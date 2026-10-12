/*
 * title: KMP automaton streaming matcher
 * topic: algorithms
 * covers: KMP DFA construction, restart state, chunked streaming input, state carried across buffers, skewed DNA data
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

/* KMP as a deterministic automaton: dfa[state][char], and a streaming matcher
 * that keeps only one integer of state between chunks of input. */
enum { SIGMA = 4 };

typedef struct {
    int m;
    int (*dfa)[SIGMA];
} Dfa;

static int code(char c) {
    switch (c) {
    case 'A': return 0;
    case 'C': return 1;
    case 'G': return 2;
    default: return 3;
    }
}

static Dfa dfa_build(const char *p) {
    Dfa d;
    d.m = (int)strlen(p);
    d.dfa = calloc((size_t)d.m + 1, sizeof(int[SIGMA]));
    int x = 0; /* restart state */
    d.dfa[0][code(p[0])] = 1;
    for (int j = 1; j <= d.m; j++) {
        for (int c = 0; c < SIGMA; c++)
            d.dfa[j][c] = d.dfa[x][c];
        if (j < d.m) {
            d.dfa[j][code(p[j])] = j + 1;
            x = d.dfa[x][code(p[j])];
        }
    }
    return d;
}

typedef struct {
    const Dfa *d;
    int state;
    long consumed;
    long found;
    long first;
} Stream;

static void stream_feed(Stream *s, const char *chunk, int n) {
    for (int i = 0; i < n; i++) {
        s->state = s->d->dfa[s->state][code(chunk[i])];
        s->consumed++;
        if (s->state == s->d->m) {
            if (s->found == 0)
                s->first = s->consumed - s->d->m;
            s->found++;
            /* the automaton continues from the full-match state naturally */
        }
    }
}

static long brute(const char *t, int n, const char *p) {
    int m = (int)strlen(p);
    long c = 0;
    for (int i = 0; i + m <= n; i++)
        c += memcmp(t + i, p, (size_t)m) == 0;
    return c;
}

static void dna(char *s, int n, int skew) {
    for (int i = 0; i < n; i++) {
        unsigned r = rnd() % 10;
        s[i] = "ACGT"[skew ? (r < 7 ? 0 : r - 6) % 4 : r % 4];
    }
    s[n] = 0;
}

int main(void) {
    const char *pats[] = {"ACGT", "AAAA", "GATTACA", "ACAC", "TTT"};
    static char text[20001];
    for (int c = 0; c < 5; c++) {
        Dfa d = dfa_build(pats[c]);
        printf("pattern %-8s states=%d  row for state %d:", pats[c], d.m + 1, d.m);
        for (int a = 0; a < SIGMA; a++)
            printf(" %d", d.dfa[d.m][a]);
        printf("\n");
        for (int skew = 0; skew < 2; skew++) {
            dna(text, 20000, skew);
            long want = brute(text, 20000, pats[c]);
            /* feed in irregular chunks: results must not depend on chunking */
            Stream s = {&d, 0, 0, 0, -1};
            int pos = 0;
            while (pos < 20000) {
                int chunk = 1 + (int)(rnd() % 97);
                if (pos + chunk > 20000)
                    chunk = 20000 - pos;
                stream_feed(&s, text + pos, chunk);
                pos += chunk;
            }
            check(s.found == want, "chunked stream count");
            Stream whole = {&d, 0, 0, 0, -1};
            stream_feed(&whole, text, 20000);
            check(whole.found == want && whole.first == s.first, "whole vs chunked");
            printf("  %s: %ld matches, first at %ld\n", skew ? "A-skewed" : "uniform ", s.found, s.first);
        }
        free(d.dfa);
    }
    return 0;
}
