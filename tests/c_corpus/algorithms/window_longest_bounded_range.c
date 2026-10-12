/*
 * title: Longest subarray with bounded max-min
 * topic: algorithms
 * covers: variable-size sliding window, paired monotonic deques (min and max), shrink-on-violation, array-backed deques
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 6006u;
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

/* longest window with max-min <= limit; reports the leftmost such best window */
static int longest(const int *a, int n, int limit, int *start) {
    int *maxq = malloc((size_t)n * sizeof(int)), *minq = malloc((size_t)n * sizeof(int));
    if (!maxq || !minq)
        fail("alloc");
    int mh = 0, mt = 0, nh = 0, nt = 0; /* head/tail of each queue (indices into a) */
    int left = 0, best = 0;
    *start = 0;
    for (int right = 0; right < n; right++) {
        while (mt > mh && a[maxq[mt - 1]] <= a[right])
            mt--;
        maxq[mt++] = right;
        while (nt > nh && a[minq[nt - 1]] >= a[right])
            nt--;
        minq[nt++] = right;
        while (a[maxq[mh]] - a[minq[nh]] > limit) {
            left++;
            if (maxq[mh] < left)
                mh++;
            if (minq[nh] < left)
                nh++;
        }
        if (right - left + 1 > best) {
            best = right - left + 1;
            *start = left;
        }
    }
    free(maxq);
    free(minq);
    return best;
}

static int brute(const int *a, int n, int limit, int *start) {
    int best = 0;
    *start = 0;
    for (int i = 0; i < n; i++) {
        int lo = a[i], hi = a[i];
        for (int j = i; j < n; j++) {
            if (a[j] < lo)
                lo = a[j];
            if (a[j] > hi)
                hi = a[j];
            if (hi - lo > limit)
                break;
            if (j - i + 1 > best) {
                best = j - i + 1;
                *start = i;
            }
        }
    }
    return best;
}

int main(void) {
    int a[300];
    for (int trial = 0; trial < 300; trial++) {
        int n = 1 + (int)(rnd() % 250);
        int spread = 1 + (int)(rnd() % 100);
        int limit = (int)(rnd() % 30);
        int v = 0;
        for (int i = 0; i < n; i++) {
            v += (int)(rnd() % (unsigned)(2 * spread + 1)) - spread;
            a[i] = v;
        }
        int s1, s2;
        int b1 = longest(a, n, limit, &s1), b2 = brute(a, n, limit, &s2);
        if (b1 != b2 || s1 != s2)
            fail("longest window");
    }
    printf("300 random walks verified\n");

    int demo[] = {8, 2, 4, 7};
    int s;
    printf("[8,2,4,7] limit 4 -> %d", longest(demo, 4, 4, &s));
    printf(" at %d\n", s);
    int demo2[] = {10, 1, 2, 4, 7, 2};
    printf("[10,1,2,4,7,2] limit 5 -> %d", longest(demo2, 6, 5, &s));
    printf(" at %d\n", s);
    int demo3[] = {4, 2, 2, 2, 4, 4, 2, 2};
    printf("[4,2,2,2,4,4,2,2] limit 0 -> %d", longest(demo3, 8, 0, &s));
    printf(" at %d\n", s);
    return 0;
}
