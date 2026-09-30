/*
 * title: RFC 1982 serial number arithmetic
 * topic: networking
 * covers: wraparound comparison, undefined half-space case, exhaustive 8-bit proof, DNS SOA serial rollover, non-transitivity
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static unsigned rng_state = 1u;
static unsigned rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static void fail(const char *m) {
    fprintf(stderr, "check failed: %s\n", m);
    exit(1);
}
static void check(int c, const char *m) { if (!c) fail(m); }

typedef enum { S_EQ, S_LT, S_GT, S_UNDEF } Cmp;
static const char *cname[] = {"==", "<", ">", "undefined"};

/* Reference definition straight from RFC 1982 section 3.2, using wide integers and a modulus parameter. */
static Cmp ref_cmp(long i1, long i2, long two_n) {
    long half = two_n / 2;
    if (i1 == i2) return S_EQ;
    if ((i1 < i2 && i2 - i1 < half) || (i1 > i2 && i1 - i2 > half)) return S_LT;
    if ((i1 < i2 && i2 - i1 > half) || (i1 > i2 && i1 - i2 < half)) return S_GT;
    return S_UNDEF;
}
/* Bit trick used in practice: interpret the modular difference as signed. */
static Cmp fast_cmp32(uint32_t a, uint32_t b) {
    uint32_t d = a - b;
    if (d == 0) return S_EQ;
    if (d == 0x80000000u) return S_UNDEF;
    return d & 0x80000000u ? S_LT : S_GT;
}
static Cmp fast_cmp8(uint8_t a, uint8_t b) {
    uint8_t d = (uint8_t)(a - b);
    if (d == 0) return S_EQ;
    if (d == 0x80) return S_UNDEF;
    return d & 0x80 ? S_LT : S_GT;
}
static uint32_t serial_add32(uint32_t s, uint32_t n) {
    if (n > 0x7fffffffu) fail("addend outside 0..2^(N-1)-1");
    return s + n;
}

static uint32_t soa_serial(int y, int m, int d, int rev) {
    return (uint32_t)(((y * 100 + m) * 100 + d) * 100 + rev);
}

int main(void) {
    /* exhaustive 8-bit proof: fast form equals the RFC definition, plus algebraic properties */
    long counts[4] = {0, 0, 0, 0};
    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b++) {
            Cmp r = ref_cmp(a, b, 256), f = fast_cmp8((uint8_t)a, (uint8_t)b);
            check(r == f, "8-bit fast comparison matches RFC definition");
            counts[r]++;
            Cmp rev = fast_cmp8((uint8_t)b, (uint8_t)a);
            if (r == S_LT) check(rev == S_GT, "antisymmetry");
            if (r == S_UNDEF) check(rev == S_UNDEF, "undefined is symmetric");
        }
    printf("8-bit pairs: equal=%ld less=%ld greater=%ld undefined=%ld\n", counts[S_EQ], counts[S_LT], counts[S_GT],
           counts[S_UNDEF]);
    check(counts[S_UNDEF] == 256 && counts[S_LT] == counts[S_GT], "exactly N pairs are undefined");

    /* addition: s < s+n for every 1 <= n <= 127, and s+n wraps but still compares greater */
    long wraps = 0;
    for (int s = 0; s < 256; s++)
        for (int n = 1; n <= 127; n++) {
            uint8_t t = (uint8_t)(s + n);
            check(fast_cmp8((uint8_t)s, t) == S_LT, "s < s + n");
            if (t < s) wraps++;
        }
    printf("8-bit s+n cases that wrapped past 255: %ld of %d\n", wraps, 256 * 127);
    /* n = 128 lands exactly on the undefined point */
    check(fast_cmp8(10, (uint8_t)(10 + 128)) == S_UNDEF, "s+128 is undefined against s");

    /* non-transitivity, RFC 1982 section 3.2 example flavour */
    uint8_t a = 0, b = 100, c = 200;
    printf("non-transitive triple: %u<%u=%s %u<%u=%s %u<%u=%s\n", a, b, fast_cmp8(a, b) == S_LT ? "yes" : "no", b, c,
           fast_cmp8(b, c) == S_LT ? "yes" : "no", a, c, fast_cmp8(a, c) == S_LT ? "yes" : "no");
    check(fast_cmp8(a, b) == S_LT && fast_cmp8(b, c) == S_LT && fast_cmp8(c, a) == S_LT, "cycle a<b<c<a");

    /* 32-bit: sampled agreement with the reference (64-bit modulus arithmetic) */
    rng_state = 1982u;
    long disagree = 0;
    uint32_t ss[] = {0u, 1u, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu};
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 6; j++) {
            Cmp r = ref_cmp((long)ss[i], (long)ss[j], 4294967296L), f = fast_cmp32(ss[i], ss[j]);
            if (r != f) disagree++;
            printf("  %08x %s %08x\n", (unsigned)ss[i], cname[f], (unsigned)ss[j]);
        }
    for (int i = 0; i < 20000; i++) {
        uint32_t x = rnd();
        uint32_t y = rnd();
        if (ref_cmp((long)x, (long)y, 4294967296L) != fast_cmp32(x, y)) disagree++;
    }
    check(disagree == 0, "32-bit fast comparison matches reference");

    /* DNS SOA serial: date-based YYYYMMDDnn, and the RFC 1982 section 7 trick for lowering a serial */
    uint32_t serial = soa_serial(2024, 12, 31, 99);
    printf("SOA serial %u\n", (unsigned)serial);
    uint32_t next = serial_add32(serial, 1);
    check(fast_cmp32(serial, next) == S_LT, "next serial is newer");
    /* Zone operator wants a smaller serial (e.g. reset to 1) while slaves hold a large one. */
    uint32_t held = 2000000000u;
    uint32_t want = 1u;
    check(fast_cmp32(held, want) == S_GT, "slaves consider their serial newer than the target");
    printf("naive reset: %u %s %u (slaves ignore it)\n", (unsigned)held, cname[fast_cmp32(held, want)], (unsigned)want);
    /* walk the standard procedure until the target serial is newer than what slaves hold */
    uint32_t cur = held;
    int steps = 0;
    while (fast_cmp32(cur, want) != S_LT) {
        uint32_t jump = 0x7fffffffu;
        uint32_t nxt = serial_add32(cur, jump);
        if (fast_cmp32(cur, nxt) != S_LT) fail("jump must be newer");
        cur = nxt;
        steps++;
        if (steps > 4) fail("too many steps");
        /* after each jump, slaves have transferred and hold cur */
    }
    printf("slaves reach serial 1 as 'newer' after %d jump(s); holding %u\n", steps, (unsigned)cur);
    check(fast_cmp32(cur, want) == S_LT, "reset succeeded");
    return 0;
}
