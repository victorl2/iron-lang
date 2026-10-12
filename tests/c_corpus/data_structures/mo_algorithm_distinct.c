/*
 * title: Mo's algorithm for distinct counts and weighted frequency sums
 * topic: data_structures
 * covers: Mo's algorithm, offline queries, block ordering with odd-even trick, add/remove pointer moves, distinct values, sum of cnt squared times value
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

#define N 2000
#define Q 1500
#define V 300

typedef struct {
    int l, r, id, block;
} Query;

static int bs;

static int cmp_query(const void *x, const void *y) {
    const Query *p = x, *q = y;
    if (p->block != q->block)
        return p->block < q->block ? -1 : 1;
    if (p->r != q->r)
        return (p->block & 1) ? (p->r > q->r ? -1 : 1) : (p->r < q->r ? -1 : 1);
    return p->id - q->id;
}

static int a[N];
static int freq[V];
static int distinct;
static long weighted; /* sum over values v of v * freq[v]^2 */
static long moves;

static void add(int i) {
    int v = a[i];
    weighted += (long)v * (2L * freq[v] + 1);
    if (freq[v]++ == 0)
        distinct++;
    moves++;
}

static void remove_(int i) {
    int v = a[i];
    weighted -= (long)v * (2L * freq[v] - 1);
    if (--freq[v] == 0)
        distinct--;
    moves++;
}

int main(void) {
    for (int i = 0; i < N; i++)
        a[i] = 1 + (int)rnd(V - 1);
    static Query qs[Q];
    for (int i = 0; i < Q; i++) {
        int l = (int)rnd(N), r = (int)rnd(N);
        if (l > r) {
            int t = l;
            l = r;
            r = t;
        }
        qs[i].l = l;
        qs[i].r = r;
        qs[i].id = i;
    }
    bs = 1;
    while (bs * bs < N)
        bs++; /* about sqrt(N) */
    for (int i = 0; i < Q; i++)
        qs[i].block = qs[i].l / bs;
    qsort(qs, Q, sizeof(Query), cmp_query);
    static int ans_d[Q];
    static long ans_w[Q];
    int cl = 0, cr = -1;
    for (int i = 0; i < Q; i++) {
        while (cl > qs[i].l)
            add(--cl);
        while (cr < qs[i].r)
            add(++cr);
        while (cl < qs[i].l)
            remove_(cl++);
        while (cr > qs[i].r)
            remove_(cr--);
        ans_d[qs[i].id] = distinct;
        ans_w[qs[i].id] = weighted;
    }
    long dsum = 0, wsum = 0, naive = 0;
    int maxd = 0;
    for (int i = 0; i < Q; i++) {
        int l = -1, r = -1;
        for (int k = 0; k < Q; k++)
            if (qs[k].id == i) {
                l = qs[k].l;
                r = qs[k].r;
                break;
            }
        int seen[V];
        memset(seen, 0, sizeof seen);
        int d = 0;
        long w = 0;
        for (int j = l; j <= r; j++) {
            if (seen[a[j]]++ == 0)
                d++;
        }
        for (int v = 0; v < V; v++)
            w += (long)v * seen[v] * seen[v];
        check(d == ans_d[i], "distinct count");
        check(w == ans_w[i], "weighted frequency sum");
        dsum += d;
        naive += r - l + 1;
        wsum = (wsum + w) % 1000000007L;
        if (d > maxd)
            maxd = d;
    }
    printf("n=%d queries=%d block=%d\n", N, Q, bs);
    printf("distinct total=%ld max=%d weighted digest=%ld\n", dsum, maxd, wsum);
    printf("pointer moves=%ld (naive would be about %ld)\n", moves, naive);
    return 0;
}
