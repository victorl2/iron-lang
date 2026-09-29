/*
 * title: Cycle sort and write minimisation
 * topic: algorithms
 * covers: cycle sort, minimal writes, rank by counting smaller elements, duplicates handling, write counting versus other sorts
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 60606u;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long writes;

static void put(int *a, int i, int v) {
    a[i] = v;
    writes++;
}

static void cycle_sort(int *a, int n) {
    for (int start = 0; start + 1 < n; start++) {
        int item = a[start];
        int pos = start;
        for (int i = start + 1; i < n; i++)
            if (a[i] < item)
                pos++;
        if (pos == start)
            continue;
        while (item == a[pos])
            pos++;
        int t = a[pos];
        put(a, pos, item);
        item = t;
        while (pos != start) {
            pos = start;
            for (int i = start + 1; i < n; i++)
                if (a[i] < item)
                    pos++;
            while (item == a[pos])
                pos++;
            if (item != a[pos]) {
                t = a[pos];
                put(a, pos, item);
                item = t;
            }
        }
    }
}

static void selection(int *a, int n) {
    for (int i = 0; i + 1 < n; i++) {
        int m = i;
        for (int j = i + 1; j < n; j++)
            if (a[j] < a[m])
                m = j;
        if (m != i) {
            int t = a[i];
            put(a, i, a[m]);
            put(a, m, t);
        }
    }
}

static void insertion(int *a, int n) {
    for (int i = 1; i < n; i++) {
        int x = a[i], j = i - 1;
        while (j >= 0 && a[j] > x) {
            put(a, j + 1, a[j]);
            j--;
        }
        put(a, j + 1, x);
    }
}

/* Lower bound: number of positions where the sorted array differs from the input. */
static int misplaced(const int *a, const int *sorted, int n) {
    int c = 0;
    for (int i = 0; i < n; i++)
        c += a[i] != sorted[i];
    return c;
}

static int cmp_int(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void) {
    enum { N = 300 };
    static int base[N], a[N], sorted[N];
    static const char *names[] = {"distinct", "few keys", "rotated", "nearly sorted"};
    for (int s = 0; s < 4; s++) {
        for (int i = 0; i < N; i++) {
            switch (s) {
            case 0: base[i] = (i * 7919) % N; break; /* a permutation: 7919 is coprime with 300 */
            case 1: base[i] = (int)(rng() % 5); break;
            case 2: base[i] = (i + 17) % N; break;
            default: base[i] = i + ((i % 25 == 0) ? 3 : 0); break;
            }
        }
        memcpy(sorted, base, sizeof sorted);
        qsort(sorted, N, sizeof(int), cmp_int);
        int lb = misplaced(base, sorted, N);
        long w[3];
        void (*fn[3])(int *, int) = {cycle_sort, selection, insertion};
        for (int k = 0; k < 3; k++) {
            memcpy(a, base, sizeof a);
            writes = 0;
            fn[k](a, N);
            check(memcmp(a, sorted, sizeof a) == 0, "sorted correctly");
            w[k] = writes;
        }
        check(w[0] <= w[1] && w[0] <= w[2], "cycle sort writes least");
        check(w[0] >= lb, "cannot beat the misplaced-count lower bound");
        printf("%-13s misplaced=%-4d writes: cycle=%-4ld selection=%-4ld insertion=%ld\n", names[s], lb, w[0], w[1], w[2]);
    }
    return 0;
}
