/*
 * title: Word break: counting and enumerating segmentations
 * topic: algorithms
 * covers: dynamic programming, word break, dictionary trie-free lookup, count segmentations, backtracking enumeration guided by reachability, shortest segmentation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

static const char *dict[] = {"a", "an", "and", "ant", "ants", "cat", "cats", "dog", "sand", "san", "pine", "apple",
                             "pineapple", "pen", "applepen", "cats", "og", "d", "t", "s"};
#define ND 20

static int in_dict(const char *s, int len) {
    for (int i = 0; i < ND; i++)
        if ((int)strlen(dict[i]) == len && strncmp(dict[i], s, (size_t)len) == 0) return 1;
    return 0;
}

static void enumerate(const char *s, int n, int pos, const unsigned char *can, char *buf, int blen, long *out_cnt, int show) {
    if (pos == n) {
        if (show && *out_cnt < show) printf("    %s\n", buf);
        (*out_cnt)++;
        return;
    }
    for (int len = 1; pos + len <= n; len++)
        if (in_dict(s + pos, len) && can[pos + len]) {
            int nb = blen;
            if (nb) buf[nb++] = ' ';
            memcpy(buf + nb, s + pos, (size_t)len);
            nb += len;
            buf[nb] = 0;
            enumerate(s, n, pos + len, can, buf, nb, out_cnt, show);
            buf[blen] = 0;
        }
}

int main(void) {
    (void)rr;
    const char *inputs[] = {"catsanddog", "pineapplepenapple", "catsandog", "antsandants", "aaaaaaaaaaaaaaaaaaaaaaaa", "dogcat", "x"};
    for (int q = 0; q < 7; q++) {
        const char *s = inputs[q];
        int n = (int)strlen(s);
        long ways[64] = {0};
        int minw[64];
        unsigned char can[64]; /* can[i]: suffix from i is segmentable */
        ways[0] = 1; minw[0] = 0;
        for (int i = 1; i <= n; i++) {
            minw[i] = -1;
            for (int j = 0; j < i; j++)
                if (ways[j] && in_dict(s + j, i - j)) {
                    ways[i] += ways[j];
                    if (minw[i] < 0 || minw[j] + 1 < minw[i]) minw[i] = minw[j] + 1;
                }
        }
        can[n] = 1;
        for (int i = n - 1; i >= 0; i--) {
            can[i] = 0;
            for (int len = 1; i + len <= n; len++) if (in_dict(s + i, len) && can[i + len]) { can[i] = 1; break; }
        }
        CHECK(can[0] == (ways[n] > 0));
        long counted = 0;
        char buf[128] = "";
        printf("%s: ways=%ld min_words=%d\n", s, ways[n], minw[n]);
        enumerate(s, n, 0, can, buf, 0, &counted, n <= 17 ? 6 : 0);
        CHECK(counted == ways[n]);
    }
    return 0;
}
