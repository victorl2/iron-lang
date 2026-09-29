/*
 * title: Range mode queries with block table and position lists
 * topic: data_structures
 * covers: range mode, sqrt blocks, precomputed block-pair modes, per-value position lists, binary search counting, smallest-value tie-break
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define N 1200
#define V 40
#define B 32
#define NB ((N + B - 1) / B)

typedef struct {
    int val, cnt;
} Mode;

static int a[N];
static int *pos[V];
static int npos[V];
static Mode bb[NB][NB];

static int better(Mode x, Mode y) { /* is x better than y? more frequent, then smaller value */
    return x.cnt > y.cnt || (x.cnt == y.cnt && x.val < y.val);
}

static int count_in(int v, int l, int r) { /* occurrences of v in [l,r] via binary search */
    int lo = 0, hi = npos[v];
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (pos[v][m] < l)
            lo = m + 1;
        else
            hi = m;
    }
    int first = lo;
    hi = npos[v];
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (pos[v][m] <= r)
            lo = m + 1;
        else
            hi = m;
    }
    return lo - first;
}

static void precompute(void) {
    for (int v = 0; v < V; v++)
        npos[v] = 0;
    for (int i = 0; i < N; i++)
        npos[a[i]]++;
    for (int v = 0; v < V; v++) {
        pos[v] = malloc(sizeof(int) * (size_t)(npos[v] + 1));
        check(pos[v] != NULL, "alloc");
        npos[v] = 0;
    }
    for (int i = 0; i < N; i++)
        pos[a[i]][npos[a[i]]++] = i;
    for (int bi = 0; bi < NB; bi++) {
        int cnt[V];
        memset(cnt, 0, sizeof cnt);
        Mode best = {0, 0};
        for (int i = bi * B; i < N; i++) {
            Mode cur = {a[i], ++cnt[a[i]]};
            if (better(cur, best))
                best = cur;
            if ((i + 1) % B == 0 || i == N - 1)
                bb[bi][i / B] = best;
        }
    }
}

static long candidates_checked;

static Mode query(int l, int r) {
    int bl = l / B, br = r / B;
    Mode best = {V, 0};
    if (br - bl <= 1) {
        for (int i = l; i <= r; i++) {
            Mode c = {a[i], count_in(a[i], l, r)};
            candidates_checked++;
            if (better(c, best))
                best = c;
        }
        return best;
    }
    best = bb[bl + 1][br - 1];
    for (int i = l; i < (bl + 1) * B; i++) {
        Mode c = {a[i], count_in(a[i], l, r)};
        candidates_checked++;
        if (better(c, best))
            best = c;
    }
    for (int i = br * B; i <= r; i++) {
        Mode c = {a[i], count_in(a[i], l, r)};
        candidates_checked++;
        if (better(c, best))
            best = c;
    }
    return best;
}

int main(void) {
    for (int i = 0; i < N; i++) {
        unsigned skew = rnd(3);
        a[i] = skew == 0 ? (int)rnd(3) : (int)rnd(V); /* values 0..2 are heavy hitters */
    }
    precompute();
    long vsum = 0, csum = 0;
    for (int q = 0; q < 3000; q++) {
        int l = (int)rnd(N), r = (int)rnd(N);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        int cnt[V];
        memset(cnt, 0, sizeof cnt);
        for (int i = l; i <= r; i++)
            cnt[a[i]]++;
        Mode want = {0, cnt[0]};
        for (int v = 1; v < V; v++)
            if (cnt[v] > want.cnt) {
                want.val = v;
                want.cnt = cnt[v];
            }
        Mode got = query(l, r);
        check(got.val == want.val && got.cnt == want.cnt, "range mode");
        vsum += got.val;
        csum += got.cnt;
    }
    printf("n=%d values=%d block=%d\n", N, V, B);
    printf("mode value sum=%ld count sum=%ld candidates checked=%ld\n", vsum, csum, candidates_checked);
    Mode all = query(0, N - 1);
    printf("global mode %d occurs %d times\n", all.val, all.cnt);
    for (int v = 0; v < V; v++)
        free(pos[v]);
    return 0;
}
