/*
 * title: Number-theoretic transform polynomial multiplication
 * topic: algorithms
 * covers: NTT mod 998244353, bit reversal, roots of unity, convolution theorem, polynomial power and inverse, schoolbook cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;
typedef unsigned int u32;
#define P 998244353ull
#define G 3ull

static u64 powmod(u64 b, u64 e) {
    u64 r = 1;
    b %= P;
    while (e) { if (e & 1) r = r * b % P; b = b * b % P; e >>= 1; }
    return r;
}

static void ntt(u32 *a, int n, int invert) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { u32 t = a[i]; a[i] = a[j]; a[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        u64 w = powmod(G, (P - 1) / (u64)len);
        if (invert) w = powmod(w, P - 2);
        for (int i = 0; i < n; i += len) {
            u64 cur = 1;
            for (int j = 0; j < len / 2; j++) {
                u32 u = a[i + j];
                u32 v = (u32)((u64)a[i + j + len / 2] * cur % P);
                u32 x = u + v, y = u >= v ? u - v : u + (u32)P - v;
                a[i + j] = x >= P ? x - (u32)P : x;
                a[i + j + len / 2] = y;
                cur = cur * w % P;
            }
        }
    }
    if (invert) {
        u64 ninv = powmod((u64)n, P - 2);
        for (int i = 0; i < n; i++) a[i] = (u32)((u64)a[i] * ninv % P);
    }
}

/* returns result length na+nb-1 in out */
static int poly_mul(const u32 *a, int na, const u32 *b, int nb, u32 *out) {
    int n = 1;
    while (n < na + nb) n <<= 1;
    u32 *fa = calloc((size_t)n, sizeof *fa), *fb = calloc((size_t)n, sizeof *fb);
    if (!fa || !fb) exit(2);
    memcpy(fa, a, (size_t)na * sizeof *a);
    memcpy(fb, b, (size_t)nb * sizeof *b);
    ntt(fa, n, 0);
    ntt(fb, n, 0);
    for (int i = 0; i < n; i++) fa[i] = (u32)((u64)fa[i] * fb[i] % P);
    ntt(fa, n, 1);
    memcpy(out, fa, (size_t)(na + nb - 1) * sizeof *out);
    free(fa); free(fb);
    return na + nb - 1;
}

static void school(const u32 *a, int na, const u32 *b, int nb, u32 *out) {
    for (int i = 0; i < na + nb - 1; i++) out[i] = 0;
    for (int i = 0; i < na; i++) for (int j = 0; j < nb; j++) out[i + j] = (u32)((out[i + j] + (u64)a[i] * b[j]) % P);
}

static u64 st = 0x2545F4914F6CDD1Dull;
static u32 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (u32)((st >> 11) % P); }

int main(void) {
    /* (1 + x)^n via repeated multiplication gives binomials */
    u32 cur[128] = {1}, one_x[2] = {1, 1}, tmp[130];
    int len = 1;
    for (int i = 0; i < 20; i++) { len = poly_mul(cur, len, one_x, 2, tmp); memcpy(cur, tmp, (size_t)len * sizeof *cur); }
    printf("(1+x)^20 coefficients:");
    for (int i = 0; i < len; i++) printf(" %u", cur[i]);
    printf("\n");

    /* random polynomials vs schoolbook */
    int sizes[][2] = {{1, 1}, {3, 5}, {17, 33}, {100, 64}, {200, 200}};
    for (size_t t = 0; t < 5; t++) {
        int na = sizes[t][0], nb = sizes[t][1];
        u32 *a = malloc((size_t)na * sizeof *a), *b = malloc((size_t)nb * sizeof *b);
        u32 *r1 = malloc((size_t)(na + nb) * sizeof *r1), *r2 = malloc((size_t)(na + nb) * sizeof *r2);
        if (!a || !b || !r1 || !r2) return 2;
        for (int i = 0; i < na; i++) a[i] = rnd();
        for (int i = 0; i < nb; i++) b[i] = rnd();
        poly_mul(a, na, b, nb, r1);
        school(a, na, b, nb, r2);
        if (memcmp(r1, r2, (size_t)(na + nb - 1) * sizeof *r1) != 0) { fprintf(stderr, "ntt mismatch\n"); return 1; }
        u64 chk = 0;
        for (int i = 0; i < na + nb - 1; i++) chk = (chk * 131 + r1[i]) % P;
        printf("deg %d x deg %d product checksum %llu\n", na - 1, nb - 1, chk);
        free(a); free(b); free(r1); free(r2);
    }
    /* multiply big integers written in base 10 using convolution then carrying */
    const char *sa = "123456789012345678901234567890", *sb = "987654321098765432109876543210";
    int la = (int)strlen(sa), lb = (int)strlen(sb);
    u32 da[64], db[64], dr[128];
    for (int i = 0; i < la; i++) da[i] = (u32)(sa[la - 1 - i] - '0');
    for (int i = 0; i < lb; i++) db[i] = (u32)(sb[lb - 1 - i] - '0');
    int lr = poly_mul(da, la, db, lb, dr);
    u64 carry = 0;
    char digits[130];
    int nd = 0;
    for (int i = 0; i < lr || carry; i++) {
        u64 v = carry + (i < lr ? dr[i] : 0);
        digits[nd++] = (char)('0' + v % 10);
        carry = v / 10;
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    printf("product of two 30-digit numbers: ");
    for (int i = nd - 1; i >= 0; i--) putchar(digits[i]);
    printf("\n");
    /* the NTT of a delta is all ones; inverse round trip */
    u32 d[16] = {0};
    d[0] = 1;
    ntt(d, 16, 0);
    int ones = 1;
    for (int i = 0; i < 16; i++) ones &= d[i] == 1;
    ntt(d, 16, 1);
    printf("NTT(delta) all ones: %d, round trip restores delta: %d\n", ones, d[0] == 1 && d[1] == 0);
    /* primitive 8th root of unity */
    u64 w8 = powmod(G, (P - 1) / 8);
    printf("w8 = %llu, w8^4 = %llu, w8^8 = %llu\n", w8, powmod(w8, 4), powmod(w8, 8));
    return ones ? 0 : 1;
}
