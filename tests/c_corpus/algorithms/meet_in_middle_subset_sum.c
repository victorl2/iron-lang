/*
 * title: Meet in the middle subset sum
 * topic: algorithms
 * covers: meet in the middle, half-subset enumeration by gray-free bitmasks, sorted merge generation, two-pointer join, closest subset sum
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned long long rs = 1618033988ull;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 24);
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

/* all 2^n subset sums of a[0..n), produced already sorted by merging */
static long *sorted_sums(const long *a, int n, size_t *count) {
    size_t cap = (size_t)1 << n;
    long *cur = malloc(cap * sizeof(long)), *tmp = malloc(cap * sizeof(long));
    if (!cur || !tmp)
        fail("alloc");
    size_t len = 1;
    cur[0] = 0;
    for (int i = 0; i < n; i++) {
        /* merge cur with cur + a[i] */
        size_t x = 0, y = 0, o = 0;
        while (x < len || y < len) {
            if (y >= len || (x < len && cur[x] <= cur[y] + a[i]))
                tmp[o++] = cur[x++];
            else
                tmp[o++] = cur[y++] + a[i];
        }
        len *= 2;
        long *t = cur;
        cur = tmp;
        tmp = t;
    }
    free(tmp);
    *count = len;
    return cur;
}

/* best subset sum <= target using meet in the middle */
static long best_at_most(const long *a, int n, long target, size_t *pairs_tested) {
    int h = n / 2;
    size_t nl, nr;
    long *L = sorted_sums(a, h, &nl), *R = sorted_sums(a + h, n - h, &nr);
    long best = 0;
    size_t j = nr;
    *pairs_tested = 0;
    for (size_t i = 0; i < nl; i++) {
        if (L[i] > target)
            break;
        while (j > 0 && L[i] + R[j - 1] > target)
            j--;
        (*pairs_tested)++;
        if (j > 0 && L[i] + R[j - 1] > best)
            best = L[i] + R[j - 1];
    }
    free(L);
    free(R);
    return best;
}

/* number of subsets with sum exactly target */
static long count_exact(const long *a, int n, long target) {
    int h = n / 2;
    size_t nl, nr;
    long *L = sorted_sums(a, h, &nl), *R = sorted_sums(a + h, n - h, &nr);
    long total = 0;
    size_t j = nr;
    for (size_t i = 0; i < nl; i++) {
        long need = target - L[i];
        /* count equal values in R via two scans (R sorted ascending) */
        while (j > 0 && R[j - 1] > need)
            j--;
        size_t k = j;
        while (k > 0 && R[k - 1] == need) {
            total++;
            k--;
        }
    }
    free(L);
    free(R);
    return total;
}

static long brute_best(const long *a, int n, long target) {
    long best = 0;
    for (unsigned m = 0; m < (1u << n); m++) {
        long s = 0;
        for (int i = 0; i < n; i++)
            if (m >> i & 1)
                s += a[i];
        if (s <= target && s > best)
            best = s;
    }
    return best;
}
static long brute_count(const long *a, int n, long target) {
    long c = 0;
    for (unsigned m = 0; m < (1u << n); m++) {
        long s = 0;
        for (int i = 0; i < n; i++)
            if (m >> i & 1)
                s += a[i];
        c += s == target;
    }
    return c;
}

int main(void) {
    long a[40];
    for (int trial = 0; trial < 20; trial++) {
        int n = 4 + (int)(rnd() % 12);
        for (int i = 0; i < n; i++)
            a[i] = 1 + (long)(rnd() % 100);
        long target = (long)(rnd() % 500);
        size_t pt;
        if (best_at_most(a, n, target, &pt) != brute_best(a, n, target))
            fail("best_at_most");
        long ct = (long)(rnd() % 200);
        if (count_exact(a, n, ct) != brute_count(a, n, ct))
            fail("count_exact");
    }
    printf("20 random instances verified against 2^n enumeration\n");

    /* n = 36: 2^36 subsets is too many, but 2 * 2^18 is easy */
    int n = 36;
    long total = 0;
    for (int i = 0; i < n; i++) {
        a[i] = 1000000 + (long)(rnd() % 9000000);
        total += a[i];
    }
    long target = total / 2;
    size_t pt;
    long best = best_at_most(a, n, target, &pt);
    printf("n=36 total=%ld target=%ld best=%ld gap=%ld\n", total, target, best, target - best);
    printf("pairs tested: %zu (vs 2^36 subsets)\n", pt);
    if (best > target)
        fail("over target");
    return 0;
}
