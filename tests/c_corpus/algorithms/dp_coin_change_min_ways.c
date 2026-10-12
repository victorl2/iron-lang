/*
 * title: Coin change: minimum coins, ways, and lexicographic reconstruction
 * topic: algorithms
 * covers: dynamic programming, coin change, combinations vs permutations counting, loop order, unreachable amounts, non-canonical coin systems vs greedy
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

#define MAXA 200

static int greedy(const int *c, int n, int amt) { /* c sorted descending */
    int cnt = 0;
    for (int i = 0; i < n; i++) { cnt += amt / c[i]; amt %= c[i]; }
    return amt ? -1 : cnt;
}

int main(void) {
    (void)rr;
    int systems[4][6] = {{25, 10, 5, 1}, {4, 3, 1}, {9, 6, 5, 1}, {7, 5, 2}};
    int sn[4] = {4, 3, 4, 3};
    for (int s = 0; s < 4; s++) {
        const int *c = systems[s];
        int n = sn[s];
        int mn[MAXA + 1], from[MAXA + 1];
        unsigned long long comb[MAXA + 1] = {0}, perm[MAXA + 1] = {0};
        mn[0] = 0; comb[0] = 1; perm[0] = 1;
        for (int a = 1; a <= MAXA; a++) {
            mn[a] = -1; from[a] = -1;
            for (int i = 0; i < n; i++)
                if (c[i] <= a && mn[a - c[i]] >= 0 && (mn[a] < 0 || mn[a - c[i]] + 1 < mn[a])) {
                    mn[a] = mn[a - c[i]] + 1;
                    from[a] = c[i];
                }
        }
        for (int i = 0; i < n; i++)
            for (int a = c[i]; a <= MAXA; a++) comb[a] += comb[a - c[i]];
        for (int a = 1; a <= MAXA; a++)
            for (int i = 0; i < n; i++)
                if (c[i] <= a) perm[a] += perm[a - c[i]];
        int mismatches = 0, first_bad = -1;
        for (int a = 1; a <= MAXA; a++) {
            int g = greedy(c, n, a);
            if (g != mn[a]) { mismatches++; if (first_bad < 0) first_bad = a; }
        }
        printf("system {");
        for (int i = 0; i < n; i++) printf("%d%s", c[i], i + 1 < n ? "," : "");
        printf("}: min(63)=%d comb(50)=%llu perm(20)=%llu greedy_fail=%d first=%d\n", mn[63], comb[50], perm[20],
               mismatches, first_bad);
        if (mn[63] >= 0) {
            printf("  63 =");
            for (int a = 63; a > 0; a -= from[a]) printf(" %d", from[a]);
            printf("\n");
        }
    }
    /* count ways for 100 with US coins (known answer 242 with 1,5,10,25,50,100) */
    int us[6] = {1, 5, 10, 25, 50, 100};
    long w[101] = {1};
    for (int i = 0; i < 6; i++) for (int a = us[i]; a <= 100; a++) w[a] += w[a - us[i]];
    CHECK(w[100] == 293);
    printf("ways(100, 1..100 coins)=%ld\n", w[100]);
    return 0;
}
