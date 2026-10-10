/*
 * title: Reflected Gray code conversion and quadrature decoding
 * topic: algorithms
 * covers: Gray code, prefix xor inverse, single-bit adjacency, incremental subset sums, rotary encoder state machine, Hamming distance
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t rs = 0xDEADBEEFu;

static uint32_t rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t to_gray(uint32_t b) {
    return b ^ (b >> 1);
}

/* Inverse: each output bit is the xor of all higher-or-equal input bits. */
static uint32_t from_gray(uint32_t g) {
    g ^= g >> 16;
    g ^= g >> 8;
    g ^= g >> 4;
    g ^= g >> 2;
    g ^= g >> 1;
    return g;
}

static uint32_t from_gray_loop(uint32_t g) {
    uint32_t b = 0;
    for (; g; g >>= 1)
        b ^= g;
    return b;
}

static int popcount(uint32_t x) {
    int c = 0;
    for (; x; x &= x - 1)
        c++;
    return c;
}

static int ctz(uint32_t x) {
    int c = 0;
    while (!(x & 1u)) {
        x >>= 1;
        c++;
    }
    return c;
}

/* Reflected construction: G(n) = 0G(n-1) followed by 1 reversed(G(n-1)). */
static void build_reflected(uint32_t *out, int bits) {
    out[0] = 0;
    uint32_t len = 1;
    for (int b = 0; b < bits; b++) {
        for (uint32_t i = 0; i < len; i++)
            out[len + i] = out[len - 1 - i] | (1u << b);
        len <<= 1;
    }
}

/* Quadrature decoder: 2-bit Gray sequence 00 01 11 10 means clockwise; returns +1, -1, 0 (no change) or 2 (illegal jump). */
static int quad_step(int prev, int cur) {
    static const int seq[4] = {0, 1, 3, 2};
    int ip = 0, ic = 0;
    for (int i = 0; i < 4; i++) {
        if (seq[i] == prev)
            ip = i;
        if (seq[i] == cur)
            ic = i;
    }
    int d = (ic - ip + 4) % 4;
    return d == 0 ? 0 : (d == 1 ? 1 : (d == 3 ? -1 : 2));
}

int main(void) {
    printf("3-bit sequence:");
    for (uint32_t i = 0; i < 8; i++) {
        printf(" ");
        for (int b = 2; b >= 0; b--)
            putchar('0' + (int)((to_gray(i) >> b) & 1u));
    }
    printf("\n");
    /* exhaustive checks for 16-bit values */
    uint32_t maxstep = 0;
    for (uint32_t i = 0; i < 65536; i++) {
        uint32_t g = to_gray(i);
        check(from_gray(g) == i && from_gray_loop(g) == i, "round trip");
        uint32_t g2 = to_gray(i + 1);
        check(popcount(g ^ g2) == 1, "adjacent codes differ in one bit");
        check((g ^ g2) == (1u << ctz(i + 1)), "flipped bit is ctz(i+1)");
        if ((g ^ g2) > maxstep)
            maxstep = g ^ g2;
    }
    printf("16-bit round trip and single-bit steps verified; highest flipped bit mask %u\n", maxstep);
    /* the sequence is cyclic: last code differs from the first by one bit */
    check(popcount(to_gray(65535) ^ to_gray(0)) == 1, "cyclic");
    /* explicit reflected construction equals the formula */
    uint32_t tab[1024];
    build_reflected(tab, 10);
    for (uint32_t i = 0; i < 1024; i++)
        check(tab[i] == to_gray(i), "reflected construction matches formula");
    /* bit distance property: Hamming distance between codes never exceeds index distance */
    for (int t = 0; t < 5000; t++) {
        uint32_t a = rnd() & 0xFFFFF, b = rnd() & 0xFFFFF;
        uint32_t d = a > b ? a - b : b - a;
        check((uint32_t)popcount(to_gray(a) ^ to_gray(b)) <= d || d == 0, "distance bound");
    }
    /* Gray-order subset walk: maintain a running sum by toggling one element per step */
    int w[12] = {3, 34, 4, 12, 5, 2, 27, 9, 14, 8, 21, 6};
    int sum = 0, hits = 0, best = 0;
    uint32_t mask = 0;
    int target = 60, best_mask_step = 0;
    for (uint32_t i = 1; i < (1u << 12); i++) {
        int b = ctz(i);
        mask ^= 1u << b;
        sum += (mask >> b & 1u) ? w[b] : -w[b];
        if (sum == target)
            hits++;
        if (sum <= target && sum > best) {
            best = sum;
            best_mask_step = (int)i;
        }
    }
    printf("Gray subset walk: %d subsets of the 12 weights sum to %d (best sum <= target %d at step %d)\n", hits,
           target, best, best_mask_step);
    /* verify by direct enumeration */
    int direct = 0;
    for (uint32_t m = 0; m < (1u << 12); m++) {
        int s = 0;
        for (int i = 0; i < 12; i++)
            if (m >> i & 1u)
                s += w[i];
        direct += s == target;
    }
    check(direct == hits + (target == 0), "walk agrees with direct enumeration");
    /* rotary encoder: simulate a noisy shaft moving through 40 detents forward then 15 back */
    int pos_true = 0, pos_dec = 0, prev = 0, illegal = 0;
    static const int seq[4] = {0, 1, 3, 2};
    int phase = 0;
    for (int step = 0; step < 55; step++) {
        int dir = step < 40 ? 1 : -1;
        phase = (phase + dir + 4) % 4;
        pos_true += dir;
        int cur = seq[phase];
        int d = quad_step(prev, cur);
        if (d == 2)
            illegal++;
        else
            pos_dec += d;
        prev = cur;
    }
    printf("encoder: true position %d, decoded %d, illegal transitions %d\n", pos_true, pos_dec, illegal);
    check(pos_true == pos_dec && illegal == 0, "encoder decoding");
    /* skipping a state is detected */
    check(quad_step(0, 3) == 2 && quad_step(0, 2) == -1 && quad_step(1, 1) == 0, "quadrature table");
    return 0;
}
