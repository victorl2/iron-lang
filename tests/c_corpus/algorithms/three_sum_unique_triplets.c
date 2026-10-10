/*
 * title: 3Sum with duplicate skipping
 * topic: algorithms
 * covers: sort plus two pointers, duplicate suppression, triplet enumeration, 3Sum closest, O(n^2) versus brute force
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 1234567u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}
static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

typedef struct {
    int a, b, c;
} Triple;

/* fills out (capacity cap) with unique triples summing to target; returns count */
static int three_sum(const int *a, int n, int target, Triple *out, int cap) {
    int cnt = 0;
    for (int i = 0; i + 2 < n; i++) {
        if (i > 0 && a[i] == a[i - 1])
            continue;
        int lo = i + 1, hi = n - 1;
        while (lo < hi) {
            int s = a[i] + a[lo] + a[hi];
            if (s < target)
                lo++;
            else if (s > target)
                hi--;
            else {
                if (cnt < cap)
                    out[cnt] = (Triple){a[i], a[lo], a[hi]};
                cnt++;
                do
                    lo++;
                while (lo < hi && a[lo] == a[lo - 1]);
                do
                    hi--;
                while (lo < hi && a[hi] == a[hi + 1]);
            }
        }
    }
    return cnt;
}

static int three_sum_closest(const int *a, int n, int target) {
    int best = a[0] + a[1] + a[2];
    for (int i = 0; i + 2 < n; i++) {
        int lo = i + 1, hi = n - 1;
        while (lo < hi) {
            int s = a[i] + a[lo] + a[hi];
            if (abs(s - target) < abs(best - target))
                best = s;
            if (s < target)
                lo++;
            else if (s > target)
                hi--;
            else
                return s;
        }
    }
    return best;
}

static int brute_unique(const int *a, int n, int target) {
    /* a sorted; collect distinct value triples by scanning all index triples */
    static Triple seen[4000];
    int cnt = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            for (int k = j + 1; k < n; k++)
                if (a[i] + a[j] + a[k] == target) {
                    int dup = 0;
                    for (int q = 0; q < cnt && !dup; q++)
                        dup = seen[q].a == a[i] && seen[q].b == a[j] && seen[q].c == a[k];
                    if (!dup)
                        seen[cnt++] = (Triple){a[i], a[j], a[k]};
                }
    return cnt;
}

int main(void) {
    int a[40];
    static Triple out[4000];
    long total = 0;
    for (int trial = 0; trial < 60; trial++) {
        int n = 3 + (int)(rnd() % 30);
        for (int i = 0; i < n; i++)
            a[i] = (int)(rnd() % 21) - 10;
        qsort(a, (size_t)n, sizeof a[0], cmp_int);
        int c = three_sum(a, n, 0, out, 4000);
        if (c != brute_unique(a, n, 0))
            fail("triple count");
        for (int i = 0; i < c; i++) {
            if (out[i].a + out[i].b + out[i].c != 0 || out[i].a > out[i].b || out[i].b > out[i].c)
                fail("triple content");
            if (i > 0 && !(out[i - 1].a < out[i].a ||
                           (out[i - 1].a == out[i].a && out[i - 1].b < out[i].b)))
                fail("uniqueness");
        }
        total += c;
    }
    printf("unique zero-sum triples over 60 trials: %ld\n", total);

    int demo[] = {-4, -1, -1, 0, 1, 2};
    Triple t[16];
    int c = three_sum(demo, 6, 0, t, 16);
    printf("demo zero-sum triples: %d\n", c);
    for (int i = 0; i < c; i++)
        printf("  (%d, %d, %d)\n", t[i].a, t[i].b, t[i].c);

    int cl[] = {-100, -25, 0, 14, 51, 90, 200};
    int targets[] = {0, 50, 100, 300, 1000, -1000};
    for (int i = 0; i < 6; i++)
        printf("closest triple sum to %d: %d\n", targets[i], three_sum_closest(cl, 7, targets[i]));
    return 0;
}
