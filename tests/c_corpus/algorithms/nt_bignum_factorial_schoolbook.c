/*
 * title: Bignum factorials and binomials by schoolbook multiplication
 * topic: algorithms
 * covers: multiply by small int, big times big, dynamic limb vectors, digit sums, trailing zeros, central binomial, Legendre formula
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 1000000000ull

typedef struct {
    size_t n, cap;
    unsigned *d; /* base 1e9, little endian */
} Num;

static void num_init(Num *a, unsigned v) {
    a->cap = 8;
    a->d = calloc(a->cap, sizeof *a->d);
    if (!a->d) exit(2);
    a->d[0] = v % BASE;
    a->d[1] = v / BASE;
    a->n = a->d[1] ? 2 : 1;
}
static void num_free(Num *a) { free(a->d); a->d = NULL; }
static void reserve(Num *a, size_t n) {
    if (n <= a->cap) return;
    while (a->cap < n) a->cap *= 2;
    a->d = realloc(a->d, a->cap * sizeof *a->d);
    if (!a->d) exit(2);
}

static void mul_small(Num *a, unsigned m) {
    unsigned long long carry = 0;
    for (size_t i = 0; i < a->n; i++) {
        unsigned long long cur = (unsigned long long)a->d[i] * m + carry;
        a->d[i] = (unsigned)(cur % BASE);
        carry = cur / BASE;
    }
    while (carry) {
        reserve(a, a->n + 1);
        a->d[a->n++] = (unsigned)(carry % BASE);
        carry /= BASE;
    }
}

static void div_small(Num *a, unsigned m) { /* exact or truncating */
    unsigned long long rem = 0;
    for (size_t i = a->n; i-- > 0;) {
        unsigned long long cur = rem * BASE + a->d[i];
        a->d[i] = (unsigned)(cur / m);
        rem = cur % m;
    }
    while (a->n > 1 && a->d[a->n - 1] == 0) a->n--;
}

static Num mul_big(const Num *a, const Num *b) {
    Num r;
    num_init(&r, 0);
    reserve(&r, a->n + b->n + 1);
    memset(r.d, 0, r.cap * sizeof *r.d);
    for (size_t i = 0; i < a->n; i++) {
        unsigned long long carry = 0;
        for (size_t j = 0; j < b->n || carry; j++) {
            unsigned long long cur = r.d[i + j] + carry + (j < b->n ? (unsigned long long)a->d[i] * b->d[j] : 0);
            r.d[i + j] = (unsigned)(cur % BASE);
            carry = cur / BASE;
        }
    }
    r.n = a->n + b->n;
    while (r.n > 1 && r.d[r.n - 1] == 0) r.n--;
    return r;
}

static char *to_dec(const Num *a) {
    size_t cap = a->n * 9 + 2, p = 0;
    char *s = malloc(cap);
    if (!s) exit(2);
    p += (size_t)snprintf(s + p, cap - p, "%u", a->d[a->n - 1]);
    for (size_t i = a->n - 1; i-- > 0;) p += (size_t)snprintf(s + p, cap - p, "%09u", a->d[i]);
    return s;
}

static int digit_sum(const char *s) { int t = 0; for (; *s; s++) t += *s - '0'; return t; }
static int trailing_zeros(const char *s) {
    size_t n = strlen(s);
    int z = 0;
    while (n-- > 0 && s[n] == '0') z++;
    return z;
}

/* exponent of prime p in n! (Legendre) */
static int legendre(int n, int p) { int e = 0; while (n) { n /= p; e += n; } return e; }

int main(void) {
    Num f;
    num_init(&f, 1);
    Num facts[201];
    (void)facts;
    for (int i = 1; i <= 200; i++) {
        mul_small(&f, (unsigned)i);
        if (i == 20 || i == 25 || i == 50 || i == 100 || i == 200) {
            char *s = to_dec(&f);
            printf("%d! has %zu digits, digit sum %d, trailing zeros %d (Legendre %d)\n",
                   i, strlen(s), digit_sum(s), trailing_zeros(s), legendre(i, 5));
            if (trailing_zeros(s) != legendre(i, 5)) { fprintf(stderr, "zeros mismatch\n"); return 1; }
            if (i == 25 || i == 20) printf("  = %s\n", s);
            free(s);
        }
    }
    num_free(&f);

    /* 100! = (product 1..50) * (product 51..100) via big*big */
    Num lo, hi;
    num_init(&lo, 1);
    num_init(&hi, 1);
    for (int i = 1; i <= 50; i++) mul_small(&lo, (unsigned)i);
    for (int i = 51; i <= 100; i++) mul_small(&hi, (unsigned)i);
    Num prod = mul_big(&lo, &hi);
    Num direct;
    num_init(&direct, 1);
    for (int i = 1; i <= 100; i++) mul_small(&direct, (unsigned)i);
    char *a = to_dec(&prod), *b = to_dec(&direct);
    if (strcmp(a, b) != 0) { fprintf(stderr, "big*big mismatch\n"); return 1; }
    printf("100! head: %.30s...\n", a);
    free(a); free(b);
    num_free(&lo); num_free(&hi); num_free(&prod); num_free(&direct);

    /* central binomial C(2n,n) = prod_{k=1..n} (n+k)/k, exact at each step */
    int ns[] = {10, 50, 100};
    for (int t = 0; t < 3; t++) {
        int n = ns[t];
        Num c;
        num_init(&c, 1);
        for (int k = 1; k <= n; k++) {
            mul_small(&c, (unsigned)(n + k));
            div_small(&c, (unsigned)k);
        }
        char *s = to_dec(&c);
        printf("C(%d,%d) = %s\n", 2 * n, n, s);
        free(s);
        num_free(&c);
    }
    /* Catalan(30) = C(60,30)/31, checked against the recurrence in 64-bit */
    Num c;
    num_init(&c, 1);
    for (int k = 1; k <= 30; k++) { mul_small(&c, (unsigned)(30 + k)); div_small(&c, (unsigned)k); }
    div_small(&c, 31);
    unsigned long long cat[31] = {1};
    for (int n = 1; n <= 30; n++) {
        cat[n] = 0;
        for (int i = 0; i < n; i++) cat[n] += cat[i] * cat[n - 1 - i];
    }
    char *s = to_dec(&c);
    char want[32];
    snprintf(want, sizeof want, "%llu", cat[30]);
    if (strcmp(s, want) != 0) { fprintf(stderr, "catalan mismatch\n"); return 1; }
    printf("Catalan(30) = %s\n", s);
    free(s);
    num_free(&c);
    /* 2^1000 by repeated doubling */
    Num p2;
    num_init(&p2, 1);
    for (int i = 0; i < 1000; i++) mul_small(&p2, 2);
    s = to_dec(&p2);
    printf("2^1000: %zu digits, digit sum %d\n", strlen(s), digit_sum(s));
    free(s);
    num_free(&p2);
    return 0;
}
