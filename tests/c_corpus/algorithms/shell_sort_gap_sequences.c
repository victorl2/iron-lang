/*
 * title: Shell sort with different gap sequences
 * topic: algorithms
 * covers: shell sort, gap sequences, function pointers, comparison and move counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long st = 88172645463325252ULL;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 7;
    st ^= st << 17;
    return (unsigned)(st >> 11);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef int (*GapFn)(int n, int *gaps); /* fills gaps in decreasing order, returns count */

static int gaps_shell(int n, int *g) {
    int k = 0;
    for (int h = n / 2; h >= 1; h /= 2)
        g[k++] = h;
    return k;
}

static int gaps_knuth(int n, int *g) {
    int tmp[32], k = 0;
    for (int h = 1; h < n; h = 3 * h + 1)
        tmp[k++] = h;
    for (int i = 0; i < k; i++)
        g[i] = tmp[k - 1 - i];
    return k;
}

static int gaps_ciura(int n, int *g) {
    static const int c[] = {1, 4, 10, 23, 57, 132, 301, 701, 1750};
    int k = 0;
    for (int i = 8; i >= 0; i--)
        if (c[i] < n)
            g[k++] = c[i];
    return k;
}

static int gaps_pratt(int n, int *g) {
    /* 3-smooth numbers, 2^p 3^q < n, descending */
    int all[64], k = 0;
    for (int p = 1; p < n; p *= 2)
        for (int q = p; q < n; q *= 3)
            all[k++] = q;
    for (int i = 0; i < k; i++)
        for (int j = i + 1; j < k; j++)
            if (all[j] > all[i]) {
                int t = all[i];
                all[i] = all[j];
                all[j] = t;
            }
    memcpy(g, all, sizeof(int) * (size_t)k);
    return k;
}

typedef struct {
    long cmps, moves;
} Cost;

static Cost shell_sort(int *a, int n, GapFn fn) {
    int gaps[64];
    int ng = fn(n, gaps);
    Cost c = {0, 0};
    for (int gi = 0; gi < ng; gi++) {
        int h = gaps[gi];
        for (int i = h; i < n; i++) {
            int x = a[i], j = i;
            while (j >= h) {
                c.cmps++;
                if (a[j - h] <= x)
                    break;
                a[j] = a[j - h];
                c.moves++;
                j -= h;
            }
            a[j] = x;
        }
    }
    return c;
}

int main(void) {
    static const struct {
        const char *name;
        GapFn fn;
    } seqs[] = {{"shell", gaps_shell}, {"knuth", gaps_knuth}, {"ciura", gaps_ciura}, {"pratt", gaps_pratt}};
    static const int sizes[] = {10, 100, 1000, 5000};
    for (int si = 0; si < 4; si++) {
        int n = sizes[si];
        int *base = malloc(sizeof(int) * (size_t)n);
        int *a = malloc(sizeof(int) * (size_t)n);
        check(base && a, "alloc");
        for (int i = 0; i < n; i++)
            base[i] = (int)(rng() % 100000);
        printf("n=%d\n", n);
        for (int q = 0; q < 4; q++) {
            memcpy(a, base, sizeof(int) * (size_t)n);
            Cost c = shell_sort(a, n, seqs[q].fn);
            for (int i = 1; i < n; i++)
                check(a[i - 1] <= a[i], "sorted");
            printf("  %-6s cmps=%-7ld moves=%ld\n", seqs[q].name, c.cmps, c.moves);
        }
        free(base);
        free(a);
    }
    int g[64];
    int k = gaps_knuth(100, g);
    printf("knuth(100):");
    for (int i = 0; i < k; i++)
        printf(" %d", g[i]);
    k = gaps_pratt(30, g);
    printf("\npratt(30):");
    for (int i = 0; i < k; i++)
        printf(" %d", g[i]);
    printf("\n");
    return 0;
}
