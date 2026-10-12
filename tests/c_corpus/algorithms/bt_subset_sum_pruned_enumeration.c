/*
 * title: Subset sum enumeration with suffix-sum pruning
 * topic: algorithms
 * covers: backtracking, branch pruning, suffix sums, node counting, DP counting cross-check, duplicate handling
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 111213u;

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

typedef struct {
    const int *a;
    const int *suffix;
    int n;
    int target;
    long nodes;
    long found;
    int use_pruning;
    int keep_first;
    int first[32], first_len;
    int cur[32];
} Ctx;

/* Enumerate all index subsets of a (sorted descending) with the given sum. */
static void go(Ctx *c, int i, int remaining, int depth) {
    c->nodes++;
    if (remaining == 0) {
        if (c->found == 0 && c->keep_first) {
            memcpy(c->first, c->cur, sizeof(int) * (size_t)depth);
            c->first_len = depth;
        }
        c->found++;
        return; /* all elements are positive, so extending a hit can only overshoot */
    }
    if (i == c->n)
        return;
    if (c->use_pruning) {
        if (remaining < 0)
            return;
        if (c->suffix[i] < remaining)
            return; /* even taking everything left cannot reach the target */
    }
    /* take a[i] */
    c->cur[depth] = c->a[i];
    go(c, i + 1, remaining - c->a[i], depth + 1);
    /* skip a[i] */
    go(c, i + 1, remaining, depth);
}

/* Distinct-multiset variant: skip repeated values at the same decision level. */
static void go_distinct(Ctx *c, int start, int remaining, int depth) {
    c->nodes++;
    if (remaining == 0) {
        c->found++;
        return;
    }
    for (int i = start; i < c->n; i++) {
        if (i > start && c->a[i] == c->a[i - 1])
            continue;
        if (c->a[i] > remaining)
            continue; /* array sorted descending: smaller values may still fit */
        if (c->suffix[i] < remaining)
            break;
        c->cur[depth] = c->a[i];
        go_distinct(c, i + 1, remaining - c->a[i], depth + 1);
    }
}

static long dp_count(const int *a, int n, int target) {
    long *ways = calloc((size_t)target + 1, sizeof(long));
    ways[0] = 1;
    for (int i = 0; i < n; i++)
        for (int s = target; s >= a[i]; s--)
            ways[s] += ways[s - a[i]];
    long r = ways[target];
    free(ways);
    return r;
}

static int cmp_desc(const void *x, const void *y) {
    return *(const int *)y - *(const int *)x;
}

static void suffixes(const int *a, int n, int *suf) {
    suf[n] = 0;
    for (int i = n - 1; i >= 0; i--)
        suf[i] = suf[i + 1] + a[i];
}

int main(void) {
    int classic[] = {10, 7, 5, 18, 12, 20, 15};
    int n = 7;
    qsort(classic, (size_t)n, sizeof(int), cmp_desc);
    int suf[33];
    suffixes(classic, n, suf);
    Ctx c = {classic, suf, n, 35, 0, 0, 1, 1, {0}, 0, {0}};
    go(&c, 0, 35, 0);
    printf("classic target 35: %ld subsets, first found:", c.found);
    for (int i = 0; i < c.first_len; i++)
        printf(" %d", c.first[i]);
    printf("\n");
    check(c.found == dp_count(classic, n, 35), "classic count");

    long total_pruned = 0, total_plain = 0, total_found = 0;
    for (int t = 0; t < 40; t++) {
        int m = 8 + (int)(rnd() % 10);
        int a[24];
        int sum = 0;
        for (int i = 0; i < m; i++) {
            a[i] = 1 + (int)(rnd() % 30);
            sum += a[i];
        }
        qsort(a, (size_t)m, sizeof(int), cmp_desc);
        int sf[25];
        suffixes(a, m, sf);
        int target = sum / 3 + (int)(rnd() % 7);
        Ctx pr = {a, sf, m, target, 0, 0, 1, 0, {0}, 0, {0}};
        Ctx pl = {a, sf, m, target, 0, 0, 0, 0, {0}, 0, {0}};
        go(&pr, 0, target, 0);
        go(&pl, 0, target, 0);
        long want = dp_count(a, m, target);
        check(pr.found == want && pl.found == want, "enumeration equals DP count");
        check(pr.nodes <= pl.nodes, "pruning never adds nodes");
        total_pruned += pr.nodes;
        total_plain += pl.nodes;
        total_found += want;
    }
    printf("40 random instances: subsets=%ld nodes plain=%ld pruned=%ld\n", total_found, total_plain, total_pruned);
    /* duplicate-heavy input: distinct multisets, compare with the DP over a value histogram */
    int dup[] = {8, 8, 8, 5, 5, 3, 3, 3, 3, 2, 2, 1};
    int nd = 12;
    qsort(dup, (size_t)nd, sizeof(int), cmp_desc);
    int sd[13];
    suffixes(dup, nd, sd);
    Ctx cd = {dup, sd, nd, 16, 0, 0, 1, 0, {0}, 0, {0}};
    go_distinct(&cd, 0, 16, 0);
    /* distinct multisets via bounded counts */
    int cnt[9] = {0};
    for (int i = 0; i < nd; i++)
        cnt[dup[i]]++;
    long multisets = 0;
    for (int e8 = 0; e8 <= cnt[8]; e8++)
        for (int e5 = 0; e5 <= cnt[5]; e5++)
            for (int e3 = 0; e3 <= cnt[3]; e3++)
                for (int e2 = 0; e2 <= cnt[2]; e2++)
                    for (int e1 = 0; e1 <= cnt[1]; e1++)
                        multisets += (8 * e8 + 5 * e5 + 3 * e3 + 2 * e2 + e1 == 16);
    printf("duplicates: %ld distinct multisets sum to 16 (%ld index subsets)\n", cd.found, dp_count(dup, nd, 16));
    check(cd.found == multisets, "distinct multiset count");
    return 0;
}
