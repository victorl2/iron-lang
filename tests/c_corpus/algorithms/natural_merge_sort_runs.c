/*
 * title: Natural merge sort and adaptivity to presortedness
 * topic: algorithms
 * covers: natural merge sort, run counting, adaptive sorting, presortedness measures, array ping-pong merging
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 97531u;
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

static long cmps;

static int count_runs(const int *a, int n) {
    int r = n ? 1 : 0;
    for (int i = 1; i < n; i++)
        r += a[i] < a[i - 1];
    return r;
}

/* Merge adjacent ascending runs pairwise until a single run remains. */
static int natural_sort(int *a, int *tmp, int n, int *rounds) {
    *rounds = 0;
    if (n < 2)
        return 0;
    int *src = a, *dst = tmp;
    for (;;) {
        int i = 0, k = 0, merged_pairs = 0;
        while (i < n) {
            int j = i + 1;
            while (j < n && (cmps++, src[j] >= src[j - 1]))
                j++;
            int e = j;
            if (j < n) {
                e = j + 1;
                while (e < n && (cmps++, src[e] >= src[e - 1]))
                    e++;
            } else if (i == 0) {
                /* whole array is one run */
                if (src != a)
                    memcpy(a, src, sizeof(int) * (size_t)n);
                return 1;
            }
            int p = i, q = j;
            while (p < j && q < e) {
                cmps++;
                dst[k++] = src[q] < src[p] ? src[q++] : src[p++];
            }
            while (p < j)
                dst[k++] = src[p++];
            while (q < e)
                dst[k++] = src[q++];
            merged_pairs++;
            i = e;
        }
        int *t = src;
        src = dst;
        dst = t;
        (*rounds)++;
        (void)merged_pairs;
    }
}

static long inversions_bruteforce(const int *a, int n) {
    long c = 0;
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            c += a[i] > a[j];
    return c;
}

int main(void) {
    enum { N = 2000 };
    static int base[N], a[N], tmp[N];
    static const char *names[] = {"sorted",     "one swap",  "10 random swaps", "few sorted blocks",
                                  "sorted+tail", "reversed", "random"};
    for (int s = 0; s < 7; s++) {
        for (int i = 0; i < N; i++)
            base[i] = i * 3;
        switch (s) {
        case 1: {
            int t = base[100];
            base[100] = base[1500];
            base[1500] = t;
            break;
        }
        case 2:
            for (int k = 0; k < 10; k++) {
                int x = (int)(rng() % N), y = (int)(rng() % N), t = base[x];
                base[x] = base[y];
                base[y] = t;
            }
            break;
        case 3:
            for (int b = 0; b < 8; b++)
                for (int i = 0; i < N / 8; i++)
                    base[b * (N / 8) + i] = (int)(rng() % 10) * 100000 * 0 + ((b * 5) % 8) * 10000 + i;
            break;
        case 4:
            for (int i = N - 50; i < N; i++)
                base[i] = (int)(rng() % 6000);
            break;
        case 5:
            for (int i = 0; i < N; i++)
                base[i] = (N - i) * 3;
            break;
        case 6:
            for (int i = 0; i < N; i++)
                base[i] = (int)(rng() % 100000);
            break;
        default:
            break;
        }
        memcpy(a, base, sizeof a);
        int runs = count_runs(a, N);
        long inv = N <= 2000 ? inversions_bruteforce(a, N) : 0;
        cmps = 0;
        int rounds;
        natural_sort(a, tmp, N, &rounds);
        for (int i = 1; i < N; i++)
            check(a[i - 1] <= a[i], "sorted");
        printf("%-18s runs=%-5d inversions=%-8ld rounds=%-2d cmps=%ld\n", names[s], runs, inv, rounds, cmps);
    }
    return 0;
}
