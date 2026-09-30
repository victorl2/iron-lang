/*
 * title: Binomial coefficients modulo a prime: Lucas, Wilson, Kummer
 * topic: algorithms
 * covers: Lucas theorem, factorial tables with inverses, Wilson theorem, Kummer carry counting, Pascal triangle parity (Sierpinski), Legendre valuation
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;

static u64 powmod(u64 b, u64 e, u64 m) {
    u64 r = 1 % m;
    b %= m;
    while (e) { if (e & 1) r = r * b % m; b = b * b % m; e >>= 1; }
    return r;
}

static u64 fact_tab[1000], inv_fact[1000];

static void prepare(u64 p) {
    fact_tab[0] = 1;
    for (u64 i = 1; i < p; i++) fact_tab[i] = fact_tab[i - 1] * i % p;
    inv_fact[p - 1] = powmod(fact_tab[p - 1], p - 2, p);
    for (u64 i = p - 1; i > 0; i--) inv_fact[i - 1] = inv_fact[i] * i % p;
}

static u64 small_binom(u64 n, u64 k, u64 p) {
    if (k > n) return 0;
    return fact_tab[n] * inv_fact[k] % p * inv_fact[n - k] % p;
}

static u64 lucas(u64 n, u64 k, u64 p) {
    u64 r = 1;
    while (n || k) {
        u64 a = n % p, b = k % p;
        if (b > a) return 0;
        r = r * small_binom(a, b, p) % p;
        n /= p; k /= p;
    }
    return r;
}

/* exponent of p in C(n,k) = number of carries when adding k and n-k in base p */
static int carries(u64 a, u64 b, u64 p) {
    int c = 0;
    u64 carry = 0;
    while (a || b || carry) {
        u64 s = a % p + b % p + carry;
        carry = s >= p;
        c += (int)carry;
        a /= p; b /= p;
    }
    return c;
}

static int legendre_val(u64 n, u64 p) { int e = 0; while (n) { n /= p; e += (int)n; } return e; }

static u64 pascal[64][64];

int main(void) {
    u64 p = 13;
    prepare(p);
    /* Wilson: (p-1)! = -1 mod p */
    u64 primes[] = {2, 3, 5, 7, 11, 13, 101, 997};
    printf("Wilson check (p-1)! mod p:");
    for (size_t i = 0; i < 8; i++) {
        prepare(primes[i]);
        printf(" %llu:%llu", primes[i], fact_tab[primes[i] - 1]);
        if (fact_tab[primes[i] - 1] != primes[i] - 1) return 1;
    }
    printf("\n");
    /* composite n fails Wilson unless n = 4 */
    printf("composites with (n-1)! = 0 mod n up to 30:");
    for (u64 n = 4; n <= 30; n++) {
        int comp = 0;
        for (u64 d = 2; d * d <= n; d++) if (n % d == 0) comp = 1;
        if (!comp) continue;
        u64 f = 1;
        for (u64 i = 2; i < n; i++) f = f * i % n;
        if (f == 0) printf(" %llu", n);
        else if (n == 4) printf(" [4 gives %llu]", f);
    }
    printf("\n");

    /* Lucas vs exact Pascal triangle rows for several primes */
    for (int i = 0; i < 64; i++) { pascal[i][0] = 1; for (int j = 1; j <= i; j++) pascal[i][j] = pascal[i - 1][j - 1] + (j < i ? pascal[i - 1][j] : 0); }
    u64 ps[] = {2, 3, 5, 7, 11, 13};
    for (size_t t = 0; t < 6; t++) {
        u64 q = ps[t];
        prepare(q);
        int zeros = 0, cells = 0;
        for (int n = 0; n < 62; n++) for (int k = 0; k <= n; k++) {
            u64 exact = pascal[n][k] % q, l = lucas((u64)n, (u64)k, q);
            if (exact != l) { fprintf(stderr, "lucas mismatch p=%llu n=%d k=%d\n", q, n, k); return 1; }
            int c = carries((u64)k, (u64)(n - k), q);
            int val = 0;
            u64 v = pascal[n][k];
            while (v && v % q == 0) { v /= q; val++; }
            if (val != c || val != legendre_val((u64)n, q) - legendre_val((u64)k, q) - legendre_val((u64)(n - k), q)) {
                fprintf(stderr, "kummer/legendre mismatch\n");
                return 1;
            }
            zeros += l == 0;
            cells++;
        }
        printf("p=%llu: rows 0..61 have %d cells divisible by p out of %d\n", q, zeros, cells);
    }
    /* Sierpinski: odd entries of row n number 2^popcount(n) */
    printf("odd entries in row n as 2^popcount(n):");
    for (int n = 0; n < 32; n += 5) {
        int odd = 0;
        for (int k = 0; k <= n; k++) odd += (int)(pascal[n][k] & 1);
        int pc = 0;
        for (int t = n; t; t &= t - 1) pc++;
        printf(" n=%d:%d", n, odd);
        if (odd != (1 << pc)) return 1;
    }
    printf("\n");
    /* huge arguments */
    p = 997;
    prepare(p);
    u64 big[][2] = {{1000000000000ull, 500000000000ull}, {123456789012345ull, 12345ull}, {997ull * 997 * 997 - 1, 997ull * 500}};
    for (int i = 0; i < 3; i++)
        printf("C(%llu, %llu) mod 997 = %llu\n", big[i][0], big[i][1], lucas(big[i][0], big[i][1], p));
    /* n! with p-factors removed, mod p */
    p = 101;
    prepare(p);
    printf("v_101(10^6!) = %d\n", legendre_val(1000000, p));
    return 0;
}
