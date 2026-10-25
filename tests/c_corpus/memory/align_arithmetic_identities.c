/*
 * title: Alignment arithmetic helpers and their identities
 * topic: memory
 * covers: align_up, align_down, padding needed, power-of-two masks, exhaustive identity check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static uint32_t align_down(uint32_t x, uint32_t a) {
    return x & ~(a - 1u);
}

static uint32_t align_up(uint32_t x, uint32_t a) {
    return (x + (a - 1u)) & ~(a - 1u);
}

static uint32_t pad_needed(uint32_t x, uint32_t a) {
    return (a - (x & (a - 1u))) & (a - 1u);
}

static int is_aligned(uint32_t x, uint32_t a) {
    return (x & (a - 1u)) == 0;
}

/* generic (non power-of-two) versions built on division */
static uint32_t align_up_div(uint32_t x, uint32_t a) {
    return (x + a - 1u) / a * a;
}

static uint32_t log2_pow2(uint32_t a) {
    uint32_t n = 0;
    while (a > 1u) {
        a >>= 1;
        n++;
    }
    return n;
}

/* alignment of an address = largest power of two dividing it */
static uint32_t alignment_of(uint32_t x) {
    return x == 0 ? 0x80000000u : (x & (~x + 1u));
}

int main(void) {
    unsigned long tested = 0;
    for (uint32_t e = 0; e <= 12; e++) {
        uint32_t a = 1u << e;
        for (uint32_t x = 0; x < 20000; x++) {
            uint32_t up = align_up(x, a);
            uint32_t dn = align_down(x, a);
            check(is_aligned(up, a) && is_aligned(dn, a), "results aligned");
            check(up >= x && dn <= x, "direction");
            check(up - x < a && x - dn < a, "minimal");
            check(up == align_up_div(x, a), "matches division form");
            check(up - x == pad_needed(x, a), "padding");
            check(dn + (x & (a - 1u)) == x, "down + remainder");
            check(align_up(up, a) == up && align_down(dn, a) == dn, "idempotent");
            check((up == x) == is_aligned(x, a), "fixed points");
            check(dn == x / a * a, "down via division");
            tested++;
        }
    }
    printf("identities checked on %lu (x, align) pairs\n", tested);

    /* alignment_of lowest set bit */
    uint32_t vals[] = {1, 2, 3, 4, 12, 40, 96, 1000, 4096, 4104, 65536, 0x30000};
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        uint32_t al = alignment_of(vals[i]);
        check(is_aligned(vals[i], al), "divisible");
        check(!is_aligned(vals[i], al << 1) || al == 0x80000000u, "maximal");
        printf("alignment_of(%u) = %u (2^%u)\n", (unsigned)vals[i], (unsigned)al, (unsigned)log2_pow2(al));
    }

    /* a running layout: place items with sizes and alignments sequentially */
    struct {
        uint32_t size, align;
    } items[] = {{1, 1}, {4, 4}, {2, 2}, {8, 8}, {1, 1}, {16, 16}, {3, 1}, {24, 8}, {5, 32}, {12, 4}};
    uint32_t off = 0, waste = 0, maxal = 1;
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        uint32_t pad = pad_needed(off, items[i].align);
        waste += pad;
        off = align_up(off, items[i].align);
        printf("item %zu size=%2u align=%2u at offset %3u (pad %2u)\n", i, (unsigned)items[i].size,
               (unsigned)items[i].align, (unsigned)off, (unsigned)pad);
        off += items[i].size;
        if (items[i].align > maxal)
            maxal = items[i].align;
    }
    uint32_t total = align_up(off, maxal);
    printf("end=%u total=%u waste=%u tail=%u\n", (unsigned)off, (unsigned)total, (unsigned)waste,
           (unsigned)(total - off));
    check(total >= off && total % maxal == 0, "total aligned");

    /* non power-of-two alignments need the division form */
    uint32_t odd[] = {3, 5, 6, 10, 12, 100};
    for (size_t i = 0; i < sizeof odd / sizeof odd[0]; i++) {
        uint32_t a = odd[i];
        uint32_t x = 1000;
        uint32_t up = align_up_div(x, a);
        check(up % a == 0 && up >= x && up - x < a, "odd align");
        printf("align_up_div(1000, %u) = %u\n", (unsigned)a, (unsigned)up);
    }
    return 0;
}
