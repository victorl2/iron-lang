/*
 * title: Shortest common superstring
 * topic: algorithms
 * covers: overlap via prefix function, bitmask DP over overlap graph, greedy heuristic, substring pruning
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

/* Shortest common superstring: pairwise overlaps via the prefix function, exact bitmask DP
 * (Hamiltonian path on the overlap graph) and the greedy merge heuristic. */
enum { MAXW = 10 };

static int overlap(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    int n = lb + 1 + la;
    char t[128];
    int pi[128];
    memcpy(t, b, (size_t)lb);
    t[lb] = '#';
    memcpy(t + lb + 1, a, (size_t)la + 1);
    pi[0] = 0;
    for (int i = 1; i < n; i++) {
        int k = pi[i - 1];
        while (k > 0 && t[i] != t[k])
            k = pi[k - 1];
        pi[i] = t[i] == t[k] ? k + 1 : 0;
    }
    return pi[n - 1]; /* longest suffix of a that is a prefix of b */
}

static int contains(const char *a, const char *b) {
    return strstr(a, b) != NULL;
}

static int prune(const char **w, int n, const char **out) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        int drop = 0;
        for (int j = 0; j < n && !drop; j++) {
            if (i == j)
                continue;
            if (contains(w[j], w[i]) && (strlen(w[j]) > strlen(w[i]) || j < i))
                drop = 1;
        }
        if (!drop)
            out[k++] = w[i];
    }
    return k;
}

static int exact_length(const char **w, int n, int ov[MAXW][MAXW], int *order) {
    static int dp[1 << MAXW][MAXW], par[1 << MAXW][MAXW];
    const int INF = 1 << 28;
    for (int m = 0; m < (1 << n); m++)
        for (int i = 0; i < n; i++)
            dp[m][i] = INF;
    for (int i = 0; i < n; i++)
        dp[1 << i][i] = (int)strlen(w[i]);
    for (int m = 1; m < (1 << n); m++)
        for (int i = 0; i < n; i++) {
            if (!(m >> i & 1) || dp[m][i] >= INF)
                continue;
            for (int j = 0; j < n; j++) {
                if (m >> j & 1)
                    continue;
                int nm = m | 1 << j;
                int v = dp[m][i] + (int)strlen(w[j]) - ov[i][j];
                if (v < dp[nm][j]) {
                    dp[nm][j] = v;
                    par[nm][j] = i;
                }
            }
        }
    int full = (1 << n) - 1, best = INF, bj = 0;
    for (int j = 0; j < n; j++)
        if (dp[full][j] < best) {
            best = dp[full][j];
            bj = j;
        }
    int m = full, j = bj;
    for (int k = n - 1; k >= 0; k--) {
        order[k] = j;
        int pj = par[m][j];
        m ^= 1 << j;
        j = pj;
    }
    return best;
}

static void build_string(const char **w, int n, const int *order, int ov[MAXW][MAXW], char *out) {
    strcpy(out, w[order[0]]);
    for (int k = 1; k < n; k++)
        strcat(out, w[order[k]] + ov[order[k - 1]][order[k]]);
}

/* greedy: repeatedly merge the pair with the largest overlap (ties: lowest indices) */
static int greedy_length(const char **w, int n, char *out) {
    char items[MAXW][256];
    int cnt = n;
    for (int i = 0; i < n; i++)
        strcpy(items[i], w[i]);
    while (cnt > 1) {
        int bi = 0, bj = 1, bo = -1;
        for (int i = 0; i < cnt; i++)
            for (int j = 0; j < cnt; j++)
                if (i != j) {
                    int o = overlap(items[i], items[j]);
                    if (o > bo) {
                        bo = o;
                        bi = i;
                        bj = j;
                    }
                }
        strcat(items[bi], items[bj] + bo);
        if (bj != cnt - 1)
            strcpy(items[bj], items[cnt - 1]);
        cnt--;
    }
    strcpy(out, items[0]);
    return (int)strlen(out);
}

int main(void) {
    const char *sets[][MAXW] = {
        {"catg", "ctaagt", "gcta", "ttca", "atgcatc"},
        {"abc", "bcd", "cde", "def"},
        {"aaa", "aab", "abb", "bbb"},
        {"xyz", "y", "yz", "wxy"},
        {"ab", "ba", "aba", "bab"},
    };
    int sizes[] = {5, 4, 4, 4, 4};
    char sup[512], gr[512];
    for (int c = 0; c < 5; c++) {
        const char *w[MAXW];
        int n = prune(sets[c], sizes[c], w);
        int ov[MAXW][MAXW], order[MAXW];
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                ov[i][j] = i == j ? 0 : overlap(w[i], w[j]);
        int best = exact_length(w, n, ov, order);
        build_string(w, n, order, ov, sup);
        check((int)strlen(sup) == best, "constructed length");
        for (int i = 0; i < n; i++)
            check(contains(sup, w[i]), "contains word");
        int g = greedy_length(w, n, gr);
        for (int i = 0; i < n; i++)
            check(contains(gr, w[i]), "greedy contains word");
        check(g >= best, "greedy not below optimum");
        printf("set %d: %d words after pruning, optimal=%d greedy=%d  %s\n", c, n, best, g, sup);
    }
    /* random small instances: exact result never longer than concatenation, greedy never shorter */
    int strictly_better = 0;
    for (int t = 0; t < 60; t++) {
        char store[8][10];
        const char *raw[8];
        int n = 3 + (int)(rnd() % 4);
        for (int i = 0; i < n; i++) {
            rand_str(store[i], 2 + (int)(rnd() % 4), 2);
            raw[i] = store[i];
        }
        const char *w[8];
        int k = prune(raw, n, w);
        int ov[MAXW][MAXW], order[MAXW];
        int total = 0;
        for (int i = 0; i < k; i++) {
            total += (int)strlen(w[i]);
            for (int j = 0; j < k; j++)
                ov[i][j] = i == j ? 0 : overlap(w[i], w[j]);
        }
        int best = exact_length(w, k, ov, order);
        build_string(w, k, order, ov, sup);
        for (int i = 0; i < k; i++)
            check(contains(sup, w[i]), "random contains");
        int g = greedy_length(w, k, gr);
        check(best <= g && best <= total, "ordering");
        strictly_better += g > best;
    }
    printf("random: greedy strictly worse than optimal in %d of 60 instances\n", strictly_better);
    return 0;
}
