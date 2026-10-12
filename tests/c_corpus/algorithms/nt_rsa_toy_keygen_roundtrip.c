/*
 * title: Toy RSA key generation, encryption and signatures
 * topic: algorithms
 * covers: prime generation with Miller-Rabin, seeded PRNG, modular inverse via extended gcd, CRT decryption, blinding-free sign/verify, message chunking
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long u64;
typedef long long i64;

static u64 state = 0x0123456789ABCDEFull;
static u64 rnd(void) {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* moduli stay below 2^62 so plain add/double mulmod is safe */
static u64 mulmod(u64 a, u64 b, u64 m) {
    u64 r = 0;
    a %= m;
    while (b) {
        if (b & 1) { r += a; if (r >= m) r -= m; }
        a += a;
        if (a >= m) a -= m;
        b >>= 1;
    }
    return r;
}
static u64 powmod(u64 b, u64 e, u64 m) {
    u64 r = 1;
    b %= m;
    while (e) { if (e & 1) r = mulmod(r, b, m); b = mulmod(b, b, m); e >>= 1; }
    return r;
}

static int is_prime(u64 n) {
    static const u64 W[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    if (n < 2) return 0;
    for (int i = 0; i < 12; i++) if (n % W[i] == 0) return n == W[i];
    u64 d = n - 1;
    int s = 0;
    while (!(d & 1)) { d >>= 1; s++; }
    for (int i = 0; i < 12; i++) {
        u64 x = powmod(W[i], d, n);
        if (x == 1 || x == n - 1) continue;
        int ok = 0;
        for (int r = 1; r < s; r++) { x = mulmod(x, x, n); if (x == n - 1) { ok = 1; break; } }
        if (!ok) return 0;
    }
    return 1;
}

static u64 gen_prime(int bits) {
    for (;;) {
        u64 c = rnd() & ((1ull << bits) - 1);
        c |= (1ull << (bits - 1)) | (1ull << (bits - 2)) | 1ull;
        if (is_prime(c)) return c;
    }
}

static i64 egcd(i64 a, i64 b, i64 *x, i64 *y) {
    if (!b) { *x = 1; *y = 0; return a; }
    i64 x1, y1, g = egcd(b, a % b, &x1, &y1);
    *x = y1; *y = x1 - (a / b) * y1;
    return g;
}
static u64 modinv(u64 a, u64 m) {
    i64 x, y;
    egcd((i64)a, (i64)m, &x, &y);
    x %= (i64)m;
    return (u64)(x < 0 ? x + (i64)m : x);
}
static u64 isqrt(u64 n) {
    u64 lo = 0, hi = 1ull << 32;
    while (hi - lo > 1) { u64 mid = (lo + hi) / 2; if (mid * mid <= n) lo = mid; else hi = mid; }
    return lo;
}
static u64 gcd(u64 a, u64 b) { while (b) { u64 t = a % b; a = b; b = t; } return a; }

typedef struct { u64 n, e, d, p, q, dp, dq, qinv; } Key;

static void keygen(Key *k, int bits) {
    for (;;) {
        u64 p = gen_prime(bits), q = gen_prime(bits);
        if (p == q) continue;
        u64 lam = (p - 1) / gcd(p - 1, q - 1) * (q - 1);
        u64 e = 65537;
        if (gcd(e, lam) != 1) continue;
        k->p = p > q ? p : q; k->q = p > q ? q : p;
        k->n = p * q; k->e = e; k->d = modinv(e, lam);
        k->dp = k->d % (k->p - 1); k->dq = k->d % (k->q - 1);
        k->qinv = modinv(k->q, k->p);
        return;
    }
}

static u64 decrypt_crt(const Key *k, u64 c) {
    u64 m1 = powmod(c % k->p, k->dp, k->p), m2 = powmod(c % k->q, k->dq, k->q);
    u64 h = mulmod(k->qinv, (m1 + k->p - m2 % k->p) % k->p, k->p);
    return m2 + h * k->q;
}

int main(void) {
    Key k;
    keygen(&k, 30);
    int nbits = 0;
    for (u64 t = k.n; t; t >>= 1) nbits++;
    printf("modulus bits: %d\n", nbits);
    printf("p*q = n check: %d, e = %llu\n", k.p * k.q == k.n, k.e);
    if (!is_prime(k.p) || !is_prime(k.q) || k.p * k.q != k.n) return 1;
    /* roundtrip on random messages, direct and CRT */
    u64 acc = 0;
    for (int i = 0; i < 300; i++) {
        u64 m = rnd() % k.n;
        u64 c = powmod(m, k.e, k.n);
        u64 d1 = powmod(c, k.d, k.n), d2 = decrypt_crt(&k, c);
        if (d1 != m || d2 != m) { fprintf(stderr, "roundtrip failed\n"); return 1; }
        acc ^= c;
    }
    printf("300 random messages round-trip (direct and CRT), ciphertext xor %llu\n", acc);
    /* text encoding: 3 bytes per block */
    const char *msg = "Attack at dawn! Bring the modular inverse.";
    size_t len = strlen(msg);
    printf("plaintext: %s\n", msg);
    u64 cipher[32];
    size_t blocks = 0;
    for (size_t i = 0; i < len; i += 3) {
        u64 m = 0;
        for (size_t j = 0; j < 3; j++) m = m * 256 + (i + j < len ? (unsigned char)msg[i + j] : 0);
        cipher[blocks++] = powmod(m, k.e, k.n);
    }
    char out[64];
    size_t o = 0;
    for (size_t b = 0; b < blocks; b++) {
        u64 m = decrypt_crt(&k, cipher[b]);
        for (int j = 2; j >= 0; j--) out[o + (size_t)j] = (char)(m & 255), m >>= 8;
        o += 3;
    }
    out[len] = 0;
    printf("decrypted: %s (%zu blocks)\n", out, blocks);
    if (strcmp(out, msg) != 0) return 1;
    /* signature: hash (FNV-1a) reduced mod n, sign with d, verify with e */
    u64 h = 1469598103934665603ull;
    for (const char *c = msg; *c; c++) { h ^= (unsigned char)*c; h *= 1099511628211ull; }
    h %= k.n;
    u64 sig = powmod(h, k.d, k.n);
    printf("signature verifies: %d\n", powmod(sig, k.e, k.n) == h);
    u64 tampered = h ^ 1;
    printf("tampered digest verifies: %d\n", powmod(sig, k.e, k.n) == tampered % k.n);
    if (powmod(sig, k.e, k.n) != h || powmod(sig, k.e, k.n) == tampered % k.n) return 1;
    /* homomorphic property: E(a) E(b) = E(ab) mod n */
    u64 a = 12345, b = 67890;
    u64 lhs = mulmod(powmod(a, k.e, k.n), powmod(b, k.e, k.n), k.n);
    printf("multiplicative homomorphism holds: %d\n", lhs == powmod(a * b % k.n, k.e, k.n));
    /* Fermat factoring works when the two primes are close together */
    u64 p2 = gen_prime(20), q2 = p2 + 4000;
    while (!is_prime(q2)) q2++;
    u64 n2 = p2 * q2, x = isqrt(n2);
    if (x * x < n2) x++;
    u64 steps = 0;
    for (;; x++) {
        u64 y2 = x * x - n2, y = isqrt(y2);
        steps++;
        if (y * y == y2) {
            u64 lo = x - y < x + y ? x - y : x + y, hi = x - y < x + y ? x + y : x - y;
            printf("Fermat factoring found both primes: %d after %llu steps\n", (lo == p2 && hi == q2) || (lo == q2 && hi == p2), steps);
            if (lo * hi != n2 || lo == 1) return 1;
            break;
        }
    }
    return 0;
}
