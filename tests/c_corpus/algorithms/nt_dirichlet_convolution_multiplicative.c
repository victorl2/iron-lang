/*
 * title: Dirichlet convolution of arithmetic functions
 * topic: algorithms
 * covers: Dirichlet convolution, multiplicative functions, identity 1*mu=e, phi*1=id, Mobius inversion, function pointers
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

#define N 2000

typedef long long i64;
typedef i64 (*arith_fn)(int);

static i64 f_one(int n) { (void)n; return 1; }
static i64 f_id(int n) { return n; }
static i64 f_eps(int n) { return n == 1; }
static i64 f_mu(int n) {
    int r = 1;
    for (int p = 2; p * p <= n; p++) {
        if (n % p) continue;
        n /= p;
        if (n % p == 0) return 0;
        r = -r;
    }
    if (n > 1) r = -r;
    return r;
}
static i64 f_phi(int n) {
    i64 r = n;
    for (int p = 2; p * p <= n; p++) {
        if (n % p) continue;
        while (n % p == 0) n /= p;
        r -= r / p;
    }
    if (n > 1) r -= r / n;
    return r;
}
static i64 f_tau(int n) { i64 c = 0; for (int d = 1; d <= n; d++) c += n % d == 0; return c; }
static i64 f_sigma(int n) { i64 c = 0; for (int d = 1; d <= n; d++) if (n % d == 0) c += d; return c; }
static i64 f_lambda(int n) { /* Liouville: (-1)^Omega */
    int o = 0;
    for (int p = 2; p * p <= n; p++) while (n % p == 0) { n /= p; o++; }
    if (n > 1) o++;
    return o & 1 ? -1 : 1;
}

static i64 tab[8][N + 1];

static void materialize(arith_fn f, i64 *t) { for (int n = 1; n <= N; n++) t[n] = f(n); }

static void convolve(const i64 *a, const i64 *b, i64 *out) {
    for (int n = 1; n <= N; n++) out[n] = 0;
    for (int d = 1; d <= N; d++)
        for (int m = d; m <= N; m += d) out[m] += a[d] * b[m / d];
}

static int equal(const i64 *a, const i64 *b) {
    for (int n = 1; n <= N; n++) if (a[n] != b[n]) return 0;
    return 1;
}

/* Dirichlet inverse of f with f(1)=1 */
static void dinverse(const i64 *f, i64 *g) {
    g[1] = 1;
    for (int n = 2; n <= N; n++) {
        i64 s = 0;
        for (int d = 2; d <= n; d++) if (n % d == 0) s += f[d] * g[n / d];
        g[n] = -s;
    }
}

int main(void) {
    arith_fn fs[] = {f_one, f_id, f_eps, f_mu, f_phi, f_tau, f_sigma, f_lambda};
    const char *names[] = {"1", "id", "eps", "mu", "phi", "tau", "sigma", "lambda"};
    for (int i = 0; i < 8; i++) materialize(fs[i], tab[i]);
    static i64 tmp[N + 1], tmp2[N + 1];
    struct { int a, b, c; } ids[] = {
        {0, 3, 2},  /* 1 * mu = eps */
        {4, 0, 1},  /* phi * 1 = id */
        {0, 0, 5},  /* 1 * 1 = tau */
        {1, 0, 6},  /* id * 1 = sigma */
        {3, 1, 4},  /* mu * id = phi */
        {7, 0, -1}, /* lambda * 1 = indicator of squares */
    };
    for (size_t k = 0; k < sizeof ids / sizeof ids[0]; k++) {
        convolve(tab[ids[k].a], tab[ids[k].b], tmp);
        if (ids[k].c >= 0) {
            if (!equal(tmp, tab[ids[k].c])) { fprintf(stderr, "identity %zu fails\n", k); return 1; }
            printf("%s * %s = %s : verified up to %d\n", names[ids[k].a], names[ids[k].b], names[ids[k].c], N);
        } else {
            int squares = 0;
            for (int n = 1; n <= N; n++) {
                int r = 0;
                while (r * r < n) r++;
                if (tmp[n] != (r * r == n)) { fprintf(stderr, "lambda*1 fails\n"); return 1; }
                squares += tmp[n];
            }
            printf("lambda * 1 = [n is a square]: %d squares up to %d\n", squares, N);
        }
    }
    /* Dirichlet inverse of 1 is mu; of id is mu*id; associativity */
    dinverse(tab[0], tmp);
    if (!equal(tmp, tab[3])) { fprintf(stderr, "inverse of 1 not mu\n"); return 1; }
    dinverse(tab[1], tmp);
    convolve(tmp, tab[1], tmp2);
    if (!equal(tmp2, tab[2])) { fprintf(stderr, "inverse of id wrong\n"); return 1; }
    printf("inverse of id at 1..12:");
    for (int n = 1; n <= 12; n++) printf(" %lld", tmp[n]);
    printf("\n");
    static i64 l[N + 1], r[N + 1];
    convolve(tab[4], tab[5], tmp);
    convolve(tmp, tab[3], r);
    convolve(tab[5], tab[3], tmp);
    convolve(tab[4], tmp, l);
    printf("associativity (phi*tau)*mu vs phi*(tau*mu): %s\n", equal(l, r) ? "equal" : "DIFFER");
    if (!equal(l, r)) return 1;
    /* multiplicativity spot check */
    int mult_ok = 1;
    for (int a = 1; a <= 40; a++) for (int b = 1; b <= 40; b++) {
        int g = a, h = b;
        while (h) { int t = g % h; g = h; h = t; }
        if (g != 1 || a * b > N) continue;
        for (int i = 3; i < 8; i++) if (tab[i][a * b] != tab[i][a] * tab[i][b]) mult_ok = 0;
    }
    printf("mu, phi, tau, sigma, lambda multiplicative on coprime pairs: %s\n", mult_ok ? "yes" : "no");
    for (int i = 3; i < 8; i++) printf("%s(360) = %lld\n", names[i], tab[i][360]);
    return mult_ok ? 0 : 1;
}
