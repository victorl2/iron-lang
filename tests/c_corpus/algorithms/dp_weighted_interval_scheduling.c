/*
 * title: Weighted interval scheduling with predecessor binary search
 * topic: algorithms
 * covers: dynamic programming, weighted interval scheduling, sort by end time, binary search predecessor, reconstruction, count of optimal schedules, exhaustive subset check
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

#define N 16

typedef struct { int s, e, w, id; } Job;

static int cmp_job(const void *a, const void *b) {
    const Job *x = a, *y = b;
    if (x->e != y->e) return x->e - y->e;
    if (x->s != y->s) return x->s - y->s;
    return x->id - y->id;
}

int main(void) {
    for (int t = 0; t < 8; t++) {
        int n = 6 + rr(N - 6);
        Job j[N];
        for (int i = 0; i < n; i++) {
            j[i].s = rr(60);
            j[i].e = j[i].s + 1 + rr(20);
            j[i].w = 1 + rr(30);
            j[i].id = i;
        }
        qsort(j, (size_t)n, sizeof(Job), cmp_job);
        int p[N]; /* last job (index) ending at or before s, -1 if none */
        for (int i = 0; i < n; i++) {
            int lo = 0, hi = i; /* count of jobs among [0,i) with e <= s */
            while (lo < hi) { int m = (lo + hi) / 2; if (j[m].e <= j[i].s) lo = m + 1; else hi = m; }
            p[i] = lo - 1;
        }
        int best[N + 1];
        long ways[N + 1];
        best[0] = 0; ways[0] = 1;
        for (int i = 1; i <= n; i++) {
            int take = j[i - 1].w + best[p[i - 1] + 1], skip = best[i - 1];
            best[i] = take > skip ? take : skip;
            ways[i] = 0;
            if (take == best[i]) ways[i] += ways[p[i - 1] + 1];
            if (skip == best[i]) ways[i] += ways[i - 1];
        }
        /* ways counts (take/skip) decision paths; identical value ties are distinct schedules only if sets differ */
        int chosen[N], nc = 0;
        for (int i = n; i > 0;) {
            int take = j[i - 1].w + best[p[i - 1] + 1];
            if (take >= best[i - 1] && take == best[i]) { chosen[nc++] = i - 1; i = p[i - 1] + 1; }
            else i--;
        }
        int total = 0;
        for (int a = 0; a < nc; a++) {
            total += j[chosen[a]].w;
            for (int b = a + 1; b < nc; b++) CHECK(j[chosen[a]].s >= j[chosen[b]].e || j[chosen[b]].s >= j[chosen[a]].e);
        }
        CHECK(total == best[n]);
        int bb = 0;
        long nopt = 0;
        for (unsigned m = 0; m < (1u << n); m++) {
            int ok = 1, sum = 0;
            for (int a = 0; a < n && ok; a++) {
                if (!(m >> a & 1)) continue;
                sum += j[a].w;
                for (int b = a + 1; b < n; b++)
                    if ((m >> b & 1) && j[a].s < j[b].e && j[b].s < j[a].e) { ok = 0; break; }
            }
            if (!ok) continue;
            if (sum > bb) { bb = sum; nopt = 1; } else if (sum == bb) nopt++;
        }
        CHECK(bb == best[n]);
        CHECK(ways[n] == nopt);
        printf("case %d: n=%2d best=%3d schedules_with_best=%ld picked=%d\n", t, n, bb, nopt, nc);
    }
    return 0;
}
