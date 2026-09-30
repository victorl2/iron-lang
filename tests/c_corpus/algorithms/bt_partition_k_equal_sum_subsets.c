/*
 * title: Partition into k subsets of equal sum
 * topic: algorithms
 * covers: backtracking, bucket filling, symmetry pruning, descending sort, bitmask DP cross-check, node counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 8128u;

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

static int cmp_desc(const void *a, const void *b) {
    return *(const int *)b - *(const int *)a;
}

typedef struct {
    const int *a;
    int n, k, target;
    int bucket[16];
    int assign[24];
    long nodes;
    int prune;
} Ctx;

/* Assign item i to some bucket. With pruning: skip buckets whose current sum equals a
 * previously tried bucket at this level (identical states), and stop at the first empty bucket. */
static int assign(Ctx *c, int i) {
    c->nodes++;
    if (i == c->n)
        return 1;
    for (int b = 0; b < c->k; b++) {
        if (c->bucket[b] + c->a[i] > c->target)
            continue;
        if (c->prune) {
            int dup = 0;
            for (int p = 0; p < b; p++)
                if (c->bucket[p] == c->bucket[b])
                    dup = 1;
            if (dup)
                continue;
        }
        c->bucket[b] += c->a[i];
        c->assign[i] = b;
        if (assign(c, i + 1))
            return 1;
        c->bucket[b] -= c->a[i];
    }
    return 0;
}

/* Bitmask DP: reach[mask] = sum of mask mod target reachable by filling buckets in order. */
static int dp_feasible(const int *a, int n, int k, int target) {
    unsigned full = (1u << n) - 1;
    signed char *ok = calloc((size_t)full + 1, 1);
    int *sum = calloc((size_t)full + 1, sizeof(int));
    ok[0] = 1;
    for (unsigned m = 0; m <= full; m++) {
        if (!ok[m])
            continue;
        int rem = sum[m] % target;
        for (int i = 0; i < n; i++) {
            if (m >> i & 1u)
                continue;
            if (rem + a[i] > target)
                continue;
            unsigned nm = m | (1u << i);
            if (!ok[nm]) {
                ok[nm] = 1;
                sum[nm] = sum[m] + a[i];
            }
        }
    }
    int r = ok[full];
    (void)k;
    free(ok);
    free(sum);
    return r;
}

static int solve(int *a, int n, int k, int prune, long *nodes, int *layout) {
    int total = 0;
    for (int i = 0; i < n; i++)
        total += a[i];
    if (total % k != 0) {
        *nodes = 0;
        return 0;
    }
    qsort(a, (size_t)n, sizeof(int), cmp_desc);
    Ctx c;
    memset(&c, 0, sizeof c);
    c.a = a;
    c.n = n;
    c.k = k;
    c.target = total / k;
    c.prune = prune;
    if (a[0] > c.target) {
        *nodes = 0;
        return 0;
    }
    int r = assign(&c, 0);
    *nodes = c.nodes;
    if (r && layout)
        memcpy(layout, c.assign, sizeof(int) * (size_t)n);
    return r;
}

int main(void) {
    int classic[] = {4, 3, 2, 3, 5, 2, 1};
    int layout[24];
    long nodes;
    int r = solve(classic, 7, 4, 1, &nodes, layout);
    printf("classic {4,3,2,3,5,2,1} into 4: %s (nodes=%ld)\n", r ? "yes" : "no", nodes);
    check(r == 1, "classic is feasible");
    for (int b = 0; b < 4; b++) {
        printf("  bucket %d:", b);
        int s = 0;
        for (int i = 0; i < 7; i++)
            if (layout[i] == b) {
                printf(" %d", classic[i]);
                s += classic[i];
            }
        printf("  (sum %d)\n", s);
        check(s == 5, "bucket sum equals target");
    }
    int bad[] = {1, 2, 3, 4};
    r = solve(bad, 4, 3, 1, &nodes, NULL);
    printf("{1,2,3,4} into 3: %s\n", r ? "yes" : "no");
    check(r == 0, "sum not divisible");
    int feasible = 0, infeasible = 0;
    long nodes_plain = 0, nodes_pruned = 0;
    for (int t = 0; t < 250; t++) {
        int n = 4 + (int)(rnd() % 11);
        int k = 2 + (int)(rnd() % 4);
        int a[24], b[24], c[24];
        int total = 0;
        for (int i = 0; i < n; i++) {
            a[i] = 1 + (int)(rnd() % 9);
            total += a[i];
        }
        /* nudge the total to a multiple of k about half the time */
        if (t & 1)
            a[0] += (k - total % k) % k;
        memcpy(b, a, sizeof a);
        memcpy(c, a, sizeof a);
        long n1, n2;
        int r1 = solve(b, n, k, 1, &n1, NULL);
        int r2 = solve(c, n, k, 0, &n2, NULL);
        check(r1 == r2, "pruned and plain agree");
        int sum = 0;
        for (int i = 0; i < n; i++)
            sum += a[i];
        if (sum % k == 0 && a[0] <= sum / k && n <= 16) {
            int mx = 0;
            for (int i = 0; i < n; i++)
                if (a[i] > mx)
                    mx = a[i];
            if (mx <= sum / k)
                check(r1 == dp_feasible(a, n, k, sum / k), "matches bitmask DP");
        }
        check(n2 == 0 || n1 <= n2, "pruning never adds nodes");
        nodes_plain += n2;
        nodes_pruned += n1;
        r1 ? feasible++ : infeasible++;
    }
    printf("250 random multisets: feasible=%d infeasible=%d\n", feasible, infeasible);
    printf("nodes without symmetry pruning=%ld with=%ld\n", nodes_plain, nodes_pruned);
    check(nodes_pruned < nodes_plain, "pruning helps overall");
    return 0;
}
