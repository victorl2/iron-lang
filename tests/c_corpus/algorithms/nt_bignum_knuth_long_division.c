/*
 * title: Multi-limb long division (Knuth algorithm D)
 * topic: algorithms
 * covers: Knuth D, normalization shift, quotient digit estimation, add-back correction, binary limbs, hex printing
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;

static int addback_count;

static size_t used(const u32 *a, size_t n) { while (n && a[n - 1] == 0) n--; return n; }

/* q gets m-n+1 limbs, r gets n limbs. u has m limbs, v has n >= 2 limbs with v[n-1] != 0. */
static void divmod(const u32 *u, size_t m, const u32 *v, size_t n, u32 *q, u32 *r) {
    int s = 0;
    while (!((v[n - 1] << s) & 0x80000000u)) s++;
    u32 *vn = calloc(n, sizeof *vn), *un = calloc(m + 1, sizeof *un);
    if (!vn || !un) exit(2);
    for (size_t i = n - 1; i > 0; i--) vn[i] = (v[i] << s) | (s ? (u32)((u64)v[i - 1] >> (32 - s)) : 0);
    vn[0] = v[0] << s;
    un[m] = s ? (u32)((u64)u[m - 1] >> (32 - s)) : 0;
    for (size_t i = m - 1; i > 0; i--) un[i] = (u[i] << s) | (s ? (u32)((u64)u[i - 1] >> (32 - s)) : 0);
    un[0] = u[0] << s;
    for (size_t jj = m - n + 1; jj-- > 0;) {
        size_t j = jj;
        u64 num = ((u64)un[j + n] << 32) | un[j + n - 1];
        u64 qhat = num / vn[n - 1], rhat = num % vn[n - 1];
        while (qhat >= (1ull << 32) || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
            qhat--;
            rhat += vn[n - 1];
            if (rhat >= (1ull << 32)) break;
        }
        int64_t borrow = 0;
        u64 carry = 0;
        for (size_t i = 0; i < n; i++) {
            u64 p = qhat * vn[i] + carry;
            carry = p >> 32;
            int64_t t = (int64_t)un[i + j] - borrow - (int64_t)(p & 0xFFFFFFFFu);
            un[i + j] = (u32)t;
            borrow = t < 0 ? 1 : 0;
        }
        int64_t t = (int64_t)un[j + n] - borrow - (int64_t)carry;
        un[j + n] = (u32)t;
        if (t < 0) {
            qhat--;
            addback_count++;
            u64 c = 0;
            for (size_t i = 0; i < n; i++) {
                u64 sum = (u64)un[i + j] + vn[i] + c;
                un[i + j] = (u32)sum;
                c = sum >> 32;
            }
            un[j + n] += (u32)c;
        }
        q[j] = (u32)qhat;
    }
    for (size_t i = 0; i < n; i++) r[i] = (un[i] >> s) | (s ? (u32)((u64)un[i + 1] << (32 - s)) : 0);
    free(vn); free(un);
}

static void mul_add(const u32 *a, size_t na, const u32 *b, size_t nb, const u32 *c, size_t nc, u32 *out) {
    memset(out, 0, (na + nb + 1) * sizeof *out);
    for (size_t i = 0; i < na; i++) {
        u64 carry = 0;
        for (size_t j = 0; j < nb; j++) {
            u64 cur = (u64)out[i + j] + (u64)a[i] * b[j] + carry;
            out[i + j] = (u32)cur;
            carry = cur >> 32;
        }
        out[i + nb] += (u32)carry;
    }
    u64 carry = 0;
    for (size_t i = 0; i < na + nb + 1; i++) {
        u64 cur = (u64)out[i] + (i < nc ? c[i] : 0) + carry;
        out[i] = (u32)cur;
        carry = cur >> 32;
    }
}

static u64 st = 0xDEADBEEFCAFEF00Dull;
static u32 rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (u32)(st >> 16); }

/* full 32-bit limb, sometimes all ones; calls sequenced explicitly */
static u32 limb(void) {
    if ((rnd() & 3) == 0) return 0xFFFFFFFFu;
    u32 hi = rnd();
    u32 lo = rnd();
    return (hi << 16) ^ lo;
}

static void print_hex(const char *label, const u32 *a, size_t n) {
    n = used(a, n);
    printf("%s0x", label);
    if (!n) { printf("0\n"); return; }
    printf("%x", a[n - 1]);
    for (size_t i = n - 1; i-- > 0;) printf("%08x", a[i]);
    printf("\n");
}

int main(void) {
    /* hand-picked case with a near-maximal quotient digit */
    u32 u1[4] = {0, 0, 0x80000000u, 0x7fffffffu}, v1[2] = {1, 0x80000000u};
    u32 q1[3], r1[2];
    divmod(u1, 4, v1, 2, q1, r1);
    print_hex("q1=", q1, 3);
    print_hex("r1=", r1, 2);
    printf("add-back steps so far: %d\n", addback_count);
    u32 chk[8];
    mul_add(q1, 3, v1, 2, r1, 2, chk);
    if (memcmp(chk, u1, sizeof u1) != 0) { fprintf(stderr, "case 1 mismatch\n"); return 1; }

    int cases = 0;
    u64 fold = 0;
    for (int t = 0; t < 400; t++) {
        size_t n = 2 + rnd() % 6, m = n + rnd() % 8;
        u32 u[16], v[8], q[16], r[8], back[32];
        for (size_t i = 0; i < m; i++) u[i] = limb();
        for (size_t i = 0; i < n; i++) v[i] = limb();
        if (v[n - 1] == 0) v[n - 1] = 1;
        if (u[m - 1] == 0) u[m - 1] = 7;
        memset(q, 0, sizeof q);
        divmod(u, m, v, n, q, r);
        mul_add(q, m - n + 1, v, n, r, n, back);
        for (size_t i = 0; i < m; i++) if (back[i] != u[i]) { fprintf(stderr, "reconstruct failed\n"); return 1; }
        if (back[m] != 0) { fprintf(stderr, "reconstruct overflow\n"); return 1; }
        /* remainder < divisor */
        size_t rn = used(r, n);
        int lt = rn < used(v, n);
        if (rn == used(v, n)) for (size_t i = rn; i-- > 0;) { if (r[i] != v[i]) { lt = r[i] < v[i]; break; } }
        if (!lt) { fprintf(stderr, "remainder too big\n"); return 1; }
        for (size_t i = 0; i < n; i++) fold = fold * 31 + r[i];
        for (size_t i = 0; i < m - n + 1; i++) fold = fold * 31 + q[i];
        cases++;
    }
    printf("random cases verified: %d, fold %llu\n", cases, (unsigned long long)fold);
    printf("total add-back corrections: %d\n", addback_count);
    /* 2^128 / (2^64 + 1) */
    u32 num[5] = {0, 0, 0, 0, 1}, den[3] = {1, 0, 1}, q3[3] = {0}, r3[3];
    divmod(num, 5, den, 3, q3, r3);
    print_hex("2^128 / (2^64+1) q=", q3, 3);
    print_hex("remainder=", r3, 3);
    return 0;
}
