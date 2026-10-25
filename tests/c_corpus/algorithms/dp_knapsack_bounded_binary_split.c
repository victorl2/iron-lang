/*
 * title: Bounded knapsack by binary splitting
 * topic: algorithms
 * covers: dynamic programming, bounded knapsack, binary splitting of multiplicities, 0/1 reduction, naive expansion cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 1234567891234567ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

typedef struct { int w, v, k; } Item;

static int zero_one(const int *w, const int *v, int n, int cap) {
    int *d = calloc((size_t)cap + 1, sizeof *d);
    CHECK(d);
    for (int i = 0; i < n; i++)
        for (int c = cap; c >= w[i]; c--)
            if (d[c - w[i]] + v[i] > d[c]) d[c] = d[c - w[i]] + v[i];
    int r = d[cap];
    free(d);
    return r;
}

int main(void) {
    for (int t = 0; t < 6; t++) {
        int n = 4 + rr(5), cap = 50 + rr(400);
        Item it[10];
        for (int i = 0; i < n; i++) { it[i].w = 3 + rr(30); it[i].v = 1 + rr(60); it[i].k = 1 + rr(20); }
        int nw = 0, nv = 0, bw[400], bv[400], ew[400], ev[400];
        int split_items = 0, naive_items = 0;
        for (int i = 0; i < n; i++) {
            int left = it[i].k;
            for (int p = 1; left > 0; p <<= 1) {
                int take = p < left ? p : left;
                bw[split_items] = it[i].w * take;
                bv[split_items] = it[i].v * take;
                split_items++;
                left -= take;
            }
            for (int j = 0; j < it[i].k; j++) { ew[naive_items] = it[i].w; ev[naive_items] = it[i].v; naive_items++; }
        }
        (void)nw; (void)nv;
        CHECK(split_items < 400 && naive_items < 400);
        int a = zero_one(bw, bv, split_items, cap);
        int b = zero_one(ew, ev, naive_items, cap);
        CHECK(a == b);
        printf("case %d: n=%d cap=%d split=%d naive=%d best=%d\n", t, n, cap, split_items, naive_items, a);
    }
    /* multiplicity 13 splits as 1,2,4,6 and can represent every count 0..13 */
    int reach[14] = {0};
    int parts[4] = {1, 2, 4, 6};
    for (int m = 0; m < 16; m++) {
        int s = 0;
        for (int i = 0; i < 4; i++) if (m >> i & 1) s += parts[i];
        CHECK(s <= 13);
        reach[s] = 1;
    }
    int all = 1;
    for (int c = 0; c <= 13; c++) all &= reach[c];
    CHECK(all);
    printf("multiplicity 13 splits into 1,2,4,6: all counts reachable\n");
    return 0;
}
