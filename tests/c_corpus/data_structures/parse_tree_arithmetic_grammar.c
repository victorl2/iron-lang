/*
 * title: Concrete parse tree for an arithmetic grammar
 * topic: data_structures
 * covers: parse tree, recursive descent, grammar with unary minus and right-associative power, function calls, pratt parser cross-check, error positions
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * expr   := term (('+'|'-') term)*
 * term   := unary (('*'|'/'|'%') unary)*
 * unary  := '-' unary | power
 * power  := atom ('^' unary)?           (right associative, binds tighter than unary minus on its left)
 * atom   := NUMBER | IDENT | IDENT '(' args ')' | '(' expr ')'
 * args   := expr (',' expr)*
 */
typedef enum { N_EXPR, N_TERM, N_UNARY, N_POWER, N_ATOM, N_ARGS, N_TOKEN } Kind;
typedef struct PN {
    Kind kind;
    char text[16];
    int nk;
    struct PN *k[10];
} PN;

typedef enum { E_NONE, E_UNEXPECTED_TOKEN, E_UNEXPECTED_END, E_MISSING_RPAREN, E_BAD_CHAR, E_DIV_ZERO, E_UNKNOWN_FUNC, E_UNKNOWN_VAR, E_ARITY } Err;
static const char *errname[] = { "ok", "unexpected-token", "unexpected-end", "missing-rparen", "bad-character", "division-by-zero", "unknown-function", "unknown-variable", "wrong-arity" };

typedef struct { char t[16]; int pos; int isnum; } Tok;
static Tok toks[128];
static int ntok, cur;
static Err perr; static int perrpos;

static unsigned long long rs = 0x9A25E0FULL * 3067;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

static Err lex(const char *s) {
    ntok = 0;
    for (int i = 0; s[i];) {
        if (s[i] == ' ') { i++; continue; }
        Tok *t = &toks[ntok]; t->pos = i; t->isnum = 0;
        if (isdigit((unsigned char)s[i])) { int j = 0; while (isdigit((unsigned char)s[i])) t->t[j++] = s[i++]; t->t[j] = 0; t->isnum = 1; }
        else if (isalpha((unsigned char)s[i])) { int j = 0; while (isalnum((unsigned char)s[i])) t->t[j++] = s[i++]; t->t[j] = 0; }
        else if (strchr("+-*/%^(),", s[i])) { t->t[0] = s[i++]; t->t[1] = 0; }
        else { perrpos = i; return E_BAD_CHAR; }
        ntok++;
    }
    return E_NONE;
}
static int is(const char *t) { return cur < ntok && strcmp(toks[cur].t, t) == 0; }
static PN *mk(Kind k) { PN *n = calloc(1, sizeof *n); n->kind = k; return n; }
static void add(PN *p, PN *c) { p->k[p->nk++] = c; }
static void destroy(PN *n) { if (!n) return; for (int i = 0; i < n->nk; i++) destroy(n->k[i]); free(n); }
static PN *tokn(void) { PN *n = mk(N_TOKEN); strcpy(n->text, toks[cur].t); cur++; return n; }
static void fail(Err e) { if (!perr) { perr = e; perrpos = cur < ntok ? toks[cur].pos : -1; } }

static PN *p_expr(void);
static PN *p_unary(void);
static PN *p_atom(void) {
    PN *n = mk(N_ATOM);
    if (cur >= ntok) { fail(E_UNEXPECTED_END); return n; }
    if (toks[cur].isnum) { add(n, tokn()); return n; }
    if (isalpha((unsigned char)toks[cur].t[0])) {
        add(n, tokn());
        if (is("(")) {
            add(n, tokn());
            PN *a = mk(N_ARGS);
            add(a, p_expr());
            while (!perr && is(",")) { add(a, tokn()); add(a, p_expr()); }
            add(n, a);
            if (is(")")) add(n, tokn()); else fail(cur < ntok ? E_UNEXPECTED_TOKEN : E_MISSING_RPAREN);
        }
        return n;
    }
    if (is("(")) {
        add(n, tokn()); add(n, p_expr());
        if (is(")")) add(n, tokn()); else fail(cur < ntok ? E_UNEXPECTED_TOKEN : E_MISSING_RPAREN);
        return n;
    }
    fail(E_UNEXPECTED_TOKEN);
    return n;
}
static PN *p_power(void) {
    PN *n = mk(N_POWER);
    add(n, p_atom());
    if (!perr && is("^")) { add(n, tokn()); add(n, p_unary()); }
    return n;
}
static PN *p_unary(void) {
    PN *n = mk(N_UNARY);
    if (is("-")) { add(n, tokn()); add(n, p_unary()); } else add(n, p_power());
    return n;
}
static PN *p_term(void) {
    PN *n = mk(N_TERM);
    add(n, p_unary());
    while (!perr && (is("*") || is("/") || is("%"))) { add(n, tokn()); add(n, p_unary()); }
    return n;
}
static PN *p_expr(void) {
    PN *n = mk(N_EXPR);
    add(n, p_term());
    while (!perr && (is("+") || is("-"))) { add(n, tokn()); add(n, p_term()); }
    return n;
}

/* ---- evaluation of the concrete tree ---- */
static long var_x = 7, var_y = -3;
static long ipow(long b, long e, Err *err) {
    if (e < 0) { *err = E_DIV_ZERO; return 0; }
    long r = 1;
    for (long i = 0; i < e && i < 40; i++) { r *= b; if (r > 1000000000L) r %= 1000003; if (r < -1000000000L) r = -((-r) % 1000003); }
    return r;
}
static long ev(const PN *n, Err *err);
static long ev_bin(const PN *n, Err *err) { /* left-assoc chain: child0 (op child)* */
    long v = ev(n->k[0], err);
    for (int i = 1; i + 1 < n->nk && !*err; i += 2) {
        long r = ev(n->k[i + 1], err); char op = n->k[i]->text[0];
        if (op == '+') v += r; else if (op == '-') v -= r; else if (op == '*') v *= r;
        else { if (r == 0) { *err = E_DIV_ZERO; return 0; } v = op == '/' ? v / r : v % r; }
    }
    return v;
}
static long ev(const PN *n, Err *err) {
    switch (n->kind) {
    case N_EXPR: case N_TERM: return ev_bin(n, err);
    case N_UNARY: return n->nk == 2 ? -ev(n->k[1], err) : ev(n->k[0], err);
    case N_POWER: { long b = ev(n->k[0], err); if (n->nk == 3) { long e = ev(n->k[2], err); return *err ? 0 : ipow(b, e, err); } return b; }
    case N_ATOM:
        if (n->nk == 1) {
            const char *t = n->k[0]->text;
            if (isdigit((unsigned char)t[0])) return atol(t);
            if (!strcmp(t, "x")) return var_x;
            if (!strcmp(t, "y")) return var_y;
            *err = E_UNKNOWN_VAR; return 0;
        }
        if (n->nk == 3) return ev(n->k[1], err);
        {
            const char *f = n->k[0]->text; const PN *args = n->k[2];
            int na = (args->nk + 1) / 2; long a[8];
            for (int i = 0; i < na; i++) a[i] = ev(args->k[2 * i], err);
            if (*err) return 0;
            if (!strcmp(f, "abs")) { if (na != 1) { *err = E_ARITY; return 0; } return a[0] < 0 ? -a[0] : a[0]; }
            if (!strcmp(f, "min") || !strcmp(f, "max")) {
                if (na < 1) { *err = E_ARITY; return 0; }
                long r = a[0]; for (int i = 1; i < na; i++) r = f[1] == 'i' ? (a[i] < r ? a[i] : r) : (a[i] > r ? a[i] : r);
                return r;
            }
            *err = E_UNKNOWN_FUNC; return 0;
        }
    default: return 0;
    }
}

/* ---- independent Pratt parser evaluating directly ---- */
static int pc; static Err pe;
static long pratt(int minbp);
static long pratt_atom(void) {
    if (pc >= ntok) { pe = pe ? pe : E_UNEXPECTED_END; return 0; }
    const char *t = toks[pc].t;
    if (toks[pc].isnum) { pc++; return atol(t); }
    if (!strcmp(t, "-")) { pc++; return -pratt(9); } /* unary minus: binds looser than ^ (bp 10/9 below) */
    if (!strcmp(t, "(")) { pc++; long v = pratt(0); if (pc < ntok && !strcmp(toks[pc].t, ")")) pc++; else pe = pe ? pe : E_MISSING_RPAREN; return v; }
    if (isalpha((unsigned char)t[0])) {
        pc++;
        if (pc < ntok && !strcmp(toks[pc].t, "(")) {
            pc++; long a[8]; int na = 0;
            a[na++] = pratt(0);
            while (pc < ntok && !strcmp(toks[pc].t, ",")) { pc++; a[na++] = pratt(0); }
            if (pc < ntok && !strcmp(toks[pc].t, ")")) pc++; else pe = pe ? pe : E_MISSING_RPAREN;
            if (!strcmp(t, "abs") && na == 1) return a[0] < 0 ? -a[0] : a[0];
            if (!strcmp(t, "min") || !strcmp(t, "max")) { long r = a[0]; for (int i = 1; i < na; i++) r = t[1] == 'i' ? (a[i] < r ? a[i] : r) : (a[i] > r ? a[i] : r); return r; }
            pe = pe ? pe : (strcmp(t, "abs") ? E_UNKNOWN_FUNC : E_ARITY);
            return 0;
        }
        if (!strcmp(t, "x")) return var_x;
        if (!strcmp(t, "y")) return var_y;
        pe = pe ? pe : E_UNKNOWN_VAR; return 0;
    }
    pe = pe ? pe : E_UNEXPECTED_TOKEN;
    return 0;
}
static long pratt(int minbp) {
    long lhs = pratt_atom();
    while (!pe && pc < ntok) {
        const char *t = toks[pc].t; int lbp, rbp;
        if (!strcmp(t, "+") || !strcmp(t, "-")) { lbp = 1; rbp = 2; }
        else if (!strcmp(t, "*") || !strcmp(t, "/") || !strcmp(t, "%")) { lbp = 3; rbp = 4; }
        else if (!strcmp(t, "^")) { lbp = 11; rbp = 9; } /* right associative; exponent may itself be a unary minus chain */
        else break;
        if (lbp < minbp) break;
        pc++;
        long rhs = pratt(rbp);
        if (pe) return 0;
        char op = t[0];
        if (op == '+') lhs += rhs; else if (op == '-') lhs -= rhs; else if (op == '*') lhs *= rhs;
        else if (op == '/' || op == '%') { if (rhs == 0) { pe = E_DIV_ZERO; return 0; } lhs = op == '/' ? lhs / rhs : lhs % rhs; }
        else { Err e = E_NONE; lhs = ipow(lhs, rhs, &e); if (e) { pe = e; return 0; } }
    }
    return lhs;
}

static void show(const PN *n, char *o) {
    static const char *nm[] = { "expr", "term", "unary", "power", "atom", "args" };
    if (n->kind == N_TOKEN) { strcat(o, n->text); return; }
    /* collapse single-child chains to keep output short */
    if (n->nk == 1 && n->kind != N_ATOM) { show(n->k[0], o); return; }
    strcat(o, nm[n->kind]); strcat(o, "[");
    for (int i = 0; i < n->nk; i++) { if (i) strcat(o, " "); show(n->k[i], o); }
    strcat(o, "]");
}
static int height(const PN *n) { int h = 0; for (int i = 0; i < n->nk; i++) { int c = height(n->k[i]); if (c > h) h = c; } return h + 1; }
static int count(const PN *n) { int c = 1; for (int i = 0; i < n->nk; i++) c += count(n->k[i]); return c; }

static Err run(const char *s, long *val, PN **tree) {
    perr = E_NONE; perrpos = -1; cur = 0;
    Err le = lex(s);
    if (le) { *tree = NULL; return le; }
    PN *t = p_expr();
    if (!perr && cur < ntok) fail(E_UNEXPECTED_TOKEN);
    *tree = t;
    if (perr) return perr;
    Err e = E_NONE; *val = ev(t, &e);
    return e;
}
static void gen(char *o, int depth) {
    static const char *bins[] = { "+", "-", "*", "/", "%", "^" };
    unsigned r = rnd() % 10;
    if (depth == 0 || r < 3) { if (rnd() % 4 == 0) strcat(o, rnd() & 1 ? "x" : "y"); else { char t[8]; snprintf(t, sizeof t, "%u", rnd() % 9); strcat(o, t); } return; }
    if (r < 4) { strcat(o, "-"); gen(o, depth - 1); return; }
    if (r < 5) { strcat(o, "max("); gen(o, depth - 1); strcat(o, ","); gen(o, depth - 1); strcat(o, ")"); return; }
    if (r < 6) { strcat(o, "abs("); gen(o, depth - 1); strcat(o, ")"); return; }
    strcat(o, "("); gen(o, depth - 1);
    const char *op = bins[rnd() % 6];
    strcat(o, op);
    if (op[0] == '^') { char t[8]; snprintf(t, sizeof t, "%u", rnd() % 4); strcat(o, t); } else gen(o, depth - 1);
    strcat(o, ")");
}

int main(void) {
    const char *fixed[] = { "1 + 2 * 3", "-2 ^ 2", "2 ^ 3 ^ 2", "2 ^ -1 + 5", "max(3, x, 4) - abs(y) * 2", "10 / (5 - 5)", "(1 + 2", "3 + * 4", "foo(1)", "abs(1, 2)", "7 $ 2", "z + 1", "17 % 5 - -3", "" };
    for (unsigned i = 0; i < sizeof fixed / sizeof fixed[0]; i++) {
        long v = 0; PN *t = NULL;
        Err e = run(fixed[i], &v, &t);
        if (!e) {
            char buf[512] = ""; show(t, buf);
            pc = 0; pe = E_NONE; long pv = pratt(0); if (!pe && pc < ntok) pe = E_UNEXPECTED_TOKEN;
            check(!pe && pv == v, "pratt parser agrees with tree evaluation");
            printf("%-28s = %ld  (nodes %d, height %d)\n  %s\n", fixed[i], v, count(t), height(t), buf);
        } else {
            printf("%-28s error %s at %d\n", fixed[i], errname[e], e == E_DIV_ZERO || e == E_UNKNOWN_FUNC || e == E_UNKNOWN_VAR || e == E_ARITY ? -1 : perrpos);
        }
        destroy(t);
    }
    /* random well-formed expressions: both parsers must agree, including error class */
    int agree = 0, errs = 0; long sum = 0; int maxh = 0;
    for (int i = 0; i < 1500; i++) {
        char s[400] = ""; gen(s, 4);
        long v = 0; PN *t = NULL;
        Err e = run(s, &v, &t);
        pc = 0; pe = E_NONE; long pv = pratt(0);
        if (!pe && pc < ntok) pe = E_UNEXPECTED_TOKEN;
        check((e == E_NONE) == (pe == E_NONE), "both parsers agree on success");
        if (e == E_NONE) { check(pv == v, "value agreement"); sum += v % 1009; if (height(t) > maxh) maxh = height(t); }
        else { check(e == pe, "same error class"); errs++; }
        agree++;
        destroy(t);
    }
    printf("random expressions %d agreeing, %d evaluation errors, value checksum %ld, max tree height %d\n", agree, errs, sum, maxh);
    return 0;
}
