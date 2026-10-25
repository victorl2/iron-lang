/*
 * title: Row-major, tiled, Morton and Hilbert index maps compared
 * topic: memory
 * covers: 2D layouts in linear memory, bijection checks, Z-order interleave, Hilbert curve, neighbour distance and cache-line footprint
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 32u        /* matrix is N x N */
#define TILE 4u
#define ELEM 4u      /* bytes per element */
#define LINE 64u

typedef uint32_t (*IndexFn)(uint32_t x, uint32_t y);

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t row_major(uint32_t x, uint32_t y) {
    return y * N + x;
}

static uint32_t col_major(uint32_t x, uint32_t y) {
    return x * N + y;
}

static uint32_t tiled(uint32_t x, uint32_t y) {
    uint32_t tx = x / TILE, ty = y / TILE, ix = x % TILE, iy = y % TILE;
    uint32_t tiles_per_row = N / TILE;
    return (ty * tiles_per_row + tx) * (TILE * TILE) + iy * TILE + ix;
}

static uint32_t spread(uint32_t v) {
    v &= 0xFFFFu;
    v = (v | (v << 8)) & 0x00FF00FFu;
    v = (v | (v << 4)) & 0x0F0F0F0Fu;
    v = (v | (v << 2)) & 0x33333333u;
    v = (v | (v << 1)) & 0x55555555u;
    return v;
}

static uint32_t morton(uint32_t x, uint32_t y) {
    return spread(x) | (spread(y) << 1);
}

static uint32_t unspread(uint32_t v) {
    v &= 0x55555555u;
    v = (v | (v >> 1)) & 0x33333333u;
    v = (v | (v >> 2)) & 0x0F0F0F0Fu;
    v = (v | (v >> 4)) & 0x00FF00FFu;
    v = (v | (v >> 8)) & 0x0000FFFFu;
    return v;
}

static uint32_t hilbert(uint32_t x, uint32_t y) {
    uint32_t d = 0;
    for (uint32_t s = N / 2; s > 0; s /= 2) {
        uint32_t rx = (x & s) ? 1u : 0u, ry = (y & s) ? 1u : 0u;
        d += s * s * ((3u * rx) ^ ry);
        /* keep the low bits, reflecting and swapping for the two quadrants that need it */
        x &= s - 1;
        y &= s - 1;
        if (ry == 0) {
            if (rx == 1) {
                x = s - 1 - x;
                y = s - 1 - y;
            }
            uint32_t t = x;
            x = y;
            y = t;
        }
    }
    return d;
}

typedef struct {
    const char *name;
    IndexFn fn;
} Layout;

static uint32_t abs_diff(uint32_t a, uint32_t b) {
    return a > b ? a - b : b - a;
}

int main(void) {
    Layout layouts[] = {{"row-major", row_major}, {"col-major", col_major}, {"tiled 4x4", tiled},
                        {"morton", morton}, {"hilbert", hilbert}};
    enum { NL = sizeof layouts / sizeof layouts[0] };

    /* every layout is a bijection onto 0..N*N-1 */
    for (int l = 0; l < NL; l++) {
        static unsigned char seen[N * N];
        memset(seen, 0, sizeof seen);
        for (uint32_t y = 0; y < N; y++)
            for (uint32_t x = 0; x < N; x++) {
                uint32_t i = layouts[l].fn(x, y);
                check(i < N * N && !seen[i], "bijection");
                seen[i] = 1;
            }
    }
    printf("all %d layouts are bijections on %ux%u\n", NL, N, N);

    /* morton is invertible with unspread */
    for (uint32_t y = 0; y < N; y++)
        for (uint32_t x = 0; x < N; x++) {
            uint32_t m = morton(x, y);
            check(unspread(m) == x && unspread(m >> 1) == y, "morton inverse");
        }
    printf("morton decode verified\n");

    /* the 8x8 corner of each layout */
    for (int l = 2; l < NL; l++) {
        printf("%s, top-left 8x8 (index):\n", layouts[l].name);
        for (uint32_t y = 0; y < 8; y++) {
            for (uint32_t x = 0; x < 8; x++)
                printf("%4u", (unsigned)layouts[l].fn(x, y));
            printf("\n");
        }
    }

    /* hilbert adjacency: consecutive indices are always grid neighbours */
    uint32_t px[N * N], py[N * N];
    for (uint32_t y = 0; y < N; y++)
        for (uint32_t x = 0; x < N; x++) {
            uint32_t d = hilbert(x, y);
            px[d] = x;
            py[d] = y;
        }
    unsigned jumps = 0;
    for (uint32_t d = 1; d < N * N; d++)
        if (abs_diff(px[d], px[d - 1]) + abs_diff(py[d], py[d - 1]) != 1)
            jumps++;
    printf("hilbert curve jumps between consecutive cells: %u\n", jumps);
    check(jumps == 0, "hilbert continuity");

    /* locality: mean and max index distance to the right and down neighbours */
    printf("%-10s %10s %10s %10s\n", "layout", "mean-dx", "mean-dy", "max-any");
    for (int l = 0; l < NL; l++) {
        unsigned long sx = 0, sy = 0, cx = 0, cy = 0;
        uint32_t mx = 0;
        for (uint32_t y = 0; y < N; y++)
            for (uint32_t x = 0; x < N; x++) {
                uint32_t i = layouts[l].fn(x, y);
                if (x + 1 < N) {
                    uint32_t d = abs_diff(i, layouts[l].fn(x + 1, y));
                    sx += d;
                    cx++;
                    if (d > mx)
                        mx = d;
                }
                if (y + 1 < N) {
                    uint32_t d = abs_diff(i, layouts[l].fn(x, y + 1));
                    sy += d;
                    cy++;
                    if (d > mx)
                        mx = d;
                }
            }
        printf("%-10s %10.2f %10.2f %10u\n", layouts[l].name, (double)sx / (double)cx, (double)sy / (double)cy,
               (unsigned)mx);
    }

    /* cache lines touched by every 4x4 window (the working set of a small stencil) */
    printf("%-10s %8s %8s\n", "layout", "avg-lines", "max");
    for (int l = 0; l < NL; l++) {
        unsigned long total = 0, windows = 0;
        unsigned maxl = 0;
        for (uint32_t wy = 0; wy + 4 <= N; wy++)
            for (uint32_t wx = 0; wx + 4 <= N; wx++) {
                uint32_t lines[16];
                unsigned nl = 0;
                for (uint32_t dy = 0; dy < 4; dy++)
                    for (uint32_t dx = 0; dx < 4; dx++) {
                        uint32_t line = layouts[l].fn(wx + dx, wy + dy) * ELEM / LINE;
                        unsigned dup = 0;
                        for (unsigned q = 0; q < nl; q++)
                            dup |= lines[q] == line;
                        if (!dup)
                            lines[nl++] = line;
                    }
                total += nl;
                windows++;
                if (nl > maxl)
                    maxl = nl;
            }
        printf("%-10s %8.3f %8u\n", layouts[l].name, (double)total / (double)windows, maxl);
    }
    return 0;
}
