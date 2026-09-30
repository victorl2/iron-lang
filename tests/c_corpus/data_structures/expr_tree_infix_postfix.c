/*
 * title: Expression tree from infix and postfix
 * topic: data_structures
 * covers: expression tree, shunting-yard, recursive descent, postfix build, evaluation, traversals
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Node {
    char op; /* 0 for leaf */
    long val;
    struct Node *l, *r;
} Node;

static unsigned long long rs = 0x9E3779B97F4A7C15ULL;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 32);
}
static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static Node *mk(char op, long v, Node *l, Node *r) {
    Node *n = malloc(sizeof *n);
    n->op = op; n->val = v; n->l = l; n->r = r;
    return n;
}
static void destroy(Node *n) {
    if (!n) return;
    destroy(n->l); destroy(n->r); free(n);
}
static int prec(char c) { return (c == '+' || c == '-') ? 1 : 2; }

/* returns 0 on division by zero */
static int eval(const Node *n, long *out) {
    if (!n->op) { *out = n->val; return 1; }
    long a, b;
    if (!eval(n->l, &a) || !eval(n->r, &b)) return 0;
    switch (n->op) {
    case '+': *out = a + b; return 1;
    case '-': *out = a - b; return 1;
    case '*': *out = a * b; return 1;
    default:
        if (b == 0) return 0;
        *out = a / b; /* operands are non-negative or trunc consistent */
        return 1;
    }
}
static int same(const Node *a, const Node *b) {
    if (!a || !b) return a == b;
    return a->op == b->op && a->val == b->val && same(a->l, b->l) && same(a->r, b->r);
}

/* output writers */
static void full(const Node *n, char *o) {
    if (!n->op) { char t[24]; snprintf(t, sizeof t, "%ld", n->val); strcat(o, t); return; }
    strcat(o, "("); full(n->l, o);
    size_t k = strlen(o); o[k] = n->op; o[k + 1] = 0;
    full(n->r, o); strcat(o, ")");
}
static void post(const Node *n, char *o) {
    if (!n->op) { char t[24]; snprintf(t, sizeof t, "%ld ", n->val); strcat(o, t); return; }
    post(n->l, o); post(n->r, o);
    size_t k = strlen(o); o[k] = n->op; o[k + 1] = ' '; o[k + 2] = 0;
}
static void pre(const Node *n, char *o) {
    if (!n->op) { char t[24]; snprintf(t, sizeof t, "%ld ", n->val); strcat(o, t); return; }
    size_t k = strlen(o); o[k] = n->op; o[k + 1] = ' '; o[k + 2] = 0;
    pre(n->l, o); pre(n->r, o);
}
/* minimal-parenthesis infix */
static void mini(const Node *n, char *o, int parent, int right) {
    if (!n->op) { char t[24]; snprintf(t, sizeof t, "%ld", n->val); strcat(o, t); return; }
    int p = prec(n->op);
    int paren = p < parent || (right && p == parent);
    if (paren) strcat(o, "(");
    mini(n->l, o, p, 0);
    size_t k = strlen(o); o[k] = n->op; o[k + 1] = 0;
    mini(n->r, o, p, 1);
    if (paren) strcat(o, ")");
}

/* postfix string -> tree using explicit stack */
static Node *from_postfix(const char *s) {
    Node *st[64]; int sp = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (*s >= '0' && *s <= '9') {
            long v = 0;
            while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
            st[sp++] = mk(0, v, NULL, NULL);
        } else {
            Node *r = st[--sp], *l = st[--sp];
            st[sp++] = mk(*s++, 0, l, r);
        }
    }
    check(sp == 1, "postfix leaves one tree");
    return st[0];
}
/* infix -> postfix by shunting-yard (all operators left associative) */
static void to_postfix(const char *s, char *out) {
    char ops[64]; int sp = 0; out[0] = 0;
    while (*s) {
        if (*s == ' ') { s++; continue; }
        if (*s >= '0' && *s <= '9') {
            while (*s >= '0' && *s <= '9') { size_t k = strlen(out); out[k] = *s++; out[k + 1] = 0; }
            strcat(out, " ");
        } else if (*s == '(') { ops[sp++] = *s++; }
        else if (*s == ')') {
            while (sp && ops[sp - 1] != '(') { size_t k = strlen(out); out[k] = ops[--sp]; out[k + 1] = ' '; out[k + 2] = 0; }
            check(sp > 0, "balanced parens");
            sp--; s++;
        } else {
            while (sp && ops[sp - 1] != '(' && prec(ops[sp - 1]) >= prec(*s)) {
                size_t k = strlen(out); out[k] = ops[--sp]; out[k + 1] = ' '; out[k + 2] = 0;
            }
            ops[sp++] = *s++;
        }
    }
    while (sp) { check(ops[sp - 1] != '(', "balanced"); size_t k = strlen(out); out[k] = ops[--sp]; out[k + 1] = ' '; out[k + 2] = 0; }
}
/* recursive-descent parser */
static const char *ip;
static Node *pexpr(void);
static void skip(void) { while (*ip == ' ') ip++; }
static Node *pfactor(void) {
    skip();
    if (*ip == '(') { ip++; Node *n = pexpr(); skip(); check(*ip == ')', "rparen"); ip++; return n; }
    long v = 0;
    check(*ip >= '0' && *ip <= '9', "digit");
    while (*ip >= '0' && *ip <= '9') v = v * 10 + (*ip++ - '0');
    return mk(0, v, NULL, NULL);
}
static Node *pterm(void) {
    Node *n = pfactor();
    for (;;) { skip(); if (*ip != '*' && *ip != '/') return n; char o = *ip++; n = mk(o, 0, n, pfactor()); }
}
static Node *pexpr(void) {
    Node *n = pterm();
    for (;;) { skip(); if (*ip != '+' && *ip != '-') return n; char o = *ip++; n = mk(o, 0, n, pterm()); }
}

static Node *randtree(int depth) {
    if (depth == 0 || rnd() % 5 == 0) return mk(0, 1 + rnd() % 6, NULL, NULL);
    const char ops[] = "+-*/";
    char o = ops[rnd() % 4];
    Node *l = randtree(depth - 1);
    Node *r = randtree(depth - 1);
    return mk(o, 0, l, r);
}
static int height(const Node *n) { return n ? 1 + (height(n->l) > height(n->r) ? height(n->l) : height(n->r)) : 0; }
static int count(const Node *n) { return n ? 1 + count(n->l) + count(n->r) : 0; }

int main(void) {
    const char *fixed[] = { "1 + 2 * 3", "(1 + 2) * 3", "10 - 4 - 3", "100 / 5 / 2 + 7 * (8 - 3)", "2 * (3 + (4 - 1)) * 5" };
    long expect[] = { 7, 9, 3, 45, 60 };
    for (int i = 0; i < 5; i++) {
        char pf[256], f[256] = "", p2[256] = "", pr[256] = "";
        to_postfix(fixed[i], pf);
        Node *a = from_postfix(pf);
        ip = fixed[i];
        Node *b = pexpr();
        check(same(a, b), "shunting-yard and descent agree");
        long v;
        check(eval(a, &v) && v == expect[i], "known value");
        full(a, f); post(a, p2); pre(a, pr);
        printf("expr %d: %s = %ld\n  full  %s\n  post  %s\n  pre   %s\n", i, fixed[i], v, f, p2, pr);
        destroy(a); destroy(b);
    }
    int tested = 0, zero = 0, nodes = 0, maxh = 0; long sum = 0;
    for (int it = 0; it < 400; it++) {
        Node *t = randtree(4);
        long want;
        int ok = eval(t, &want);
        char s[1024] = "", pf[1024] = "";
        mini(t, s, 0, 0);
        to_postfix(s, pf);
        Node *a = from_postfix(pf);
        ip = s;
        Node *b = pexpr();
        check(same(a, b), "parsed shape agree");
        long v1, v2;
        int ok2 = eval(a, &v1), ok3 = eval(b, &v2);
        check(ok == ok2 && ok == ok3, "div-by-zero agreement");
        if (ok) { check(v1 == want && v2 == want, "value matches"); sum += want; }
        else zero++;
        /* postfix printer round-trips */
        char pp[1024] = "";
        post(t, pp);
        Node *c = from_postfix(pp);
        check(same(c, t), "postfix round trip");
        check(count(t) == count(a), "node count");
        nodes += count(t);
        if (height(t) > maxh) maxh = height(t);
        tested++;
        destroy(t); destroy(a); destroy(b); destroy(c);
    }
    printf("random trees %d, div-by-zero %d, nodes %d, max height %d, value sum %ld\n", tested, zero, nodes, maxh, sum);
    return 0;
}
