/*
 * title: Bin packing and job partition by bitmask DP
 * topic: algorithms
 * covers: dynamic programming, bitmask, min bins, (bins, fill) pair minimisation, subset feasibility, submask enumeration, first-fit-decreasing comparison
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

#define N 14

static int wt[N];

static int cmp_desc(const void *a, const void *b) { return *(const int *)b - *(const int *)a; }

static int ffd(int n, int cap) {
    int s[N], bins[N] = {0}, nb = 0;
    memcpy(s, wt, sizeof(int) * (size_t)n);
    qsort(s, (size_t)n, sizeof(int), cmp_desc);
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (j < nb && bins[j] + s[i] > cap) j++;
        if (j == nb) nb++;
        bins[j] += s[i];
    }
    return nb;
}

int main(void) {
    for (int t = 0; t < 7; t++) {
        int n = 6 + t * 1 + rr(2), cap = 30 + rr(30);
        if (n > N) n = N;
        for (int i = 0; i < n; i++) wt[i] = 3 + rr(cap - 8);
        int full = (1 << n) - 1;
        static int bins[1 << N], fill[1 << N], fsum[1 << N];
        for (int m = 0; m <= full; m++) { fsum[m] = 0; for (int i = 0; i < n; i++) if (m >> i & 1) fsum[m] += wt[i]; }
        bins[0] = 1; fill[0] = 0;
        for (int m = 1; m <= full; m++) { bins[m] = 1 << 20; fill[m] = 0; }
        for (int m = 0; m < full; m++)
            for (int i = 0; i < n; i++) {
                if (m >> i & 1) continue;
                int nm = m | 1 << i, nb, nf;
                if (fill[m] + wt[i] <= cap) { nb = bins[m]; nf = fill[m] + wt[i]; }
                else { nb = bins[m] + 1; nf = wt[i]; }
                if (nb < bins[nm] || (nb == bins[nm] && nf < fill[nm])) { bins[nm] = nb; fill[nm] = nf; }
            }
        int best = bins[full];
        /* alternative: min partitions of the set into subsets each with sum <= cap, via submask enumeration */
        static int part[1 << N];
        part[0] = 0;
        for (int m = 1; m <= full; m++) {
            part[m] = 1 << 20;
            int low = m & -m;
            for (int s = m; s; s = (s - 1) & m) {
                if (!(s & low) || fsum[s] > cap) continue;
                if (part[m ^ s] + 1 < part[m]) part[m] = part[m ^ s] + 1;
            }
        }
        CHECK(part[full] == best);
        int lower = (fsum[full] + cap - 1) / cap;
        int heur = ffd(n, cap);
        CHECK(best >= lower && heur >= best);
        printf("case %d: n=%d cap=%d total=%d lower_bound=%d optimal=%d ffd=%d\n", t, n, cap, fsum[full], lower, best, heur);
    }
    return 0;
}
