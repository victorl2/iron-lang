/*
 * title: Smallest range covering k sorted lists
 * topic: data_structures
 * covers: min-heap of list cursors, running maximum, k sorted lists, brute-force cross-check, ragged lists
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs = 0x2A2Aull;
static unsigned rng(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

enum { KMAX = 12, LMAX = 60 };

typedef struct {
    int val, list, idx;
} Cur;

static int cmp_cur(Cur a, Cur b) { return a.val != b.val ? a.val < b.val : a.list < b.list; }

typedef struct {
    Cur a[KMAX];
    int n;
} H;

static void down(H *h, int i) {
    Cur x = h->a[i];
    for (;;) {
        int c = 2 * i + 1;
        if (c >= h->n)
            break;
        if (c + 1 < h->n && cmp_cur(h->a[c + 1], h->a[c]))
            c++;
        if (!cmp_cur(h->a[c], x))
            break;
        h->a[i] = h->a[c];
        i = c;
    }
    h->a[i] = x;
}

typedef struct {
    int lo, hi;
    long pops;
} Range;

static Range heap_solution(int k, int *len, int lists[][LMAX]) {
    H h;
    h.n = 0;
    int cur_max = -(1 << 30);
    for (int i = 0; i < k; i++) {
        h.a[h.n++] = (Cur){lists[i][0], i, 0};
        if (lists[i][0] > cur_max)
            cur_max = lists[i][0];
    }
    for (int i = h.n / 2 - 1; i >= 0; i--)
        down(&h, i);
    Range best = {h.a[0].val, cur_max, 0};
    for (;;) {
        Cur top = h.a[0];
        best.pops++;
        if (cur_max - top.val < best.hi - best.lo)
            best = (Range){top.val, cur_max, best.pops};
        if (top.idx + 1 >= len[top.list])
            break; /* one list exhausted: no larger window can cover everyone */
        top.idx++;
        top.val = lists[top.list][top.idx];
        if (top.val > cur_max)
            cur_max = top.val;
        h.a[0] = top;
        down(&h, 0);
    }
    return best;
}

static Range brute(int k, int *len, int lists[][LMAX]) {
    Range best = {0, 1 << 29, 0};
    for (int i = 0; i < k; i++)
        for (int j = 0; j < len[i]; j++) {
            int lo = lists[i][j], hi = lo;
            int ok = 1;
            for (int q = 0; q < k && ok; q++) {
                int found = 0;
                for (int t = 0; t < len[q]; t++)
                    if (lists[q][t] >= lo) {
                        found = 1;
                        if (lists[q][t] > hi)
                            hi = lists[q][t];
                        break;
                    }
                if (!found)
                    ok = 0;
            }
            if (ok && (hi - lo < best.hi - best.lo || (hi - lo == best.hi - best.lo && lo < best.lo)))
                best = (Range){lo, hi, 0};
        }
    return best;
}

static int cmp_int(const void *x, const void *y) { return *(const int *)x - *(const int *)y; }

int main(void) {
    static int lists[KMAX][LMAX];
    int len[KMAX];
    /* fixed textbook case first */
    int t0[3][5] = {{4, 10, 15, 24, 26}, {0, 9, 12, 20, 25}, {5, 18, 22, 30, 31}};
    for (int i = 0; i < 3; i++) {
        len[i] = 5;
        for (int j = 0; j < 5; j++)
            lists[i][j] = t0[i][j];
    }
    Range r = heap_solution(3, len, lists);
    check(r.lo == 22 && r.hi == 25, "hand-checked answer [22,25]");
    printf("fixed case: [%d,%d] after %ld pops\n", r.lo, r.hi, r.pops);

    long sumw = 0;
    int trials = 400, widest = 0, degenerate = 0;
    for (int t = 0; t < trials; t++) {
        int k = 2 + (int)(rng() % (KMAX - 1));
        unsigned span = 20 + rng() % 500;
        for (int i = 0; i < k; i++) {
            len[i] = 1 + (int)(rng() % LMAX);
            for (int j = 0; j < len[i]; j++)
                lists[i][j] = (int)(rng() % span);
            qsort(lists[i], (size_t)len[i], sizeof(int), cmp_int);
        }
        Range a = heap_solution(k, len, lists), b = brute(k, len, lists);
        check(a.hi - a.lo == b.hi - b.lo, "heap width equals brute-force width");
        /* the heap's window must really cover every list */
        for (int i = 0; i < k; i++) {
            int cov = 0;
            for (int j = 0; j < len[i]; j++)
                if (lists[i][j] >= a.lo && lists[i][j] <= a.hi)
                    cov = 1;
            check(cov, "window covers each list");
        }
        check(a.lo == b.lo, "same leftmost smallest window");
        sumw += a.hi - a.lo;
        if (a.hi - a.lo > widest)
            widest = a.hi - a.lo;
        if (a.hi == a.lo)
            degenerate++;
    }
    printf("trials=%d total_width=%ld widest=%d zero_width=%d\n", trials, sumw, widest, degenerate);
    return 0;
}
