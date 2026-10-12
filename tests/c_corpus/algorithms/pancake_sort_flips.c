/*
 * title: Pancake sorting with prefix reversals
 * topic: algorithms
 * covers: pancake sort, prefix reversal, flip counting, burnt pancakes, bound of 2n-3 flips, permutation enumeration
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void flip(int *a, int k) { /* reverse a[0..k] */
    for (int i = 0, j = k; i < j; i++, j--) {
        int t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static int pancake(int *a, int n, int *log, int logmax) {
    int flips = 0;
    for (int size = n; size > 1; size--) {
        int m = 0;
        for (int i = 1; i < size; i++)
            if (a[i] > a[m])
                m = i;
        if (m == size - 1)
            continue;
        if (m > 0) {
            flip(a, m);
            if (flips < logmax)
                log[flips] = m + 1;
            flips++;
        }
        flip(a, size - 1);
        if (flips < logmax)
            log[flips] = size;
        flips++;
    }
    return flips;
}

/* Burnt pancakes: signed values; every flip also negates the prefix. Sort to +1..+n. */
static void sflip(int *a, int k) {
    for (int i = 0, j = k; i <= j; i++, j--) {
        int t = a[i];
        a[i] = -a[j];
        a[j] = -t;
    }
}

static int burnt(int *a, int n) {
    int flips = 0;
    for (int target = n; target >= 1; target--) {
        int idx = target - 1, pos = 0;
        for (int i = 0; i <= idx; i++)
            if (a[i] == target || a[i] == -target)
                pos = i;
        if (pos == idx && a[idx] > 0)
            continue;
        if (pos != 0) {
            sflip(a, pos);
            flips++;
        }
        if (a[0] > 0) {
            sflip(a, 0);
            flips++;
        }
        sflip(a, idx); /* lands positive at idx */
        flips++;
    }
    return flips;
}

static int next_perm(int *a, int n) {
    int i = n - 2;
    while (i >= 0 && a[i] >= a[i + 1])
        i--;
    if (i < 0)
        return 0;
    int j = n - 1;
    while (a[j] <= a[i])
        j--;
    int t = a[i];
    a[i] = a[j];
    a[j] = t;
    for (int l = i + 1, r = n - 1; l < r; l++, r--) {
        t = a[l];
        a[l] = a[r];
        a[r] = t;
    }
    return 1;
}

int main(void) {
    /* exhaustive over all permutations of 1..n */
    for (int n = 2; n <= 7; n++) {
        int p[8], total = 0, worst = 0, count = 0;
        for (int i = 0; i < n; i++)
            p[i] = i + 1;
        do {
            int q[8];
            memcpy(q, p, sizeof(int) * (size_t)n);
            int f = pancake(q, n, NULL, 0);
            for (int i = 0; i < n; i++)
                check(q[i] == i + 1, "sorted");
            check(f <= 2 * n - 3, "flip bound 2n-3");
            total += f;
            if (f > worst)
                worst = f;
            count++;
        } while (next_perm(p, n));
        printf("n=%d permutations=%-5d worst_flips=%-2d total_flips=%d\n", n, count, worst, total);
    }
    int a[8] = {3, 6, 1, 8, 2, 7, 4, 5}, log[32];
    int f = pancake(a, 8, log, 32);
    printf("example flips=%d sequence:", f);
    for (int i = 0; i < f && i < 32; i++)
        printf(" %d", log[i]);
    printf("\n");
    for (int i = 0; i < 8; i++)
        check(a[i] == i + 1, "example sorted");
    /* burnt pancakes must end fully sorted and positive */
    int b[6] = {-3, 1, -5, 2, 6, -4};
    int bf = burnt(b, 6);
    printf("burnt flips=%d result:", bf);
    for (int i = 0; i < 6; i++) {
        check(b[i] == i + 1, "burnt sorted");
        printf(" %+d", b[i]);
    }
    printf("\n");
    return 0;
}
