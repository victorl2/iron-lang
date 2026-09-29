/*
 * title: Arithmetic from bitwise operations and GF(2^8) multiplication
 * topic: algorithms
 * covers: ripple carry addition, two's complement, shift-add multiplication, restoring division, carry-less multiply, GF(256), Rijndael field
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t st = 0xB5297A4Du;

static uint32_t rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* addition with no + operator: sum bits are xor, carries are and-shifted */
static uint32_t add_bits(uint32_t a, uint32_t b, int *iterations) {
    *iterations = 0;
    while (b) {
        uint32_t carry = (a & b) << 1;
        a ^= b;
        b = carry;
        (*iterations)++;
    }
    return a;
}

static uint32_t neg_bits(uint32_t a) {
    int it;
    return add_bits(~a, 1u, &it);
}

static uint32_t sub_bits(uint32_t a, uint32_t b) {
    int it;
    return add_bits(a, neg_bits(b), &it);
}

static uint32_t mul_bits(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    int it;
    while (b) {
        if (b & 1u)
            r = add_bits(r, a, &it);
        a <<= 1;
        b >>= 1;
    }
    return r;
}

/* restoring division, one quotient bit per step */
static void divmod_bits(uint32_t n, uint32_t d, uint32_t *q, uint32_t *r) {
    uint32_t quo = 0, rem = 0;
    for (int i = 31; i >= 0; i--) {
        rem = (rem << 1) | ((n >> i) & 1u);
        if (rem >= d) {
            rem = sub_bits(rem, d);
            quo |= 1u << i;
        }
    }
    *q = quo;
    *r = rem;
}

static int lt_bits(uint32_t a, uint32_t b) {
    /* unsigned compare by scanning the first differing bit from the top */
    uint32_t diff = a ^ b;
    if (!diff)
        return 0;
    uint32_t top = diff;
    top |= top >> 1;
    top |= top >> 2;
    top |= top >> 4;
    top |= top >> 8;
    top |= top >> 16;
    top ^= top >> 1; /* highest differing bit */
    return (b & top) != 0;
}

/* carry-less product of two 8-bit polynomials over GF(2) */
static uint32_t clmul8(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int i = 0; i < 8; i++)
        if ((b >> i) & 1u)
            r ^= a << i;
    return r;
}

/* reduce modulo x^8 + x^4 + x^3 + x + 1 (0x11B) */
static uint32_t reduce_aes(uint32_t p) {
    for (int bit = 14; bit >= 8; bit--)
        if ((p >> bit) & 1u)
            p ^= 0x11Bu << (bit - 8);
    return p;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    return (uint8_t)reduce_aes(clmul8(a, b));
}

/* the classic Russian-peasant form with xtime */
static uint8_t gf_mul_peasant(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1u)
            p ^= a;
        uint8_t hi = a & 0x80u;
        a = (uint8_t)(a << 1);
        if (hi)
            a ^= 0x1Bu;
        b >>= 1;
    }
    return p;
}

static uint8_t gf_pow(uint8_t a, unsigned e) {
    uint8_t r = 1;
    while (e) {
        if (e & 1u)
            r = gf_mul(r, a);
        a = gf_mul(a, a);
        e >>= 1;
    }
    return r;
}

int main(void) {
    int it;
    unsigned sum57 = add_bits(57, 13, &it);
    printf("57+13=%u  (iterations %d)\n", sum57, it);
    long total_iter = 0, samples = 0;
    int worst = 0;
    for (int t = 0; t < 20000; t++) {
        uint32_t a = rnd(), b = rnd();
        if (t % 4 == 0)
            b = (uint32_t)(0u - (a & 0xFFFFu)); /* long carry chains */
        uint32_t s = add_bits(a, b, &it);
        check(s == a + b, "addition");
        check(sub_bits(a, b) == a - b, "subtraction");
        check(neg_bits(a) == 0u - a, "negation");
        check(lt_bits(a, b) == (a < b), "unsigned comparison");
        total_iter += it;
        samples++;
        if (it > worst)
            worst = it;
    }
    printf("ripple additions: mean iterations %.2f over %ld samples, worst %d\n", (double)total_iter / (double)samples,
           samples, worst);
    for (int t = 0; t < 5000; t++) {
        uint32_t a = rnd();
        a >>= rnd() % 20;
        uint32_t b = rnd();
        b >>= rnd() % 30;
        check(mul_bits(a, b) == a * b, "multiplication");
        if (b) {
            uint32_t q, r;
            divmod_bits(a, b, &q, &r);
            check(q == a / b && r == a % b, "division");
        }
    }
    uint32_t q, r;
    divmod_bits(1000000007u, 97u, &q, &r);
    printf("1000000007 divmod 97 = %u rem %u; 12345*6789 = %u\n", q, r, mul_bits(12345u, 6789u));
    check(mul_bits(12345u, 6789u) == 83810205u, "known product");
    /* GF(2^8): the AES field */
    check(gf_mul(0x57, 0x83) == 0xC1, "FIPS-197 example 0x57 * 0x83 = 0xC1");
    check(gf_mul(0x57, 0x13) == 0xFE, "FIPS-197 example 0x57 * 0x13 = 0xFE");
    printf("gf(256): 57*83=%02x 57*13=%02x\n", gf_mul(0x57, 0x83), gf_mul(0x57, 0x13));
    for (int a = 0; a < 256; a++)
        for (int b = 0; b < 256; b++) {
            check(gf_mul((uint8_t)a, (uint8_t)b) == gf_mul_peasant((uint8_t)a, (uint8_t)b), "two multipliers agree");
            check(gf_mul((uint8_t)a, (uint8_t)b) == gf_mul((uint8_t)b, (uint8_t)a), "commutative");
        }
    /* distributivity over xor (field addition) on a sample, and inverses via a^254 */
    for (int t = 0; t < 2000; t++) {
        uint8_t a = (uint8_t)rnd(), b = (uint8_t)rnd(), c = (uint8_t)rnd();
        check(gf_mul(a, (uint8_t)(b ^ c)) == (uint8_t)(gf_mul(a, b) ^ gf_mul(a, c)), "distributive");
        check(gf_mul(gf_mul(a, b), c) == gf_mul(a, gf_mul(b, c)), "associative");
    }
    uint8_t inv[256];
    inv[0] = 0;
    unsigned inv_sum = 0;
    for (int a = 1; a < 256; a++) {
        inv[a] = gf_pow((uint8_t)a, 254);
        check(gf_mul((uint8_t)a, inv[a]) == 1, "inverse");
        inv_sum += inv[a];
    }
    printf("inverse of 0x53 = 0x%02x, inverse of 0x02 = 0x%02x, sum of all inverses = %u\n", inv[0x53], inv[0x02], inv_sum);
    check(inv[0x53] == 0xCA, "0x53 inverse is 0xCA");
    /* generator 3 has order 255 in this field */
    uint8_t g = 1;
    int order = 0;
    do {
        g = gf_mul(g, 3);
        order++;
    } while (g != 1);
    printf("order of 3 in GF(256)* = %d\n", order);
    check(order == 255, "3 generates the multiplicative group");
    return 0;
}
