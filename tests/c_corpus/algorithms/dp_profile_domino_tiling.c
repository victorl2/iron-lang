/*
 * title: Broken-profile DP: domino tilings of a grid
 * topic: algorithms
 * covers: dynamic programming, broken profile, bitmask transitions, cell-by-cell DP, transfer over rows, Fibonacci and known counts, obstacle cells
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

typedef unsigned long long u64;

/* count tilings of an h x w board with dominoes, cells with blocked[r][c] excluded, cell-by-cell profile */
static u64 tilings(int h, int w, unsigned char blocked[][10]) {
    size_t sz = (size_t)1 << w;
    u64 *cur = calloc(sz, sizeof *cur), *nxt = calloc(sz, sizeof *nxt);
    CHECK(cur && nxt);
    cur[0] = 1;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            memset(nxt, 0, sz * sizeof *nxt);
            for (size_t m = 0; m < sz; m++) {
                u64 v = cur[m];
                if (!v) continue;
                /* bit c: cell (r,c) already covered by a vertical domino from the row above */
                int filled = (int)(m >> c & 1);
                size_t clr = m & ~((size_t)1 << c);
                if (blocked[r][c]) {
                    if (!filled) nxt[clr] += v;
                    continue;
                }
                if (filled) { nxt[clr] += v; continue; }
                /* vertical: covers (r+1,c) */
                if (r + 1 < h && !blocked[r + 1][c]) nxt[clr | ((size_t)1 << c)] += v;
                /* horizontal: covers (r,c+1); bit c+1 must be free and cell open */
                if (c + 1 < w && !blocked[r][c + 1] && !(m >> (c + 1) & 1)) nxt[clr | ((size_t)1 << (c + 1))] += v;
            }
            u64 *t = cur; cur = nxt; nxt = t;
        }
    u64 r = cur[0];
    free(cur); free(nxt);
    return r;
}

/* brute force by placing at first empty cell */
static u64 brute(int h, int w, unsigned char g[][10], int pos) {
    while (pos < h * w && g[pos / w][pos % w]) pos++;
    if (pos == h * w) return 1;
    int r = pos / w, c = pos % w;
    u64 n = 0;
    g[r][c] = 1;
    if (c + 1 < w && !g[r][c + 1]) { g[r][c + 1] = 1; n += brute(h, w, g, pos + 1); g[r][c + 1] = 0; }
    if (r + 1 < h && !g[r + 1][c]) { g[r + 1][c] = 1; n += brute(h, w, g, pos + 1); g[r + 1][c] = 0; }
    g[r][c] = 0;
    return n;
}

int main(void) {
    (void)rr;
    unsigned char none[10][10];
    memset(none, 0, sizeof none);
    printf("open boards:\n");
    for (int h = 2; h <= 8; h += 2) {
        printf(" %d rows:", h);
        for (int w = 1; w <= 8; w++) printf(" %llu", tilings(h, w, none));
        printf("\n");
    }
    CHECK(tilings(8, 8, none) == 12988816ULL);
    CHECK(tilings(2, 10 > 9 ? 9 : 10, none) == 55); /* Fibonacci */
    for (int t = 0; t < 6; t++) {
        int h = 3 + t % 3, w = 4 + t % 4;
        unsigned char g[10][10];
        memset(g, 0, sizeof g);
        int holes = 0;
        for (int i = 0; i < h; i++) for (int j = 0; j < w; j++) if (rr(100) < 15) { g[i][j] = 1; holes++; }
        u64 f = tilings(h, w, g);
        u64 b = brute(h, w, g, 0);
        CHECK(f == b);
        printf("board %dx%d holes=%d tilings=%llu\n", h, w, holes, f);
    }
    return 0;
}
