/*
 * title: Interval painting and next-free-slot union-find
 * topic: data_structures
 * covers: DSU next pointer, skip filled cells, range assignment amortized, offline paint last writer, first-free allocation, brute-force array
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define L 500

static unsigned long long rs = 0x17E4FA1ULL * 131;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

/* nxt[i] = smallest free position >= i (L is a sentinel) */
static int nxt[L + 2];
static int find(int x) {
    int r = x;
    while (nxt[r] != r) r = nxt[r];
    while (nxt[x] != r) { int t = nxt[x]; nxt[x] = r; x = t; }
    return r;
}
static void occupy(int x) { nxt[x] = x + 1; }
static void reset_free(void) { for (int i = 0; i <= L + 1; i++) nxt[i] = i; }

int main(void) {
    /* Part A: paint intervals in reverse order; each cell gets its LAST writer's color. */
    enum { Q = 120 };
    int lo[Q], hi[Q];
    for (int i = 0; i < Q; i++) {
        int a = (int)(rnd() % L), len = 1 + (int)(rnd() % 40);
        lo[i] = a; hi[i] = a + len > L ? L : a + len;   /* [lo,hi) */
    }
    int color[L], ref[L];
    for (int i = 0; i < L; i++) color[i] = ref[i] = -1;
    reset_free();
    long touched = 0;
    for (int q = Q - 1; q >= 0; q--)
        for (int x = find(lo[q]); x < hi[q]; x = find(x)) { color[x] = q; occupy(x); touched++; }
    for (int q = 0; q < Q; q++) for (int x = lo[q]; x < hi[q]; x++) ref[x] = q;
    check(memcmp(color, ref, sizeof color) == 0, "paint matches naive overwrite");
    int painted = 0, distinct_seen[Q]; memset(distinct_seen, 0, sizeof distinct_seen); int visible = 0;
    for (int i = 0; i < L; i++) if (color[i] >= 0) { painted++; if (!distinct_seen[color[i]]) { distinct_seen[color[i]] = 1; visible++; } }
    printf("paint: queries=%d cells_painted=%d visible_layers=%d cell_visits=%ld (naive would touch about %d)\n",
           Q, painted, visible, touched, Q * 20);
    /* Part B: first-fit allocator over ids with frees, using two-sided structure: alloc = find(lo), free needs re-open, so rebuild lazily */
    reset_free();
    unsigned char used[L]; memset(used, 0, sizeof used);
    int allocs = 0, frees = 0, dirty = 0;
    for (int step = 0; step < 4000; step++) {
        unsigned r = rnd() % 10;
        if (r < 6) {
            int from = (int)(rnd() % (L / 2));
            int got = find(from);
            int want = -1;
            for (int i = from; i < L; i++) if (!used[i]) { want = i; break; }
            if (want < 0) want = L;
            check(got == want, "first free at or after from");
            if (got < L) { used[got] = 1; occupy(got); allocs++; }
        } else {
            int x = (int)(rnd() % L);
            if (used[x]) { used[x] = 0; frees++; dirty = 1; }
            if (dirty) { /* freeing re-opens a cell, which path compression cannot express: rebuild */
                reset_free();
                for (int i = 0; i < L; i++) if (used[i]) nxt[i] = i + 1;
                dirty = 0;
            }
        }
    }
    int u = 0; for (int i = 0; i < L; i++) u += used[i];
    printf("allocator: allocs=%d frees=%d in_use=%d first_free=%d\n", allocs, frees, u, find(0));
    return 0;
}
