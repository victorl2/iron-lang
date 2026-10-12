/*
 * title: Aho-Corasick text censoring
 * topic: algorithms
 * covers: Aho-Corasick, difference array, longest match via fail chain, ASCII alphabet, fuzz against brute force
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

enum { MAXN = 256 };

typedef struct {
    int next[128];
    int fail;
    int depth_hit; /* longest pattern length ending at this state, 0 if none (via fail chain) */
} State;

static State st[MAXN];
static int nst;

static int mk_state(void) {
    for (int c = 0; c < 128; c++)
        st[nst].next[c] = -1;
    st[nst].fail = 0;
    st[nst].depth_hit = 0;
    return nst++;
}

static void add_word(const char *w) {
    int cur = 0;
    for (const char *p = w; *p; p++) {
        int c = (unsigned char)*p;
        if (st[cur].next[c] < 0) {
            int nn = mk_state();
            st[cur].next[c] = nn;
        }
        cur = st[cur].next[c];
    }
    int l = (int)strlen(w);
    if (l > st[cur].depth_hit)
        st[cur].depth_hit = l;
}

static void finish(void) {
    int q[MAXN], h = 0, t = 0;
    for (int c = 0; c < 128; c++) {
        int v = st[0].next[c];
        if (v < 0)
            st[0].next[c] = 0;
        else
            q[t++] = v;
    }
    while (h < t) {
        int u = q[h++];
        int f = st[u].fail;
        if (st[f].depth_hit > st[u].depth_hit)
            st[u].depth_hit = st[f].depth_hit;
        for (int c = 0; c < 128; c++) {
            int v = st[u].next[c];
            if (v < 0)
                st[u].next[c] = st[f].next[c];
            else {
                st[v].fail = st[f].next[c];
                q[t++] = v;
            }
        }
    }
}

/* Mask every character covered by some pattern occurrence, using a
 * difference array over "start" positions of the longest match ending at i. */
static int censor(const char *text, char *out) {
    int n = (int)strlen(text);
    int *diff = calloc((size_t)n + 2, sizeof(int));
    int cur = 0;
    for (int i = 0; i < n; i++) {
        cur = st[cur].next[(unsigned char)text[i]];
        int l = st[cur].depth_hit;
        if (l > 0) {
            diff[i - l + 1]++;
            diff[i + 1]--;
        }
    }
    int run = 0, masked = 0;
    for (int i = 0; i < n; i++) {
        run += diff[i];
        if (run > 0) {
            out[i] = '*';
            masked++;
        } else {
            out[i] = text[i];
        }
    }
    out[n] = 0;
    free(diff);
    return masked;
}

static int censor_brute(const char *text, const char **words, int nw, char *out) {
    int n = (int)strlen(text), masked = 0;
    memcpy(out, text, (size_t)n + 1);
    for (int w = 0; w < nw; w++) {
        int l = (int)strlen(words[w]);
        for (int i = 0; i + l <= n; i++)
            if (strncmp(text + i, words[w], (size_t)l) == 0)
                for (int k = 0; k < l; k++)
                    out[i + k] = '*';
    }
    for (int i = 0; i < n; i++)
        masked += out[i] == '*';
    return masked;
}

int main(void) {
    const char *bad[] = {"darn", "heck", "frick", "dang", "ck", "rat"};
    enum { NW = 6 };
    mk_state();
    for (int i = 0; i < NW; i++)
        add_word(bad[i]);
    finish();
    printf("states: %d\n", nst);

    const char *lines[] = {
        "what the heck, darn it, frickin dang rat",
        "clean sentence with nothing bad",
        "ratrat darndarn hecking",
        "cracker jack",
    };
    static char out[512], ref[512];
    for (int i = 0; i < 4; i++) {
        int m1 = censor(lines[i], out);
        int m2 = censor_brute(lines[i], bad, NW, ref);
        check(m1 == m2 && strcmp(out, ref) == 0, "censor matches brute");
        printf("%s\n%s (%d masked)\n", lines[i], out, m1);
    }
    /* random fuzz over a small alphabet */
    int total = 0;
    for (int r = 0; r < 200; r++) {
        char buf[100];
        int n = 20 + (int)(rnd() % 70);
        for (int i = 0; i < n; i++) {
            const char *al = "darnhekcfitgo ";
            buf[i] = al[rnd() % 14];
        }
        buf[n] = 0;
        int m1 = censor(buf, out);
        int m2 = censor_brute(buf, bad, NW, ref);
        check(m1 == m2 && strcmp(out, ref) == 0, "fuzz");
        total += m1;
    }
    printf("fuzz total masked: %d\n", total);
    return 0;
}
