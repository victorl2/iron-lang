/*
 * title: Combinatorial DP tables: binomial, Stirling and Bell numbers modulo a prime
 * topic: algorithms
 * covers: dynamic programming, Pascal triangle, Stirling numbers of both kinds, Bell triangle, modular arithmetic, inverse via Fermat, factorial closed form cross-check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 88172645463325252ULL;
static unsigned long long rnd(void) {
    rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17;
    return rs;
}
static int rr(int n) { return (int)(rnd() % (unsigned long long)n); }
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s line %d\n", #c, __LINE__); exit(1); } } while (0)

typedef unsigned long long u64;
#define MOD 1000000007ULL
#define N 60

static u64 power(u64 b, u64 e) {
    u64 r = 1;
    b %= MOD;
    while (e) { if (e & 1) r = r * b % MOD; b = b * b % MOD; e >>= 1; }
    return r;
}

int main(void) {
    (void)rr;
    static u64 C[N + 1][N + 1], S2[N + 1][N + 1], S1[N + 1][N + 1];
    for (int n = 0; n <= N; n++) {
        C[n][0] = 1;
        for (int k = 1; k <= n; k++) C[n][k] = (C[n - 1][k - 1] + (k <= n - 1 ? C[n - 1][k] : 0)) % MOD;
    }
    S2[0][0] = 1; S1[0][0] = 1;
    for (int n = 1; n <= N; n++)
        for (int k = 1; k <= n; k++) {
            S2[n][k] = (S2[n - 1][k - 1] + (u64)k * S2[n - 1][k]) % MOD;
            S1[n][k] = (S1[n - 1][k - 1] + (u64)(n - 1) * S1[n - 1][k]) % MOD;
        }
    /* factorials and inverse-factorial closed form for binomials */
    u64 fact[N + 1], inv[N + 1];
    fact[0] = 1;
    for (int i = 1; i <= N; i++) fact[i] = fact[i - 1] * i % MOD;
    for (int i = 0; i <= N; i++) inv[i] = power(fact[i], MOD - 2);
    for (int n = 0; n <= N; n++)
        for (int k = 0; k <= n; k++) CHECK(C[n][k] == fact[n] * inv[k] % MOD * inv[n - k] % MOD);
    /* identities: sum_k S1[n][k] = n!  ; row sums of C = 2^n ; sum_k S2[n][k] k! C(x,k) relation via surjections */
    for (int n = 0; n <= N; n++) {
        u64 a = 0, b = 0;
        for (int k = 0; k <= n; k++) { a = (a + S1[n][k]) % MOD; b = (b + C[n][k]) % MOD; }
        CHECK(a == fact[n]);
        CHECK(b == power(2, (u64)n));
    }
    /* surjections: k! * S2[n][k] = sum_j (-1)^(k-j) C(k,j) j^n */
    for (int n = 1; n <= 12; n++)
        for (int k = 1; k <= n; k++) {
            u64 acc = 0;
            for (int j = 0; j <= k; j++) {
                u64 term = C[k][j] * power((u64)j, (u64)n) % MOD;
                if ((k - j) % 2) acc = (acc + MOD - term) % MOD; else acc = (acc + term) % MOD;
            }
            CHECK(acc == fact[k] * S2[n][k] % MOD);
        }
    /* Bell numbers via Stirling row sum and via the Bell triangle */
    u64 bell[N + 1], tri[N + 2][N + 2];
    tri[0][0] = 1;
    for (int n = 1; n <= N; n++) {
        tri[n][0] = tri[n - 1][n - 1];
        for (int k = 1; k <= n; k++) tri[n][k] = (tri[n][k - 1] + tri[n - 1][k - 1]) % MOD;
    }
    for (int n = 0; n <= N; n++) {
        bell[n] = 0;
        for (int k = 0; k <= n; k++) bell[n] = (bell[n] + S2[n][k]) % MOD;
        CHECK(bell[n] == tri[n][0]);
    }
    printf("Pascal row 10:");
    for (int k = 0; k <= 10; k++) printf(" %llu", C[10][k]);
    printf("\nS2 row 8:");
    for (int k = 1; k <= 8; k++) printf(" %llu", S2[8][k]);
    printf("\nS1 row 7 (unsigned):");
    for (int k = 1; k <= 7; k++) printf(" %llu", S1[7][k]);
    printf("\nBell 0..12:");
    for (int n = 0; n <= 12; n++) printf(" %llu", bell[n]);
    printf("\nC(60,30) mod p=%llu Bell(60) mod p=%llu S2(60,30) mod p=%llu\n", C[60][30], bell[60], S2[60][30]);
    return 0;
}
