/*
 * title: Job sequencing with deadlines using union-find slots
 * topic: algorithms
 * covers: profit greedy, deadline scheduling, union-find free slot search, matroid exchange, brute-force cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int id, deadline, profit;
} Job;

static unsigned st = 65537u;

static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static int cmp_profit(const void *pa, const void *pb) {
    const Job *a = pa, *b = pb;
    if (a->profit != b->profit)
        return b->profit - a->profit;
    if (a->deadline != b->deadline)
        return a->deadline - b->deadline;
    return a->id - b->id;
}

static int *parent;

static int find(int x) {
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

/* slot[t] (1-based) receives the job id run in time unit t, or -1. */
static int schedule(Job *jobs, int n, int maxd, int *slot, long *finds) {
    qsort(jobs, (size_t)n, sizeof(Job), cmp_profit);
    parent = malloc(sizeof(int) * (size_t)(maxd + 1));
    for (int i = 0; i <= maxd; i++) {
        parent[i] = i;
        slot[i] = -1;
    }
    int total = 0;
    for (int i = 0; i < n; i++) {
        int d = jobs[i].deadline > maxd ? maxd : jobs[i].deadline;
        (*finds)++;
        int t = find(d); /* latest free time unit not after the deadline; 0 means none */
        if (t > 0) {
            slot[t] = jobs[i].id;
            parent[t] = t - 1;
            total += jobs[i].profit;
        }
    }
    free(parent);
    return total;
}

static int cmp_deadline(const void *pa, const void *pb) {
    const Job *a = pa, *b = pb;
    return a->deadline != b->deadline ? a->deadline - b->deadline : a->id - b->id;
}

/* Exhaustive: a subset is feasible iff EDF order meets every deadline (unit-time jobs). */
static int brute(const Job *jobs, int n) {
    int best = 0;
    for (unsigned m = 0; m < (1u << n); m++) {
        Job sel[16];
        int k = 0, sum = 0;
        for (int i = 0; i < n; i++)
            if (m >> i & 1) {
                sel[k++] = jobs[i];
                sum += jobs[i].profit;
            }
        qsort(sel, (size_t)k, sizeof(Job), cmp_deadline);
        int ok = 1;
        for (int i = 0; i < k; i++)
            if (i + 1 > sel[i].deadline)
                ok = 0;
        if (ok && sum > best)
            best = sum;
    }
    return best;
}

int main(void) {
    Job classic[] = {{0, 2, 100}, {1, 1, 19}, {2, 2, 27}, {3, 1, 25}, {4, 3, 15}};
    int slot[64];
    long finds = 0;
    int p = schedule(classic, 5, 3, slot, &finds);
    printf("classic: profit=%d slots:", p);
    for (int t = 1; t <= 3; t++)
        printf(" t%d=j%d", t, slot[t]);
    printf("\n");
    check(p == 142, "classic profit");

    long total_finds = 0;
    int total_profit = 0, full = 0;
    for (int t = 0; t < 300; t++) {
        int n = 2 + (int)(rnd() % 11);
        Job jobs[16], copy[16];
        for (int i = 0; i < n; i++)
            jobs[i] = (Job){i, 1 + (int)(rnd() % (unsigned)(1 + n / 2 + t % 4)), 1 + (int)(rnd() % 90)};
        for (int i = 0; i < n; i++)
            copy[i] = jobs[i];
        int maxd = 0;
        for (int i = 0; i < n; i++)
            if (jobs[i].deadline > maxd)
                maxd = jobs[i].deadline;
        int got = schedule(jobs, n, maxd, slot, &total_finds);
        check(got == brute(copy, n), "greedy equals exhaustive optimum");
        int used = 0, sum = 0;
        for (int u = 1; u <= maxd; u++)
            if (slot[u] >= 0) {
                used++;
                check(u <= copy[slot[u]].deadline, "job runs before its deadline");
                sum += copy[slot[u]].profit;
            }
        check(sum == got, "slot table sums to reported profit");
        full += used == maxd;
        total_profit += got;
    }
    printf("300 instances: total profit=%d, fully packed horizons=%d, find calls=%ld\n", total_profit, full,
           total_finds);
    /* larger instance for a stress pass with no brute force */
    Job big[400];
    int bs[512];
    long bf = 0;
    long sumall = 0;
    for (int i = 0; i < 400; i++) {
        big[i] = (Job){i, 1 + (int)(rnd() % 150), 1 + (int)(rnd() % 1000)};
        sumall += big[i].profit;
    }
    int bp = schedule(big, 400, 150, bs, &bf);
    int filled = 0;
    for (int u = 1; u <= 150; u++)
        filled += bs[u] >= 0;
    printf("large: profit=%d of %ld offered, filled=%d/150\n", bp, sumall, filled);
    check(filled <= 150, "no more jobs than time units");
    return 0;
}
