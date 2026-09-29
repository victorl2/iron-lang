/*
 * title: Sparse polynomials as sorted linked lists
 * topic: data_structures
 * covers: sparse polynomial, sorted term list, add, multiply, evaluate, derivative, dense reference check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static unsigned long long rs = 0x923F82A4AF194F9BULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }

typedef struct Term { int exp; long long coef; struct Term *next; } Term; /* descending exponent, no zero coefficients */

static Term *term(int exp, long long coef, Term *next) { Term *t = malloc(sizeof *t); CHECK(t); t->exp = exp; t->coef = coef; t->next = next; return t; }
static void free_poly(Term *p) { while (p) { Term *n = p->next; free(p); p = n; } }

/* add term c*x^e into p, keeping order and dropping zeros */
static Term *add_term(Term *p, int e, long long c) {
    if (c == 0) return p;
    Term **pp = &p;
    while (*pp && (*pp)->exp > e) pp = &(*pp)->next;
    if (*pp && (*pp)->exp == e) {
        (*pp)->coef += c;
        if ((*pp)->coef == 0) { Term *d = *pp; *pp = d->next; free(d); }
    } else *pp = term(e, c, *pp);
    return p;
}
static Term *poly_add(const Term *a, const Term *b, int sign) {
    Term *r = NULL, **pp = &r;
    while (a || b) {
        int e; long long c;
        if (a && (!b || a->exp > b->exp)) { e = a->exp; c = a->coef; a = a->next; }
        else if (b && (!a || b->exp > a->exp)) { e = b->exp; c = sign * b->coef; b = b->next; }
        else { e = a->exp; c = a->coef + sign * b->coef; a = a->next; b = b->next; }
        if (c) { *pp = term(e, c, NULL); pp = &(*pp)->next; }
    }
    return r;
}
static Term *poly_mul(const Term *a, const Term *b) {
    Term *r = NULL;
    for (const Term *x = a; x; x = x->next) for (const Term *y = b; y; y = y->next) r = add_term(r, x->exp + y->exp, x->coef * y->coef);
    return r;
}
static Term *poly_derive(const Term *a) {
    Term *r = NULL, **pp = &r;
    for (; a; a = a->next) if (a->exp > 0) { *pp = term(a->exp - 1, a->coef * a->exp, NULL); pp = &(*pp)->next; }
    return r;
}
static long long poly_eval(const Term *a, long long x) { /* Horner across gaps in the exponents */
    long long r = 0; int prev = a ? a->exp : 0;
    for (; a; a = a->next) {
        for (int i = prev; i > a->exp; i--) r *= x;
        r += a->coef;
        prev = a->exp;
    }
    for (int i = prev; i > 0; i--) r *= x;
    return r;
}
static int terms(const Term *p) { int n = 0; for (; p; p = p->next) n++; return n; }

#define D 40
static void to_dense(const Term *p, long long *d) { memset(d, 0, D * sizeof *d); for (; p; p = p->next) { CHECK(p->exp < D); d[p->exp] = p->coef; } }
static Term *random_poly(int nterms, int maxexp) {
    Term *p = NULL;
    for (int i = 0; i < nterms; i++) p = add_term(p, (int)(rnd() % (unsigned)(maxexp + 1)), (long long)(rnd() % 21) - 10);
    return p;
}
static void check_canonical(const Term *p) { for (; p; p = p->next) { CHECK(p->coef != 0); if (p->next) CHECK(p->exp > p->next->exp); } }
static long long dense_eval(const long long *d, long long x) { long long r = 0; for (int i = D - 1; i >= 0; i--) r = r * x + d[i]; return r; }

int main(void) {
    long checks = 0, term_total = 0;
    for (int t = 0; t < 300; t++) {
        Term *a = random_poly(1 + (int)(rnd() % 8), 18), *b = random_poly(1 + (int)(rnd() % 8), 18);
        long long da[D], db[D], dr[D], dd[D];
        to_dense(a, da); to_dense(b, db);
        Term *s = poly_add(a, b, 1), *df = poly_add(a, b, -1), *m = poly_mul(a, b), *dv = poly_derive(a);
        check_canonical(s); check_canonical(df); check_canonical(m); check_canonical(dv);
        to_dense(s, dr); for (int i = 0; i < D; i++) CHECK(dr[i] == da[i] + db[i]);
        to_dense(df, dr); for (int i = 0; i < D; i++) CHECK(dr[i] == da[i] - db[i]);
        to_dense(m, dr);
        for (int i = 0; i < D; i++) { long long acc = 0; for (int j = 0; j <= i; j++) acc += da[j] * db[i - j]; CHECK(dr[i] == acc); }
        to_dense(dv, dd); for (int i = 0; i < D - 1; i++) CHECK(dd[i] == da[i + 1] * (i + 1));
        for (long long x = -2; x <= 2; x++) {
            CHECK(poly_eval(a, x) == dense_eval(da, x));
            CHECK(poly_eval(m, x) == poly_eval(a, x) * poly_eval(b, x));
        }
        checks += 4 + 10; term_total += terms(m);
        free_poly(a); free_poly(b); free_poly(s); free_poly(df); free_poly(m); free_poly(dv);
    }
    printf("checks=%ld product_terms=%ld\n", checks, term_total);
    /* (x+1)^5 by repeated multiplication */
    Term *base = add_term(add_term(NULL, 1, 1), 0, 1), *pw = add_term(NULL, 0, 1);
    for (int i = 0; i < 5; i++) { Term *n = poly_mul(pw, base); free_poly(pw); pw = n; }
    printf("(x+1)^5 =");
    for (Term *p = pw; p; p = p->next) printf(" %+lldx^%d", p->coef, p->exp);
    printf("\n");
    printf("(x+1)^5 at x=3 = %lld\n", poly_eval(pw, 3));
    Term *sq = poly_add(pw, pw, -1); printf("p - p has %d terms\n", terms(sq));
    free_poly(sq); free_poly(base); free_poly(pw);
    return 0;
}
