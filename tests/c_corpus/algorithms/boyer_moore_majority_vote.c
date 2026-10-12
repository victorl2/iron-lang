/*
 * title: Boyer-Moore majority vote and its generalisation
 * topic: algorithms
 * covers: majority element selection, pairing-off argument, verification pass, n/3 candidates, Misra-Gries with k counters
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 424u * 1000u + 24u;
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

/* returns 1 and sets *out when some value occurs > n/2 times */
static int majority(const int *a, int n, int *out) {
    int cand = 0, votes = 0;
    for (int i = 0; i < n; i++) {
        if (votes == 0) {
            cand = a[i];
            votes = 1;
        } else if (a[i] == cand)
            votes++;
        else
            votes--;
    }
    int c = 0;
    for (int i = 0; i < n; i++)
        c += a[i] == cand;
    if (c * 2 > n) {
        *out = cand;
        return 1;
    }
    return 0;
}

/* Misra-Gries: values occurring more than n/(k+1) times survive among k counters */
typedef struct {
    int val, cnt;
} Counter;

static int frequent(const int *a, int n, int k, int *out) {
    Counter c[8] = {{0, 0}};
    for (int i = 0; i < n; i++) {
        int hit = -1, free_slot = -1;
        for (int j = 0; j < k; j++) {
            if (c[j].cnt > 0 && c[j].val == a[i])
                hit = j;
            if (c[j].cnt == 0 && free_slot < 0)
                free_slot = j;
        }
        if (hit >= 0)
            c[hit].cnt++;
        else if (free_slot >= 0) {
            c[free_slot].val = a[i];
            c[free_slot].cnt = 1;
        } else
            for (int j = 0; j < k; j++)
                c[j].cnt--;
    }
    int m = 0;
    for (int j = 0; j < k; j++) {
        if (c[j].cnt <= 0)
            continue;
        int real = 0;
        for (int i = 0; i < n; i++)
            real += a[i] == c[j].val;
        if ((long)real * (k + 1) > n) {
            int dup = 0;
            for (int q = 0; q < m; q++)
                dup |= out[q] == c[j].val;
            if (!dup)
                out[m++] = c[j].val;
        }
    }
    /* sort output for determinism */
    for (int i = 1; i < m; i++) {
        int x = out[i], j = i - 1;
        while (j >= 0 && out[j] > x) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = x;
    }
    return m;
}

static int brute_frequent(const int *a, int n, int k, int *out) {
    int m = 0;
    for (int v = 0; v < 20; v++) {
        long c = 0;
        for (int i = 0; i < n; i++)
            c += a[i] == v;
        if (c * (k + 1) > n)
            out[m++] = v;
    }
    return m;
}

int main(void) {
    int a[400];
    int got_maj = 0;
    for (int trial = 0; trial < 500; trial++) {
        int n = 1 + (int)(rnd() % 300);
        int bias = (int)(rnd() % 100);
        int heavy = (int)(rnd() % 20);
        for (int i = 0; i < n; i++)
            a[i] = ((int)(rnd() % 100) < bias) ? heavy : (int)(rnd() % 20);
        int want = -1, res;
        for (int v = 0; v < 20; v++) {
            int c = 0;
            for (int i = 0; i < n; i++)
                c += a[i] == v;
            if (c * 2 > n)
                want = v;
        }
        int has = majority(a, n, &res);
        if (has != (want >= 0) || (has && res != want))
            fail("majority");
        got_maj += has;
        for (int k = 1; k <= 4; k++) {
            int o1[8], o2[8];
            int m1 = frequent(a, n, k, o1), m2 = brute_frequent(a, n, k, o2);
            if (m1 != m2)
                fail("frequent count");
            for (int i = 0; i < m1; i++)
                if (o1[i] != o2[i])
                    fail("frequent value");
        }
    }
    printf("500 trials verified, %d had a strict majority\n", got_maj);
    int d[] = {2, 2, 1, 1, 1, 2, 2}, r;
    printf("[2,2,1,1,1,2,2] majority: %d\n", majority(d, 7, &r) ? r : -1);
    int e[] = {3, 2, 3};
    printf("[3,2,3] majority: %d\n", majority(e, 3, &r) ? r : -1);
    int f[] = {1, 2, 3, 4};
    printf("[1,2,3,4] majority: %d\n", majority(f, 4, &r) ? r : -1);
    int g[] = {1, 1, 1, 3, 3, 2, 2, 2};
    int o[8];
    int m = frequent(g, 8, 2, o);
    printf("> n/3 in [1,1,1,3,3,2,2,2]:");
    for (int i = 0; i < m; i++)
        printf(" %d", o[i]);
    printf("\n");
    return 0;
}
