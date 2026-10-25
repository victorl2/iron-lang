/*
 * title: Totient and Mobius tables from one linear sieve
 * topic: algorithms
 * covers: Euler phi, Mobius mu, Mertens function, linear sieve, phi summatory, coprime pair counting
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define N 30000

static int phi[N + 1], mu[N + 1];
static int primes[4000], np;
static unsigned char comp[N + 1];

static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

int main(void) {
    phi[1] = 1; mu[1] = 1;
    for (int i = 2; i <= N; i++) {
        if (!comp[i]) { primes[np++] = i; phi[i] = i - 1; mu[i] = -1; }
        for (int j = 0; j < np && (long)i * primes[j] <= N; j++) {
            int p = primes[j], m = i * p;
            comp[m] = 1;
            if (i % p == 0) { phi[m] = phi[i] * p; mu[m] = 0; break; }
            phi[m] = phi[i] * (p - 1);
            mu[m] = -mu[i];
        }
    }
    /* brute-force checks on small n */
    for (int n = 1; n <= 400; n++) {
        int c = 0;
        for (int k = 1; k <= n; k++) if (gcd(n, k) == 1) c++;
        if (c != phi[n]) { fprintf(stderr, "phi(%d) wrong\n", n); return 1; }
    }
    /* sum over d|n of phi(d) = n; sum over d|n of mu(d) = [n==1] */
    for (int n = 1; n <= 3000; n++) {
        long sp = 0, sm = 0;
        for (int d = 1; d <= n; d++) if (n % d == 0) { sp += phi[d]; sm += mu[d]; }
        if (sp != n || sm != (n == 1)) { fprintf(stderr, "identity fails at %d\n", n); return 1; }
    }
    int probes[] = {1, 12, 30, 97, 210, 1000, 30000};
    for (size_t t = 0; t < sizeof probes / sizeof probes[0]; t++)
        printf("n=%d phi=%d mu=%d\n", probes[t], phi[probes[t]], mu[probes[t]]);

    /* Mertens function at checkpoints */
    int M = 0;
    int cps[] = {10, 100, 1000, 10000, 30000};
    size_t ci = 0;
    for (int n = 1; n <= N && ci < 5; n++) {
        M += mu[n];
        if (n == cps[ci]) { printf("Mertens(%d) = %d\n", n, M); ci++; }
    }
    /* number of coprime ordered pairs (a,b) in [1,n]^2 = 2*sum phi - 1 = sum mu(d) floor(n/d)^2 */
    int ns[] = {10, 100, 1000, 30000};
    for (size_t t = 0; t < 4; t++) {
        int n = ns[t];
        long s1 = 0, s2 = 0;
        for (int k = 1; k <= n; k++) s1 += phi[k];
        for (int d = 1; d <= n; d++) s2 += (long)mu[d] * (n / d) * (n / d);
        long pairs = 2 * s1 - 1;
        if (pairs != s2) { fprintf(stderr, "pair count mismatch\n"); return 1; }
        long brute = -1;
        if (n <= 1000) {
            brute = 0;
            for (int a = 1; a <= n; a++) for (int b = 1; b <= n; b++) if (gcd(a, b) == 1) brute++;
            if (brute != pairs) { fprintf(stderr, "brute mismatch\n"); return 1; }
        }
        printf("coprime pairs in [1,%d]^2: %ld (sum phi %ld)\n", n, pairs, s1);
    }
    /* squarefree count */
    long sqf = 0;
    for (int n = 1; n <= N; n++) sqf += mu[n] != 0;
    printf("squarefree numbers up to %d: %ld\n", N, sqf);
    int maxphi_ratio_n = 1;
    for (int n = 2; n <= N; n++)
        if ((long)phi[n] * maxphi_ratio_n < (long)phi[maxphi_ratio_n] * n) maxphi_ratio_n = n;
    printf("smallest phi(n)/n ratio at n=%d (phi=%d)\n", maxphi_ratio_n, phi[maxphi_ratio_n]);
    return 0;
}
