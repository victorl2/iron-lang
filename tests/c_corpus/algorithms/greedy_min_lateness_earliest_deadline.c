/*
 * title: Minimising maximum lateness with earliest deadline first
 * topic: algorithms
 * covers: scheduling, exchange argument, EDD ordering, permutation brute force, comparison with other orderings
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int id, p, d; /* processing time, deadline */
} Job;

static unsigned st = 13579u;

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

static int cmp_edd(const void *pa, const void *pb) {
    const Job *a = pa, *b = pb;
    if (a->d != b->d)
        return a->d - b->d;
    return a->id - b->id;
}

static int cmp_spt(const void *pa, const void *pb) {
    const Job *a = pa, *b = pb;
    if (a->p != b->p)
        return a->p - b->p;
    return a->id - b->id;
}

static int cmp_slack(const void *pa, const void *pb) {
    const Job *a = pa, *b = pb;
    int sa = a->d - a->p, sb = b->d - b->p;
    if (sa != sb)
        return sa - sb;
    return a->id - b->id;
}

static int max_lateness(const Job *j, int n) {
    int t = 0, worst = -1000000;
    for (int i = 0; i < n; i++) {
        t += j[i].p;
        int late = t - j[i].d;
        if (late > worst)
            worst = late;
    }
    return worst;
}

static int best_perm;

static void permute(Job *j, int k, int n) {
    if (k == n) {
        int l = max_lateness(j, n);
        if (l < best_perm)
            best_perm = l;
        return;
    }
    for (int i = k; i < n; i++) {
        Job t = j[k];
        j[k] = j[i];
        j[i] = t;
        permute(j, k + 1, n);
        t = j[k];
        j[k] = j[i];
        j[i] = t;
    }
}

int main(void) {
    Job classic[] = {{1, 3, 6}, {2, 2, 8}, {3, 1, 9}, {4, 4, 9}, {5, 3, 14}, {6, 2, 15}};
    qsort(classic, 6, sizeof(Job), cmp_edd);
    printf("classic EDD order:");
    int t = 0;
    for (int i = 0; i < 6; i++) {
        printf(" j%d[%d-%d,due %d]", classic[i].id, t, t + classic[i].p, classic[i].d);
        t += classic[i].p;
    }
    printf("\nmax lateness = %d\n", max_lateness(classic, 6));
    check(max_lateness(classic, 6) == 1, "classic lateness");
    int edd_wins_spt = 0, edd_wins_slack = 0, ties_all = 0, sum_opt = 0, nonpositive = 0;
    for (int trial = 0; trial < 250; trial++) {
        int n = 2 + (int)(rnd() % 6);
        Job a[8], b[8], c[8], d[8];
        for (int i = 0; i < n; i++)
            a[i] = (Job){i, 1 + (int)(rnd() % 9), 1 + (int)(rnd() % 40)};
        for (int i = 0; i < n; i++)
            b[i] = c[i] = d[i] = a[i];
        best_perm = 1 << 30;
        permute(d, 0, n);
        qsort(a, (size_t)n, sizeof(Job), cmp_edd);
        int edd = max_lateness(a, n);
        check(edd == best_perm, "EDD is optimal");
        qsort(b, (size_t)n, sizeof(Job), cmp_spt);
        qsort(c, (size_t)n, sizeof(Job), cmp_slack);
        int spt = max_lateness(b, n), slack = max_lateness(c, n);
        check(spt >= edd && slack >= edd, "other orders are never better");
        edd_wins_spt += spt > edd;
        edd_wins_slack += slack > edd;
        ties_all += spt == edd && slack == edd;
        sum_opt += edd;
        nonpositive += edd <= 0;
    }
    printf("250 instances: sum of optimal max lateness=%d, on-time schedules=%d\n", sum_opt, nonpositive);
    printf("EDD strictly beat shortest-first %d times, slack-first %d times, all tied %d times\n", edd_wins_spt,
           edd_wins_slack, ties_all);
    Job sl[] = {{0, 1, 100}, {1, 10, 10}};
    qsort(sl, 2, sizeof(Job), cmp_spt);
    printf("SPT trap: lateness %d, ", max_lateness(sl, 2));
    qsort(sl, 2, sizeof(Job), cmp_edd);
    printf("EDD lateness %d\n", max_lateness(sl, 2));
    return 0;
}
