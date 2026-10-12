/*
 * title: Odd-even transposition sort with parallel-round simulation
 * topic: algorithms
 * covers: brick sort, round-based parallel model, n rounds guarantee, early termination, snapshot semantics
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 16180339u;
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

/* One synchronous round: every processor pair (i, i+1) with i%2==phase acts on the old snapshot. */
static int round_sync(int *a, int *next, int n, int phase) {
    memcpy(next, a, sizeof(int) * (size_t)n);
    int changed = 0;
    for (int i = phase; i + 1 < n; i += 2) {
        if (a[i] > a[i + 1]) {
            next[i] = a[i + 1];
            next[i + 1] = a[i];
            changed = 1;
        }
    }
    memcpy(a, next, sizeof(int) * (size_t)n);
    return changed;
}

static int is_sorted(const int *a, int n) {
    for (int i = 1; i < n; i++)
        if (a[i - 1] > a[i])
            return 0;
    return 1;
}

int main(void) {
    static const int sizes[] = {1, 2, 5, 10, 33, 64, 200};
    for (int s = 0; s < 7; s++) {
        int n = sizes[s];
        int *a = malloc(sizeof(int) * (size_t)n), *nx = malloc(sizeof(int) * (size_t)n);
        check(a && nx, "alloc");
        for (int i = 0; i < n; i++)
            a[i] = (int)(rng() % 1000);
        int rounds = 0, idle = 0;
        while (idle < 2) {
            int ch = round_sync(a, nx, n, rounds & 1);
            rounds++;
            idle = ch ? 0 : idle + 1;
        }
        int useful = rounds - 2;
        check(is_sorted(a, n), "sorted");
        check(useful <= n, "at most n rounds are useful");
        printf("n=%-4d useful_rounds=%-4d bound=%-4d\n", n, useful, n);
        free(a);
        free(nx);
    }
    /* worst case: reversed input needs exactly n rounds */
    for (int n = 4; n <= 32; n *= 2) {
        int a[32], nx[32];
        for (int i = 0; i < n; i++)
            a[i] = n - i;
        int rounds = 0;
        while (!is_sorted(a, n)) {
            round_sync(a, nx, n, rounds & 1);
            rounds++;
        }
        printf("reversed n=%-2d rounds=%d\n", n, rounds);
        check(rounds <= n, "n rounds");
    }
    return 0;
}
