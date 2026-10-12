/*
 * title: Comb sort and the shrink factor
 * topic: algorithms
 * covers: comb sort, shrink factor tuning with integer arithmetic, rule of eleven, pass counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 7u;
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

typedef struct {
    long cmps;
    int rounds;
} Cost;

/* shrink = num/den; optional rule of eleven (gaps 9 and 10 become 11). */
static Cost comb(int *a, int n, int num, int den, int rule11) {
    Cost c = {0, 0};
    int gap = n, swapped = 1;
    while (gap > 1 || swapped) {
        gap = gap * den / num;
        if (gap < 1)
            gap = 1;
        if (rule11 && (gap == 9 || gap == 10))
            gap = 11;
        swapped = 0;
        c.rounds++;
        for (int i = 0; i + gap < n; i++) {
            c.cmps++;
            if (a[i] > a[i + gap]) {
                int t = a[i];
                a[i] = a[i + gap];
                a[i + gap] = t;
                swapped = 1;
            }
        }
    }
    return c;
}

int main(void) {
    enum { N = 2000 };
    static int base[N], a[N];
    for (int i = 0; i < N; i++)
        base[i] = (int)(rng() % 50000);
    static const struct {
        int num, den, r11;
    } cfg[] = {{13, 10, 0}, {13, 10, 1}, {5, 4, 0}, {3, 2, 0}, {2, 1, 0}, {6, 5, 0}, {4, 3, 1}};
    for (unsigned k = 0; k < sizeof cfg / sizeof cfg[0]; k++) {
        memcpy(a, base, sizeof a);
        Cost c = comb(a, N, cfg[k].num, cfg[k].den, cfg[k].r11);
        for (int i = 1; i < N; i++)
            check(a[i - 1] <= a[i], "sorted");
        printf("shrink %d/%d rule11=%d: rounds=%d cmps=%ld\n", cfg[k].num, cfg[k].den, cfg[k].r11,
               c.rounds, c.cmps);
    }
    /* turtles: small values at the far end */
    for (int i = 0; i < N; i++)
        a[i] = (i < N - 10) ? 1000 + i : N - i;
    Cost c = comb(a, N, 13, 10, 1);
    for (int i = 1; i < N; i++)
        check(a[i - 1] <= a[i], "turtles sorted");
    printf("turtles: rounds=%d cmps=%ld first=%d last=%d\n", c.rounds, c.cmps, a[0], a[N - 1]);
    int two[2] = {2, 1};
    comb(two, 2, 13, 10, 0);
    check(two[0] == 1 && two[1] == 2, "two elements");
    return 0;
}
