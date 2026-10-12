/*
 * title: Karatsuba multiplication against schoolbook
 * topic: algorithms
 * covers: Karatsuba, divide and conquer, threshold cutoff, limb vectors, operation counting, three-multiplication trick
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;
typedef unsigned int u32;

/* little-endian base 2^16 digits stored in u32 */
#define B 65536u

static u64 mul_ops;

static void school(const u32 *a, const u32 *b, size_t n, u32 *out) { /* out has 2n */
    memset(out, 0, 2 * n * sizeof *out);
    for (size_t i = 0; i < n; i++) {
        u64 carry = 0;
        for (size_t j = 0; j < n; j++) {
            u64 cur = out[i + j] + carry + (u64)a[i] * b[j];
            out[i + j] = (u32)(cur % B);
            carry = cur / B;
            mul_ops++;
        }
        out[i + n] += (u32)carry;
    }
}

/* out = a - b where a >= b as n-limb numbers; returns nothing */
static void sub_n(u32 *a, const u32 *b, size_t n) {
    long borrow = 0;
    for (size_t i = 0; i < n; i++) {
        long v = (long)a[i] - (long)b[i] - borrow;
        borrow = v < 0;
        a[i] = (u32)(borrow ? v + (long)B : v);
    }
}

/* Karatsuba for n limbs, n a power of two times a small base (n even at every split) */
static void kara(const u32 *a, const u32 *b, size_t n, u32 *out, size_t cutoff) {
    if (n <= cutoff || (n & 1)) { school(a, b, n, out); return; }
    size_t h = n / 2;
    u32 *z0 = calloc(2 * h, sizeof *z0), *z2 = calloc(2 * h, sizeof *z2);
    u32 *sa = calloc(h + 1, sizeof *sa), *sb = calloc(h + 1, sizeof *sb);
    u32 *z1 = calloc(2 * (h + 1), sizeof *z1);
    if (!z0 || !z2 || !sa || !sb || !z1) exit(2);
    kara(a, b, h, z0, cutoff);
    kara(a + h, b + h, h, z2, cutoff);
    /* (a0 + a1), (b0 + b1) with a carry limb; handle it by padding to h+1 limbs */
    u32 ca = 0, cb = 0;
    for (size_t i = 0; i < h; i++) {
        u32 s = a[i] + a[i + h] + ca; sa[i] = s % B; ca = s / B;
        u32 t = b[i] + b[i + h] + cb; sb[i] = t % B; cb = t / B;
    }
    sa[h] = ca; sb[h] = cb;
    /* multiply (h+1)-limb values by schoolbook padded to even size for simplicity */
    size_t m = h + 1;
    u32 *pa = calloc(m + 1, sizeof *pa), *pb = calloc(m + 1, sizeof *pb), *pz = calloc(2 * (m + 1), sizeof *pz);
    if (!pa || !pb || !pz) exit(2);
    memcpy(pa, sa, m * sizeof *pa);
    memcpy(pb, sb, m * sizeof *pb);
    if (m + 1 <= cutoff || ((m + 1) & 1)) school(pa, pb, m + 1, pz);
    else kara(pa, pb, m + 1, pz, cutoff);
    memcpy(z1, pz, 2 * (m + 1) * sizeof *z1 > 2 * (h + 1) * sizeof *z1 ? 2 * (h + 1) * sizeof *z1 : 2 * (m + 1) * sizeof *z1);
    /* z1 = (a0+a1)(b0+b1) - z0 - z2 (subtract with widened operands) */
    u32 *w0 = calloc(2 * (h + 1), sizeof *w0), *w2 = calloc(2 * (h + 1), sizeof *w2);
    if (!w0 || !w2) exit(2);
    memcpy(w0, z0, 2 * h * sizeof *w0);
    memcpy(w2, z2, 2 * h * sizeof *w2);
    sub_n(z1, w0, 2 * (h + 1));
    sub_n(z1, w2, 2 * (h + 1));
    /* out = z0 + z1 << h limbs + z2 << 2h limbs */
    memset(out, 0, 2 * n * sizeof *out);
    memcpy(out, z0, 2 * h * sizeof *out);
    memcpy(out + 2 * h, z2, 2 * h * sizeof *out);
    u32 carry = 0;
    for (size_t i = 0; i < 2 * (h + 1) && h + i < 2 * n; i++) {
        u32 v = out[h + i] + z1[i] + carry;
        out[h + i] = v % B;
        carry = v / B;
    }
    for (size_t i = h + 2 * (h + 1); carry && i < 2 * n; i++) {
        u32 v = out[i] + carry;
        out[i] = v % B;
        carry = v / B;
    }
    free(z0); free(z2); free(sa); free(sb); free(z1); free(pa); free(pb); free(pz); free(w0); free(w2);
}

static u64 st = 0x123456789ABCDEFull;
static u32 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (u32)(st >> 20) % B; }

int main(void) {
    size_t sizes[] = {2, 4, 8, 16, 32, 64, 128, 256};
    for (size_t t = 0; t < sizeof sizes / sizeof sizes[0]; t++) {
        size_t n = sizes[t];
        u32 *a = malloc(n * sizeof *a), *b = malloc(n * sizeof *b);
        u32 *r1 = malloc(2 * n * sizeof *r1), *r2 = malloc(2 * n * sizeof *r2);
        if (!a || !b || !r1 || !r2) return 2;
        for (size_t i = 0; i < n; i++) { a[i] = rnd(); b[i] = rnd(); }
        if (n >= 4) { a[n - 1] = B - 1; b[n - 1] = B - 1; a[0] = B - 1; b[0] = B - 1; }
        mul_ops = 0;
        school(a, b, n, r1);
        u64 school_ops = mul_ops;
        mul_ops = 0;
        kara(a, b, n, r2, 4);
        u64 kara_ops = mul_ops;
        if (memcmp(r1, r2, 2 * n * sizeof *r1) != 0) { fprintf(stderr, "product mismatch at n=%zu\n", n); return 1; }
        u64 chk = 0;
        for (size_t i = 0; i < 2 * n; i++) chk = chk * 1000003u + r1[i];
        printf("n=%zu limbs: schoolbook %llu limb mults, karatsuba %llu, checksum %llu\n", n, school_ops, kara_ops, chk);
        free(a); free(b); free(r1); free(r2);
    }
    /* all-ones operand stress: (B^n - 1)^2 = B^2n - 2 B^n + 1 */
    size_t n = 64;
    u32 *a = malloc(n * sizeof *a), *r = malloc(2 * n * sizeof *r);
    if (!a || !r) return 2;
    for (size_t i = 0; i < n; i++) a[i] = B - 1;
    kara(a, a, n, r, 4);
    int ok = r[0] == 1 && r[n] == B - 2;
    for (size_t i = 1; i < n; i++) ok = ok && r[i] == 0;
    for (size_t i = n + 1; i < 2 * n; i++) ok = ok && r[i] == B - 1;
    printf("(B^64-1)^2 structure correct: %s\n", ok ? "yes" : "no");
    free(a); free(r);
    return ok ? 0 : 1;
}
