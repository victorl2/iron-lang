/*
 * title: Josephus problem: simulation, recurrence and closed forms
 * topic: algorithms
 * covers: circular elimination, linked list simulation, O(n) recurrence, binary rotation closed form for k=2, order statistic tree by Fenwick, generalized k
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

/* circular linked list simulation; returns survivor (0-based) and fills order */
static int simulate(int n, int k, int *order) {
    int *next = malloc((size_t)n * sizeof *next);
    if (!next) exit(2);
    for (int i = 0; i < n; i++) next[i] = (i + 1) % n;
    int prev = n - 1, cur = 0, cnt = 0;
    while (cnt < n) {
        for (int s = 1; s < k; s++) { prev = cur; cur = next[cur]; }
        order[cnt++] = cur;
        next[prev] = next[cur];
        cur = next[cur];
    }
    free(next);
    return order[n - 1];
}

static int recurrence(int n, int k) {
    int r = 0;
    for (int i = 2; i <= n; i++) r = (r + k) % i;
    return r;
}

/* k = 2: rotate the leading bit of n to the end (1-based answer) */
static int closed_k2(int n) {
    int hb = 1;
    while (hb * 2 <= n) hb *= 2;
    return 2 * (n - hb) + 1;
}

/* Fenwick tree with k-th order statistic, O(n log n) for any k */
static int fen_survivor(int n, int k, int *order) {
    int *bit = calloc((size_t)n + 1, sizeof *bit);
    if (!bit) exit(2);
    for (int i = 1; i <= n; i++) {
        bit[i] += 1;
        int j = i + (i & -i);
        if (j <= n) bit[j] += bit[i];
    }
    int top = 1;
    while (top * 2 <= n) top *= 2;
    int alive = n, pos = 0, cnt = 0; /* pos: 0-based index among alive */
    while (alive > 0) {
        pos = (pos + k - 1) % alive;
        int want = pos + 1, idx = 0;
        for (int step = top; step; step >>= 1) {
            if (idx + step <= n && bit[idx + step] < want) { idx += step; want -= bit[idx]; }
        }
        idx++; /* 1-based person */
        order[cnt++] = idx - 1;
        for (int i = idx; i <= n; i += i & -i) bit[i]--;
        alive--;
        if (alive) pos %= alive;
    }
    free(bit);
    return order[n - 1];
}

int main(void) {
    static int ord[5000], ord2[5000];
    int n = 10, k = 3;
    int s = simulate(n, k, ord);
    printf("n=10 k=3 elimination order:");
    for (int i = 0; i < n; i++) printf(" %d", ord[i] + 1);
    printf("\nsurvivor %d\n", s + 1);
    (void)fen_survivor(n, k, ord2);
    for (int i = 0; i < n; i++) if (ord[i] != ord2[i]) { fprintf(stderr, "fenwick order mismatch\n"); return 1; }

    for (n = 1; n <= 300; n++) {
        for (k = 1; k <= 12; k++) {
            int a = simulate(n, k, ord), b = recurrence(n, k), c = fen_survivor(n, k, ord2);
            if (a != b || a != c) { fprintf(stderr, "mismatch n=%d k=%d\n", n, k); return 1; }
            for (int i = 0; i < n; i++) if (ord[i] != ord2[i]) { fprintf(stderr, "order mismatch\n"); return 1; }
        }
        if (recurrence(n, 2) + 1 != closed_k2(n)) { fprintf(stderr, "closed form fails %d\n", n); return 1; }
    }
    printf("simulation, recurrence, Fenwick agree for n<=300, k<=12\n");
    printf("k=2 survivors for n=1..16:");
    for (n = 1; n <= 16; n++) printf(" %d", closed_k2(n));
    printf("\n");
    /* n = 2^m gives survivor 1 */
    for (int m = 0; m < 20; m++) if (closed_k2(1 << m) != 1) return 1;
    printf("n = 2^m always leaves position 1 (m<20)\n");
    int ns[] = {41, 1000, 100000, 1000000};
    for (size_t i = 0; i < 4; i++)
        printf("n=%d: k=2 survivor %d, k=3 survivor %d, k=7 survivor %d\n", ns[i], closed_k2(ns[i]),
               recurrence(ns[i], 3) + 1, recurrence(ns[i], 7) + 1);
    /* positions from which elimination order runs: last 5 eliminated for n=41,k=3 (classic Josephus) */
    simulate(41, 3, ord);
    printf("n=41 k=3 last three standing: %d %d %d\n", ord[38] + 1, ord[39] + 1, ord[40] + 1);
    return 0;
}
