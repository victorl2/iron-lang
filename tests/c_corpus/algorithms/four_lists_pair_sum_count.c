/*
 * title: Count zero-sum quadruples across four lists
 * topic: algorithms
 * covers: meet in the middle by pair sums, sorted pair-sum arrays, run-length two-pointer join, 64-bit counts
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 11235813u;
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
static int cmp_long(const void *x, const void *y) {
    long a = *(const long *)x, b = *(const long *)y;
    return (a > b) - (a < b);
}

/* number of (i,j,k,l) with A[i]+B[j]+C[k]+D[l] == target */
static long four_sum_count(const int *A, const int *B, const int *C, const int *D, int n,
                           long target) {
    size_t m = (size_t)n * n;
    long *ab = malloc(m * sizeof(long)), *cd = malloc(m * sizeof(long));
    if (!ab || !cd)
        fail("alloc");
    size_t p = 0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            ab[p] = (long)A[i] + B[j];
            cd[p] = (long)C[i] + D[j];
            p++;
        }
    qsort(ab, m, sizeof(long), cmp_long);
    qsort(cd, m, sizeof(long), cmp_long);
    long total = 0;
    size_t i = 0, j = m;
    while (i < m && j > 0) {
        long s = ab[i] + cd[j - 1];
        if (s < target)
            i++;
        else if (s > target)
            j--;
        else {
            long x = ab[i], y = cd[j - 1];
            long ci = 0, cj = 0;
            while (i < m && ab[i] == x) {
                ci++;
                i++;
            }
            while (j > 0 && cd[j - 1] == y) {
                cj++;
                j--;
            }
            total += ci * cj;
        }
    }
    free(ab);
    free(cd);
    return total;
}

int main(void) {
    int A[64], B[64], C[64], D[64];
    for (int trial = 0; trial < 40; trial++) {
        int n = 1 + (int)(rnd() % 14);
        int range = 3 + (int)(rnd() % 12);
        for (int i = 0; i < n; i++) {
            A[i] = (int)(rnd() % (unsigned)range) - range / 2;
            B[i] = (int)(rnd() % (unsigned)range) - range / 2;
            C[i] = (int)(rnd() % (unsigned)range) - range / 2;
            D[i] = (int)(rnd() % (unsigned)range) - range / 2;
        }
        long target = (long)(rnd() % 5) - 2;
        long want = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                for (int k = 0; k < n; k++)
                    for (int l = 0; l < n; l++)
                        want += (long)A[i] + B[j] + C[k] + D[l] == target;
        if (four_sum_count(A, B, C, D, n, target) != want)
            fail("four sum");
    }
    printf("40 random instances verified against O(n^4)\n");

    int a1[] = {1, 2}, b1[] = {-2, -1}, c1[] = {-1, 2}, d1[] = {0, 2};
    printf("classic example: %ld\n", four_sum_count(a1, b1, c1, d1, 2, 0));

    int n = 300;
    static int P[300], Q[300], R[300], S[300];
    for (int i = 0; i < n; i++) {
        P[i] = (int)(rnd() % 2001) - 1000;
        Q[i] = (int)(rnd() % 2001) - 1000;
        R[i] = (int)(rnd() % 2001) - 1000;
        S[i] = (int)(rnd() % 2001) - 1000;
    }
    for (long t = -100; t <= 100; t += 100)
        printf("n=300, target %ld: %ld quadruples\n", t, four_sum_count(P, Q, R, S, n, t));
    int zeros[128] = {0};
    printf("all zeros n=128: %ld (expect 128^4)\n", four_sum_count(zeros, zeros, zeros, zeros, 128, 0));
    return 0;
}
