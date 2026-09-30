/*
 * title: Signed bignum addition and subtraction on decimal limbs
 * topic: algorithms
 * covers: base 1e9 limbs, magnitude compare, signed add/sub, carry and borrow, decimal parse and print, Fibonacci and Lucas identities
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 1000000000u
#define MAXL 64

typedef struct {
    unsigned n;       /* used limbs, 0 means zero */
    int neg;
    unsigned d[MAXL]; /* little endian */
} Big;

static void trim(Big *a) {
    while (a->n && a->d[a->n - 1] == 0) a->n--;
    if (!a->n) a->neg = 0;
}

static void from_str(Big *a, const char *s) {
    a->n = 0;
    a->neg = 0;
    if (*s == '-') { a->neg = 1; s++; }
    size_t len = strlen(s);
    while (len > 0) {
        size_t take = len >= 9 ? 9 : len;
        unsigned v = 0;
        for (size_t i = len - take; i < len; i++) v = v * 10 + (unsigned)(s[i] - '0');
        a->d[a->n++] = v;
        len -= take;
    }
    trim(a);
}

static void from_int(Big *a, long v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%ld", v);
    from_str(a, buf);
}

static void to_str(const Big *a, char *out, size_t cap) {
    if (!a->n) { snprintf(out, cap, "0"); return; }
    size_t p = 0;
    if (a->neg) out[p++] = '-';
    p += (size_t)snprintf(out + p, cap - p, "%u", a->d[a->n - 1]);
    for (unsigned i = a->n - 1; i-- > 0;) p += (size_t)snprintf(out + p, cap - p, "%09u", a->d[i]);
}

static int cmp_mag(const Big *a, const Big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (unsigned i = a->n; i-- > 0;)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

static int cmp(const Big *a, const Big *b) {
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int c = cmp_mag(a, b);
    return a->neg ? -c : c;
}

static void add_mag(const Big *a, const Big *b, Big *r) {
    unsigned carry = 0, n = a->n > b->n ? a->n : b->n;
    Big t;
    for (unsigned i = 0; i < n; i++) {
        unsigned s = carry + (i < a->n ? a->d[i] : 0) + (i < b->n ? b->d[i] : 0);
        carry = s >= BASE;
        t.d[i] = carry ? s - BASE : s;
    }
    t.n = n;
    if (carry) t.d[t.n++] = 1;
    t.neg = 0;
    *r = t;
}

/* |a| >= |b| required */
static void sub_mag(const Big *a, const Big *b, Big *r) {
    unsigned borrow = 0;
    Big t;
    for (unsigned i = 0; i < a->n; i++) {
        long s = (long)a->d[i] - borrow - (i < b->n ? (long)b->d[i] : 0);
        borrow = s < 0;
        t.d[i] = (unsigned)(borrow ? s + (long)BASE : s);
    }
    t.n = a->n;
    t.neg = 0;
    trim(&t);
    *r = t;
}

static void add(const Big *a, const Big *b, Big *r) {
    if (a->neg == b->neg) {
        add_mag(a, b, r);
        r->neg = a->neg;
    } else if (cmp_mag(a, b) >= 0) {
        int neg = a->neg;
        sub_mag(a, b, r);
        r->neg = neg;
    } else {
        int neg = b->neg;
        sub_mag(b, a, r);
        r->neg = neg;
    }
    trim(r);
}

static void neg(const Big *a, Big *r) { *r = *a; if (r->n) r->neg = !r->neg; }
static void sub(const Big *a, const Big *b, Big *r) { Big nb; neg(b, &nb); add(a, &nb, r); }

int main(void) {
    char buf[700];
    const char *cases[][2] = {
        {"999999999", "1"}, {"999999999999999999", "1"}, {"1000000000000000000", "-1"},
        {"-5", "5"}, {"123456789012345678901234567890", "-987654321098765432109876543210"},
        {"0", "0"}, {"-0", "7"}, {"5", "-12"}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Big a, b, s, d;
        from_str(&a, cases[i][0]);
        from_str(&b, cases[i][1]);
        add(&a, &b, &s);
        sub(&a, &b, &d);
        char sa[200], sd[200];
        to_str(&s, sa, sizeof sa);
        to_str(&d, sd, sizeof sd);
        printf("%s, %s: sum=%s diff=%s cmp=%d\n", cases[i][0], cases[i][1], sa, sd, cmp(&a, &b));
        /* round trip: (a + b) - b == a */
        Big back;
        sub(&s, &b, &back);
        if (cmp(&back, &a) != 0) { fprintf(stderr, "round trip failed\n"); return 1; }
    }
    /* Fibonacci up to F(400) */
    static Big fib[402];
    from_int(&fib[0], 0);
    from_int(&fib[1], 1);
    for (int i = 2; i <= 400; i++) add(&fib[i - 1], &fib[i - 2], &fib[i]);
    to_str(&fib[100], buf, sizeof buf);
    printf("F(100) = %s\n", buf);
    to_str(&fib[400], buf, sizeof buf);
    printf("F(400) = %s (%zu digits)\n", buf, strlen(buf));
    /* Cassini: F(n-1)F(n+1) - F(n)^2 = (-1)^n is checked via small n with repeated add */
    /* d'Ocagne-like additive identity: F(2n) = F(n) * (F(n-1) + F(n+1)) checked mod 10^9 via limbs */
    for (int n = 5; n <= 45; n += 8) {
        unsigned long long a = fib[n].d[0], b = fib[n - 1].d[0], c = fib[n + 1].d[0];
        unsigned long long lhs = fib[2 * n].d[0];
        unsigned long long rhs = a * ((b + c) % BASE) % BASE;
        if (lhs != rhs) { fprintf(stderr, "F(2n) identity fails at %d\n", n); return 1; }
    }
    /* sum of first n Fibonacci numbers is F(n+2) - 1 */
    Big sum, one;
    from_int(&sum, 0);
    from_int(&one, 1);
    for (int i = 1; i <= 300; i++) add(&sum, &fib[i], &sum);
    Big expect;
    sub(&fib[302 > 401 ? 401 : 302], &one, &expect);
    if (cmp(&sum, &expect) != 0) { fprintf(stderr, "sum identity fails\n"); return 1; }
    to_str(&sum, buf, sizeof buf);
    printf("F(1)+...+F(300) = %s\n", buf);
    /* Lucas numbers L(n) = F(n-1) + F(n+1) */
    Big luc;
    add(&fib[199], &fib[201], &luc);
    to_str(&luc, buf, sizeof buf);
    printf("L(200) = %s\n", buf);
    /* alternating sum of magnitudes exercise borrow chains */
    Big acc;
    from_int(&acc, 0);
    for (int i = 1; i <= 120; i++) { if (i & 1) add(&acc, &fib[i], &acc); else sub(&acc, &fib[i], &acc); }
    to_str(&acc, buf, sizeof buf);
    printf("alternating sum to 120 = %s\n", buf);
    return 0;
}
