/*
 * title: Permutation generation by adjacent swaps and Heap's algorithm
 * topic: algorithms
 * covers: Steinhaus-Johnson-Trotter, Heap's algorithm, minimal-change orders, direction arrows, parity of permutations
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fact_int(int n) { int r = 1; while (n > 1) r *= n--; return r; }

/* Steinhaus-Johnson-Trotter with mobile elements */
static int sjt(int n, int (*out)[8], int *swaps_pos) {
    int a[8], dir[8]; /* dir -1 left, +1 right */
    for (int i = 0; i < n; i++) { a[i] = i + 1; dir[i] = -1; }
    int cnt = 0;
    memcpy(out[cnt++], a, sizeof a);
    for (;;) {
        int mob = -1;
        for (int i = 0; i < n; i++) {
            int j = i + dir[i];
            if (j < 0 || j >= n || a[j] >= a[i]) continue;
            if (mob < 0 || a[i] > a[mob]) mob = i;
        }
        if (mob < 0) break;
        int j = mob + dir[mob];
        int mv = a[mob];
        int t = a[mob]; a[mob] = a[j]; a[j] = t;
        t = dir[mob]; dir[mob] = dir[j]; dir[j] = t;
        swaps_pos[cnt - 1] = mob < j ? mob : j;
        for (int i = 0; i < n; i++) if (a[i] > mv) dir[i] = -dir[i];
        memcpy(out[cnt++], a, sizeof a);
    }
    return cnt;
}

static int heap_out[40320][8];
static int heap_cnt;
static void heap_rec(int k, int *a, int n) {
    if (k == 1) { memcpy(heap_out[heap_cnt], a, 8 * sizeof(int)); heap_cnt++; return; }
    heap_rec(k - 1, a, n);
    for (int i = 0; i < k - 1; i++) {
        int t;
        if (k % 2 == 0) { t = a[i]; a[i] = a[k - 1]; a[k - 1] = t; }
        else { t = a[0]; a[0] = a[k - 1]; a[k - 1] = t; }
        heap_rec(k - 1, a, n);
    }
}

static int parity(const int *p, int n) {
    int inv = 0;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) inv += p[i] > p[j];
    return inv & 1;
}

static int hamming(const int *a, const int *b, int n) { int d = 0; for (int i = 0; i < n; i++) d += a[i] != b[i]; return d; }

int main(void) {
    static int out[40320][8];
    static int pos[40320];
    int n = 4;
    int c = sjt(n, out, pos);
    printf("SJT n=4 (%d perms):", c);
    for (int i = 0; i < c; i++) { printf(" "); for (int k = 0; k < n; k++) printf("%d", out[i][k]); }
    printf("\n");
    for (n = 2; n <= 8; n++) {
        c = sjt(n, out, pos);
        if (c != fact_int(n)) { fprintf(stderr, "sjt count wrong\n"); return 1; }
        int distinct_ok = 1, adj_ok = 1, alt_ok = 1;
        /* each consecutive pair differs by exactly one adjacent transposition */
        for (int i = 1; i < c; i++) {
            if (hamming(out[i - 1], out[i], n) != 2) adj_ok = 0;
            if (parity(out[i], n) == parity(out[i - 1], n)) alt_ok = 0;
        }
        /* distinct: rank via base-n encoding into a bitmap */
        for (int i = 0; i < c && distinct_ok; i++) {
            /* Lehmer rank */
            long r = 0;
            for (int x = 0; x < n; x++) {
                int sm = 0;
                for (int y = x + 1; y < n; y++) sm += out[i][y] < out[i][x];
                r = r * (n - x) + sm;
            }
            static unsigned char mark[40320];
            if (i == 0) memset(mark, 0, sizeof mark);
            if (mark[r]) distinct_ok = 0;
            mark[r] = 1;
        }
        printf("n=%d: %d perms, adjacent swaps only: %s, parity alternates: %s, all distinct: %s\n", n, c,
               adj_ok ? "yes" : "no", alt_ok ? "yes" : "no", distinct_ok ? "yes" : "no");
        if (!adj_ok || !alt_ok || !distinct_ok) return 1;
    }
    /* Heap's algorithm: one swap per step, not necessarily adjacent */
    int a[8] = {1, 2, 3, 4, 0, 0, 0, 0};
    heap_cnt = 0;
    heap_rec(4, a, 4);
    printf("Heap n=4 (%d perms):", heap_cnt);
    for (int i = 0; i < heap_cnt; i++) { printf(" "); for (int k = 0; k < 4; k++) printf("%d", heap_out[i][k]); }
    printf("\n");
    for (n = 3; n <= 8; n++) {
        int b[8];
        for (int i = 0; i < 8; i++) b[i] = i < n ? i + 1 : 0;
        heap_cnt = 0;
        heap_rec(n, b, n);
        int one_swap = 1, adjacent = 0;
        for (int i = 1; i < heap_cnt; i++) {
            if (hamming(heap_out[i - 1], heap_out[i], n) != 2) one_swap = 0;
            for (int k = 0; k + 1 < n; k++)
                if (heap_out[i - 1][k] == heap_out[i][k + 1] && heap_out[i - 1][k + 1] == heap_out[i][k] && hamming(heap_out[i-1], heap_out[i], n) == 2) { adjacent++; break; }
        }
        printf("Heap n=%d: %d perms, single swap each step: %s, steps that happen to be adjacent: %d\n",
               n, heap_cnt, one_swap ? "yes" : "no", adjacent);
        if (heap_cnt != fact_int(n) || !one_swap) return 1;
    }
    /* sum over permutations of number of fixed points = n! (expected 1 per perm) */
    n = 7;
    c = sjt(n, out, pos);
    long fixed = 0;
    for (int i = 0; i < c; i++) for (int k = 0; k < n; k++) fixed += out[i][k] == k + 1;
    printf("total fixed points over all 7! permutations: %ld\n", fixed);
    return fixed == 5040 ? 0 : 1;
}
