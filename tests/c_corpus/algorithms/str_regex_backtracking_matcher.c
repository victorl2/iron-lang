/*
 * title: Backtracking regex engine with AST
 * topic: algorithms
 * covers: regex parser, alternation, greedy quantifiers, continuation-passing backtracking, empty-loop guard, set-based reference
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 0x9e3779b97f4a7c15ULL;

static inline unsigned rnd(void) {
    unsigned long long z = (rng_s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return (unsigned)((z ^ (z >> 31)) >> 16);
}

static inline void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static inline void rand_str(char *s, int n, int alpha) {
    for (int i = 0; i < n; i++)
        s[i] = (char)('a' + rnd() % (unsigned)alpha);
    s[n] = 0;
}

/* Backtracking regex matcher (Pike/Kernighan style extended with groups,
 * alternation, + ? *, character classes and anchors). The pattern is parsed
 * into an AST once, then matched with continuation-passing recursion. */
typedef enum { N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_STAR, N_PLUS, N_OPT, N_EMPTY, N_END, N_ACCEPT } Kind;

typedef struct Node Node;
struct Node {
    Kind kind;
    unsigned char ch;
    unsigned char set[32]; /* bitmap for N_CLASS */
    Node *a, *b;
};

static Node pool[256];
static int npool;
static const char *pp;
static int parse_err;

static Node *mk(Kind k, Node *a, Node *b) {
    Node *n = &pool[npool++];
    memset(n, 0, sizeof *n);
    n->kind = k;
    n->a = a;
    n->b = b;
    return n;
}

static Node *parse_alt(void);

static Node *parse_atom(void) {
    if (*pp == '(') {
        pp++;
        Node *n = parse_alt();
        if (*pp != ')')
            parse_err = 1;
        else
            pp++;
        return n;
    }
    if (*pp == '.') {
        pp++;
        return mk(N_ANY, NULL, NULL);
    }
    if (*pp == '[') {
        Node *n = mk(N_CLASS, NULL, NULL);
        int neg = 0;
        pp++;
        if (*pp == '^') {
            neg = 1;
            pp++;
        }
        while (*pp && *pp != ']') {
            unsigned char lo = (unsigned char)*pp++, hi = lo;
            if (*pp == '-' && pp[1] && pp[1] != ']') {
                hi = (unsigned char)pp[1];
                pp += 2;
            }
            for (int c = lo; c <= hi; c++)
                n->set[c >> 3] |= (unsigned char)(1u << (c & 7));
        }
        if (*pp != ']')
            parse_err = 1;
        else
            pp++;
        if (neg)
            for (int i = 0; i < 32; i++)
                n->set[i] = (unsigned char)~n->set[i];
        return n;
    }
    if (*pp == '\\' && pp[1]) {
        Node *n = mk(N_CHAR, NULL, NULL);
        n->ch = (unsigned char)pp[1];
        pp += 2;
        return n;
    }
    if (*pp == '*' || *pp == '+' || *pp == '?') {
        parse_err = 1;
        pp++;
        return mk(N_EMPTY, NULL, NULL);
    }
    Node *n = mk(N_CHAR, NULL, NULL);
    n->ch = (unsigned char)*pp++;
    return n;
}

static Node *parse_rep(void) {
    Node *n = parse_atom();
    while (*pp == '*' || *pp == '+' || *pp == '?') {
        n = mk(*pp == '*' ? N_STAR : *pp == '+' ? N_PLUS : N_OPT, n, NULL);
        pp++;
    }
    return n;
}

static Node *parse_cat(void) {
    Node *n = mk(N_EMPTY, NULL, NULL);
    while (*pp && *pp != '|' && *pp != ')')
        n = mk(N_CAT, n, parse_rep());
    return n;
}

static Node *parse_alt(void) {
    Node *n = parse_cat();
    while (*pp == '|') {
        pp++;
        n = mk(N_ALT, n, parse_cat());
    }
    return n;
}

typedef struct Cont Cont;
struct Cont {
    Node *node;
    const Cont *next;
    const char *guard; /* star iterations must consume input */
};

static long steps;
static const char *accept_end;

static int m(Node *n, const char *s, const char *end, const Cont *k);

static int run_cont(const char *s, const char *end, const Cont *k) {
    if (k->guard && s == k->guard)
        return 0;
    return m(k->node, s, end, k->next);
}

static int m(Node *n, const char *s, const char *end, const Cont *k) {
    steps++;
    switch (n->kind) {
    case N_EMPTY:
        return run_cont(s, end, k);
    case N_END:
        return s == end;
    case N_ACCEPT:
        accept_end = s;
        return 1;
    case N_CHAR:
        return s < end && (unsigned char)*s == n->ch && run_cont(s + 1, end, k);
    case N_ANY:
        return s < end && run_cont(s + 1, end, k);
    case N_CLASS:
        return s < end && ((n->set[(unsigned char)*s >> 3] >> (*s & 7)) & 1) &&
               run_cont(s + 1, end, k);
    case N_CAT: {
        Cont c = {n->b, k, NULL};
        return m(n->a, s, end, &c);
    }
    case N_ALT:
        return m(n->a, s, end, k) || m(n->b, s, end, k);
    case N_OPT:
        return m(n->a, s, end, k) || run_cont(s, end, k);
    case N_PLUS: {
        Node star;
        memset(&star, 0, sizeof star);
        star.kind = N_STAR;
        star.a = n->a;
        Cont c = {&star, k, NULL};
        return m(n->a, s, end, &c);
    }
    case N_STAR: {
        Cont again = {n, k, s};
        if (m(n->a, s, end, &again))
            return 1;
        return run_cont(s, end, k);
    }
    }
    return 0;
}

/* Independent reference: set-of-end-positions semantics with bitmasks. */
typedef unsigned long long u64;

static u64 ends(Node *n, const char *t, int len, u64 from) {
    u64 out = 0;
    switch (n->kind) {
    case N_EMPTY:
        return from;
    case N_END:
    case N_ACCEPT:
        return from;
    case N_CHAR:
    case N_ANY:
    case N_CLASS:
        for (int i = 0; i < len; i++)
            if (from >> i & 1) {
                int ok = n->kind == N_ANY ||
                         (n->kind == N_CHAR && (unsigned char)t[i] == n->ch) ||
                         (n->kind == N_CLASS && ((n->set[(unsigned char)t[i] >> 3] >> (t[i] & 7)) & 1));
                if (ok)
                    out |= 1ULL << (i + 1);
            }
        return out;
    case N_CAT:
        return ends(n->b, t, len, ends(n->a, t, len, from));
    case N_ALT:
        return ends(n->a, t, len, from) | ends(n->b, t, len, from);
    case N_OPT:
        return from | ends(n->a, t, len, from);
    case N_PLUS:
    case N_STAR: {
        u64 cur = n->kind == N_STAR ? from : ends(n->a, t, len, from);
        u64 all = cur;
        for (;;) {
            u64 nx = ends(n->a, t, len, all) | all;
            if (nx == all)
                break;
            all = nx;
        }
        return n->kind == N_STAR ? all | from : all;
    }
    }
    return out;
}

typedef struct {
    Node *root;
    int ok;
} Regex;

static Regex compile(const char *pat) {
    npool = 0;
    pp = pat;
    parse_err = 0;
    Regex r;
    r.root = parse_alt();
    r.ok = !parse_err && *pp == 0;
    return r;
}

static int full_match(Node *root, const char *s) {
    Node end = {N_END, 0, {0}, NULL, NULL};
    Cont c = {&end, NULL, NULL};
    return m(root, s, s + strlen(s), &c);
}

/* leftmost match; returns start or -1, length in *mlen */
static int search(Node *root, const char *s, int *mlen) {
    Node acc = {N_ACCEPT, 0, {0}, NULL, NULL};
    Cont c = {&acc, NULL, NULL};
    int n = (int)strlen(s);
    for (int i = 0; i <= n; i++)
        if (m(root, s + i, s + n, &c)) {
            *mlen = (int)(accept_end - (s + i));
            return i;
        }
    return -1;
}

int main(void) {
    static const struct {
        const char *re, *text;
        int want;
    } cases[] = {
        {"abc", "abc", 1},          {"a.c", "axc", 1},         {"a*b", "aaab", 1},
        {"a*b", "aaa", 0},          {"(ab)+", "ababab", 1},    {"(ab)+", "aba", 0},
        {"colou?r", "color", 1},    {"colou?r", "colour", 1},  {"cat|dog", "dog", 1},
        {"cat|dog", "cow", 0},      {"[a-c]+x", "abcabx", 1},  {"[^a-c]+", "xyz", 1},
        {"[^a-c]+", "xaz", 0},      {"(a|b)*abb", "babaabb", 1}, {"(a*)*b", "aaaaaaaaaab", 1},
        {"(a*)*b", "aaaaaaaaaac", 0}, {"\\.", ".", 1},
        {"(a|ab)(c|bcd)", "abcd", 1}, {"", "", 1},              {"x*", "", 1},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Regex r = compile(cases[i].re);
        check(r.ok, "compile");
        steps = 0;
        int got = full_match(r.root, cases[i].text);
        int len = (int)strlen(cases[i].text);
        int ref = (int)(ends(r.root, cases[i].text, len, 1ULL) >> len & 1);
        check(got == cases[i].want && ref == got, "known case");
        printf("%-14s %-12s %-5s steps=%ld\n", cases[i].re, cases[i].text, got ? "yes" : "no", steps);
    }
    const char *bad[] = {"(ab", "*a", "[abc", "a)"};
    for (int i = 0; i < 4; i++) {
        Regex r = compile(bad[i]);
        check(!r.ok, "reject malformed");
        printf("malformed %-5s rejected\n", bad[i]);
    }
    /* search: leftmost, greedy */
    Regex r = compile("[0-9]+(\\.[0-9]+)?");
    const char *texts[] = {"price 12.50 usd", "no digits", "v3 and 4.25.1"};
    for (int i = 0; i < 3; i++) {
        int len = 0;
        int at = search(r.root, texts[i], &len);
        if (at < 0)
            printf("search \"%s\": none\n", texts[i]);
        else
            printf("search \"%s\": at %d \"%.*s\"\n", texts[i], at, len, texts[i] + at);
    }
    /* fuzz: random regexes from a tiny grammar vs the position-set reference */
    int agree = 0, yes = 0;
    for (int t = 0; t < 3000; t++) {
        char re[40];
        int k = 0, depth = 0;
        int pieces = 1 + (int)(rnd() % 5);
        for (int i = 0; i < pieces && k < 30; i++) {
            unsigned x = rnd() % 9;
            if (x < 3)
                re[k++] = (char)('a' + x % 2);
            else if (x == 3)
                re[k++] = '.';
            else if (x == 4 && k > 0 && re[k - 1] != '(' && re[k - 1] != '|')
                re[k++] = '*';
            else if (x == 5 && k > 0 && re[k - 1] != '(' && re[k - 1] != '|')
                re[k++] = '+';
            else if (x == 6) {
                re[k++] = '(';
                depth++;
            } else if (x == 7 && depth > 0 && re[k - 1] != '(' && re[k - 1] != '|') {
                re[k++] = ')';
                depth--;
            } else if (x == 8 && k > 0 && re[k - 1] != '(' && re[k - 1] != '|') {
                re[k++] = '|';
            } else
                re[k++] = 'b';
        }
        while (depth-- > 0)
            re[k++] = ')';
        re[k] = 0;
        Regex q = compile(re);
        if (!q.ok)
            continue;
        char tx[16];
        int len = (int)(rnd() % 10);
        rand_str(tx, len, 2);
        int got = full_match(q.root, tx);
        int ref = (int)(ends(q.root, tx, len, 1ULL) >> len & 1);
        check(got == ref, "fuzz regex vs reference");
        agree++;
        yes += got;
    }
    printf("fuzz: %d compared, %d matched\n", agree, yes);
    return 0;
}
