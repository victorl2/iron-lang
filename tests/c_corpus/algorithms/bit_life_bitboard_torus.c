/*
 * title: Game of Life on a bitboard with bit-sliced adders
 * topic: algorithms
 * covers: bitboards, toroidal shifts, bit-sliced neighbour counting, half and full adders, glider periodicity, oscillators
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* 8x8 torus packed row-major into one 64-bit word: bit (y*8 + x). */
typedef uint64_t Board;

static uint64_t rs = 0x5DEECE66DULL;

static uint64_t rnd64(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return rs;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Board rot_x(Board b, int dx) { /* shift every row cyclically by dx columns (+1 = east) */
    Board out = 0;
    for (int y = 0; y < 8; y++) {
        unsigned row = (unsigned)((b >> (8 * y)) & 0xFF);
        unsigned r = dx > 0 ? ((row << 1) | (row >> 7)) & 0xFFu : ((row >> 1) | (row << 7)) & 0xFFu;
        out |= (Board)r << (8 * y);
    }
    return out;
}

static Board rot_y(Board b, int dy) { /* +1 = south */
    return dy > 0 ? (b << 8) | (b >> 56) : (b >> 8) | (b << 56);
}

/* Bit-sliced step: add the eight neighbour planes with a tree of adders, then apply the rule. */
static void half_add(uint64_t a, uint64_t b, uint64_t *sum, uint64_t *carry) {
    *sum = a ^ b;
    *carry = a & b;
}

static void full_add(uint64_t a, uint64_t b, uint64_t c, uint64_t *sum, uint64_t *carry) {
    uint64_t t = a ^ b;
    *sum = t ^ c;
    *carry = (a & b) | (t & c);
}

static Board step_bits(Board b) {
    Board n[8];
    int k = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy)
                continue;
            Board t = b;
            if (dx)
                t = rot_x(t, dx);
            if (dy)
                t = rot_y(t, dy);
            n[k++] = t;
        }
    /* three-plane counter: bit0, bit1, bit2 of the neighbour count */
    uint64_t s0, c0, s1, c1, s2, c2, s3, c3;
    full_add(n[0], n[1], n[2], &s0, &c0);
    full_add(n[3], n[4], n[5], &s1, &c1);
    half_add(n[6], n[7], &s2, &c2);
    uint64_t one, twos_a;
    full_add(s0, s1, s2, &one, &twos_a); /* weight-1 plane and a carry of weight 2 */
    /* weight-2 planes: c0, c1, c2, twos_a: add four bits into (two, four) */
    full_add(c0, c1, c2, &s3, &c3);
    uint64_t two, c4;
    half_add(s3, twos_a, &two, &c4);
    uint64_t four_any = c3 | c4;
    /* count is 2 or 3 with the four-plane empty */
    uint64_t eq2 = ~one & two & ~four_any;
    uint64_t eq3 = one & two & ~four_any;
    return eq3 | (b & eq2);
}

static Board step_naive(Board b) {
    Board out = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int cnt = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (!dx && !dy)
                        continue;
                    int yy = (y + dy + 8) % 8, xx = (x + dx + 8) % 8;
                    cnt += (int)((b >> (yy * 8 + xx)) & 1u);
                }
            int alive = (int)((b >> (y * 8 + x)) & 1u);
            if (cnt == 3 || (alive && cnt == 2))
                out |= 1ULL << (y * 8 + x);
        }
    return out;
}

static int popcount(uint64_t x) {
    int c = 0;
    for (; x; x &= x - 1)
        c++;
    return c;
}

static void show(Board b) {
    for (int y = 0; y < 8; y++) {
        printf("  ");
        for (int x = 0; x < 8; x++)
            putchar(((b >> (y * 8 + x)) & 1u) ? '#' : '.');
        putchar('\n');
    }
}

static Board make(const char *rows[], int n) {
    Board b = 0;
    for (int y = 0; y < n; y++)
        for (int x = 0; rows[y][x]; x++)
            if (rows[y][x] == '#')
                b |= 1ULL << (y * 8 + x);
    return b;
}

int main(void) {
    /* cross-check the bit-sliced rule on random boards, including full and empty */
    long steps = 0;
    for (int t = 0; t < 3000; t++) {
        Board b = rnd64();
        if (t % 3 == 1)
            b &= rnd64();
        if (t % 3 == 2)
            b |= rnd64();
        Board x = step_bits(b), y = step_naive(b);
        check(x == y, "bit-sliced step equals naive step");
        steps++;
    }
    check(step_bits(0) == 0 && step_bits(~0ULL) == 0, "empty stays empty, full dies");
    printf("bit-sliced rule verified on %ld random boards\n", steps);
    const char *glider_rows[] = {".#......", "..#.....", "###....."};
    Board g = make(glider_rows, 3);
    printf("glider generation 0 (population %d):\n", popcount(g));
    show(g);
    Board cur = g;
    int period = 0;
    for (int gen = 1; gen <= 64; gen++) {
        cur = step_bits(cur);
        check(popcount(cur) == 5, "glider keeps five cells");
        if (gen == 4) {
            Board moved = rot_y(rot_x(g, 1), 1);
            check(cur == moved, "after 4 generations the glider moved one cell diagonally");
        }
        if (cur == g && period == 0)
            period = gen;
    }
    printf("glider returns to its start after %d generations on the 8x8 torus\n", period);
    check(period == 32, "glider period on 8x8 torus");
    const char *blinker_rows[] = {"........", "...###.."};
    Board bl = make(blinker_rows, 2);
    check(step_bits(step_bits(bl)) == bl && step_bits(bl) != bl, "blinker period 2");
    const char *block_rows[] = {"..##....", "..##...."};
    Board bk = make(block_rows, 2);
    check(step_bits(bk) == bk, "block is stable");
    printf("blinker phase 1:\n");
    show(step_bits(bl));
    /* random soups: track population and detect the eventual cycle with a tortoise/hare style scan */
    for (int soup = 0; soup < 5; soup++) {
        Board b = rnd64();
        Board seen[512];
        int n = 0, start = -1, len = 0, maxpop = 0;
        for (int gen = 0; gen < 512 && start < 0; gen++) {
            for (int i = 0; i < n; i++)
                if (seen[i] == b) {
                    start = i;
                    len = n - i;
                    break;
                }
            if (start >= 0)
                break;
            seen[n++] = b;
            if (popcount(b) > maxpop)
                maxpop = popcount(b);
            b = step_bits(b);
        }
        printf("soup %d: enters a cycle of length %d after %d generations, peak population %d, final population %d\n",
               soup, len, start, maxpop, popcount(seen[start]));
    }
    return 0;
}
