/*
 * title: Complex FFT convolution and spectral checks
 * topic: algorithms
 * covers: radix-2 FFT, complex struct arithmetic, convolution, Parseval, real-signal spectrum, rounding to integers, cross-correlation
 * deps: libc, libm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { double re, im; } Cx;

static Cx cadd(Cx a, Cx b) { Cx r = {a.re + b.re, a.im + b.im}; return r; }
static Cx csub(Cx a, Cx b) { Cx r = {a.re - b.re, a.im - b.im}; return r; }
static Cx cmul(Cx a, Cx b) { Cx r = {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; return r; }

static void fft(Cx *a, int n, int invert) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { Cx t = a[i]; a[i] = a[j]; a[j] = t; }
    }
    const double PI = 3.14159265358979323846;
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2 * PI / len * (invert ? -1 : 1);
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < len / 2; j++) {
                Cx w = {cos(ang * j), sin(ang * j)};
                Cx u = a[i + j], v = cmul(a[i + j + len / 2], w);
                a[i + j] = cadd(u, v);
                a[i + j + len / 2] = csub(u, v);
            }
        }
    }
    if (invert) for (int i = 0; i < n; i++) { a[i].re /= n; a[i].im /= n; }
}

static long long *convolve(const long long *a, int na, const long long *b, int nb) {
    int n = 1;
    while (n < na + nb) n <<= 1;
    Cx *fa = calloc((size_t)n, sizeof *fa), *fb = calloc((size_t)n, sizeof *fb);
    long long *out = calloc((size_t)(na + nb), sizeof *out);
    if (!fa || !fb || !out) exit(2);
    for (int i = 0; i < na; i++) fa[i].re = (double)a[i];
    for (int i = 0; i < nb; i++) fb[i].re = (double)b[i];
    fft(fa, n, 0);
    fft(fb, n, 0);
    for (int i = 0; i < n; i++) fa[i] = cmul(fa[i], fb[i]);
    fft(fa, n, 1);
    for (int i = 0; i < na + nb - 1; i++) out[i] = llround(fa[i].re);
    free(fa); free(fb);
    return out;
}

static unsigned st = 99991;
static unsigned rnd(void) { st = st * 1664525u + 1013904223u; return st >> 8; }

int main(void) {
    long long a[] = {1, 2, 3}, b[] = {4, 5, 6, 7};
    long long *c = convolve(a, 3, b, 4);
    printf("[1 2 3] * [4 5 6 7] =");
    for (int i = 0; i < 6; i++) printf(" %lld", c[i]);
    printf("\n");
    free(c);

    /* random integer polynomials vs direct convolution */
    for (int t = 0; t < 6; t++) {
        int na = 1 + (int)(rnd() % 150), nb = 1 + (int)(rnd() % 150);
        long long *x = malloc((size_t)na * sizeof *x), *y = malloc((size_t)nb * sizeof *y);
        if (!x || !y) return 2;
        for (int i = 0; i < na; i++) x[i] = (long long)(rnd() % 2001) - 1000;
        for (int i = 0; i < nb; i++) y[i] = (long long)(rnd() % 2001) - 1000;
        long long *f = convolve(x, na, y, nb);
        long long chk = 0;
        for (int k = 0; k < na + nb - 1; k++) {
            long long s = 0;
            for (int i = 0; i < na; i++) if (k - i >= 0 && k - i < nb) s += x[i] * y[k - i];
            if (s != f[k]) { fprintf(stderr, "convolution mismatch\n"); return 1; }
            chk = (chk * 31 + s) % 1000000007;
        }
        printf("sizes %3d x %3d: exact match, checksum %lld\n", na, nb, chk);
        free(x); free(y); free(f);
    }
    /* Parseval: sum |x|^2 = (1/n) sum |X|^2 */
    enum { N = 64 };
    Cx sig[N];
    double energy_t = 0;
    for (int i = 0; i < N; i++) { sig[i].re = (double)(rnd() % 100) - 50.0; sig[i].im = 0; energy_t += sig[i].re * sig[i].re; }
    Cx spec[N];
    memcpy(spec, sig, sizeof sig);
    fft(spec, N, 0);
    double energy_f = 0;
    for (int i = 0; i < N; i++) energy_f += spec[i].re * spec[i].re + spec[i].im * spec[i].im;
    energy_f /= N;
    printf("Parseval: time energy %.2f, freq energy %.2f, equal: %d\n", energy_t, energy_f, fabs(energy_t - energy_f) < 1e-6 * energy_t);
    if (fabs(energy_t - energy_f) > 1e-6 * energy_t) return 1;
    /* real signal has conjugate-symmetric spectrum */
    int sym = 1;
    for (int k = 1; k < N; k++) if (fabs(spec[k].re - spec[N - k].re) > 1e-6 || fabs(spec[k].im + spec[N - k].im) > 1e-6) sym = 0;
    printf("conjugate symmetry of real spectrum: %d\n", sym);
    /* pure tone at bin 5 lands in bins 5 and N-5 */
    const double PI = 3.14159265358979323846;
    for (int i = 0; i < N; i++) { sig[i].re = cos(2 * PI * 5 * i / N); sig[i].im = 0; }
    fft(sig, N, 0);
    printf("tone spectrum peaks:");
    for (int k = 0; k < N; k++) if (hypot(sig[k].re, sig[k].im) > 1.0) printf(" bin%d=%.1f", k, hypot(sig[k].re, sig[k].im));
    printf("\n");
    /* inverse recovers the original up to rounding */
    Cx x2[N];
    for (int i = 0; i < N; i++) { x2[i].re = (double)(i * i % 17); x2[i].im = 0; }
    Cx y2[N];
    memcpy(y2, x2, sizeof x2);
    fft(y2, N, 0);
    fft(y2, N, 1);
    double maxerr = 0;
    for (int i = 0; i < N; i++) maxerr = fmax(maxerr, fabs(x2[i].re - y2[i].re));
    printf("round trip error below 1e-9: %d\n", maxerr < 1e-9);
    /* cross-correlation to find a shift */
    long long s1[16], s2[16];
    for (int i = 0; i < 16; i++) s1[i] = (long long)(rnd() % 9) - 4;
    int shift = 5;
    for (int i = 0; i < 16; i++) s2[i] = s1[(i + shift) % 16];
    long long rev[16];
    for (int i = 0; i < 16; i++) rev[i] = s2[15 - i];
    long long *cc = convolve(s1, 16, rev, 16);
    int best = 0;
    long long bv = -1000000;
    long long circ[16] = {0};
    for (int k = 0; k < 31; k++) circ[k % 16] += cc[k];
    for (int k = 0; k < 16; k++) if (circ[k] > bv) { bv = circ[k]; best = k; }
    printf("cross-correlation peak index %d (shift %d)\n", best, shift);
    free(cc);
    return maxerr < 1e-9 ? 0 : 1;
}
