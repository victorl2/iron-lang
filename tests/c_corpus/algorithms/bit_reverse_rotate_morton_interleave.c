/*
 * title: Bit reversal, rotation and Morton interleaving
 * topic: algorithms
 * covers: bit reversal by swaps, rotation, byte swap, Morton codes with magic masks, de-interleave, Z-order locality, masks
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t rs = 0x0123456789ABCDEFULL;

static uint64_t rnd64(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return rs * 0x2545F4914F6CDD1DULL;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t rev32_swap(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}

static uint32_t rev32_loop(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

static unsigned char rev8_tab[256];

static uint32_t rev32_table(uint32_t v) {
    return ((uint32_t)rev8_tab[v & 0xFF] << 24) | ((uint32_t)rev8_tab[(v >> 8) & 0xFF] << 16) |
           ((uint32_t)rev8_tab[(v >> 16) & 0xFF] << 8) | rev8_tab[v >> 24];
}

static uint32_t rotl32(uint32_t v, unsigned k) {
    k &= 31u;
    return k ? (v << k) | (v >> (32 - k)) : v;
}

static uint32_t rotr32(uint32_t v, unsigned k) {
    k &= 31u;
    return k ? (v >> k) | (v << (32 - k)) : v;
}

static uint32_t bswap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

/* spread the low 16 bits of x so bit i moves to bit 2i */
static uint32_t spread16(uint32_t x) {
    x &= 0xFFFFu;
    x = (x | (x << 8)) & 0x00FF00FFu;
    x = (x | (x << 4)) & 0x0F0F0F0Fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    return x;
}

static uint32_t compact16(uint32_t x) {
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0F0F0F0Fu;
    x = (x | (x >> 4)) & 0x00FF00FFu;
    x = (x | (x >> 8)) & 0x0000FFFFu;
    return x;
}

static uint32_t morton(uint32_t x, uint32_t y) {
    return spread16(x) | (spread16(y) << 1);
}

static uint32_t morton_loop(uint32_t x, uint32_t y) {
    uint32_t m = 0;
    for (int i = 0; i < 16; i++)
        m |= (((x >> i) & 1u) << (2 * i)) | (((y >> i) & 1u) << (2 * i + 1));
    return m;
}

/* Z-order range decomposition helper: BIGMIN-free check that a Morton box query visits
 * only cells whose codes lie between the codes of the box corners. */
static int in_box(uint32_t x, uint32_t y, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

int main(void) {
    for (int i = 0; i < 256; i++) {
        unsigned r = 0, v = (unsigned)i;
        for (int b = 0; b < 8; b++) {
            r = (r << 1) | (v & 1u);
            v >>= 1;
        }
        rev8_tab[i] = (unsigned char)r;
    }
    printf("reverse: 00000001 -> %08x, 12345678 -> %08x, f0f0f0f0 -> %08x\n", rev32_swap(1), rev32_swap(0x12345678u),
           rev32_swap(0xF0F0F0F0u));
    long checked = 0;
    for (int t = 0; t < 20000; t++) {
        uint32_t v = (uint32_t)rnd64();
        uint32_t r = rev32_swap(v);
        check(r == rev32_loop(v) && r == rev32_table(v), "three reversal methods agree");
        check(rev32_swap(r) == v, "reversal is an involution");
        unsigned k = (unsigned)(rnd64() % 40);
        check(rotr32(rotl32(v, k), k) == v, "rotate inverse");
        check(rotl32(v, 32 - (k & 31u)) == rotr32(v, k & 31u) || (k & 31u) == 0, "rotl/rotr duality");
        check(bswap32(bswap32(v)) == v, "byte swap involution");
        uint32_t inbyte = ((uint32_t)rev8_tab[v >> 24] << 24) | ((uint32_t)rev8_tab[(v >> 16) & 0xFF] << 16) |
                          ((uint32_t)rev8_tab[(v >> 8) & 0xFF] << 8) | rev8_tab[v & 0xFF];
        check(bswap32(inbyte) == r, "full reversal = per-byte reversal + byte swap");
        uint32_t x = (uint32_t)(rnd64() & 0xFFFF), y = (uint32_t)(rnd64() & 0xFFFF);
        uint32_t m = morton(x, y);
        check(m == morton_loop(x, y), "morton magic masks vs loop");
        check(compact16(m) == x && compact16(m >> 1) == y, "de-interleave");
        checked++;
    }
    printf("random words checked: %ld\n", checked);
    printf("rotl(80000001,1)=%08x rotr(80000001,1)=%08x bswap(11223344)=%08x\n", rotl32(0x80000001u, 1),
           rotr32(0x80000001u, 1), bswap32(0x11223344u));
    /* Morton order of an 8x8 grid, printed as a table of ranks */
    uint32_t rank[8][8];
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++)
            rank[y][x] = morton(x, y);
    check(rank[7][7] == 63 && rank[0][7] == 21 && rank[7][0] == 42, "corner ranks");
    printf("Morton rank table (8x8):\n");
    for (int y = 0; y < 8; y++) {
        printf(" ");
        for (int x = 0; x < 8; x++)
            printf(" %2u", rank[y][x]);
        printf("\n");
    }
    /* locality: walking the Z curve, count how often consecutive cells are grid neighbours */
    uint32_t px = 0, py = 0;
    int adjacent = 0, jumps = 0, longest = 0;
    for (uint32_t c = 1; c < 1024; c++) {
        uint32_t x = compact16(c), y = compact16(c >> 1);
        int dx = (int)x - (int)px, dy = (int)y - (int)py;
        int dist = abs(dx) + abs(dy);
        if (dist == 1)
            adjacent++;
        else
            jumps++;
        if (dist > longest)
            longest = dist;
        px = x;
        py = y;
    }
    printf("32x32 Z-curve: %d unit steps, %d jumps, longest jump %d\n", adjacent, jumps, longest);
    /* box query: every cell inside the box has code in [morton(lo), morton(hi)] */
    int inside = 0, in_range_outside = 0;
    uint32_t lo = morton(5, 9), hi = morton(20, 25);
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++) {
            uint32_t m = morton(x, y);
            int box = in_box(x, y, 5, 9, 20, 25);
            if (box)
                check(m >= lo && m <= hi, "box cells lie inside the code range");
            inside += box;
            in_range_outside += !box && m >= lo && m <= hi;
        }
    printf("box query: %d cells in box, %d false positives in the code interval [%u,%u]\n", inside,
           in_range_outside, lo, hi);
    return 0;
}
