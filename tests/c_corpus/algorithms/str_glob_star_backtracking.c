/*
 * title: Glob matcher with single-star backtracking
 * topic: algorithms
 * covers: wildcard matching, character classes, escapes, greedy star backtrack, recursive reference, fuzzing
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

/* Iterative glob matcher with single-star backtracking: * ? [set] [!set] [a-z] \x */
static int match_class(const char *p, unsigned char c, const char **after) {
    int neg = 0, ok = 0;
    p++; /* skip [ */
    if (*p == '!' || *p == '^') {
        neg = 1;
        p++;
    }
    int first = 1;
    while (*p && (*p != ']' || first)) {
        first = 0;
        unsigned char lo = (unsigned char)*p;
        if (p[1] == '-' && p[2] && p[2] != ']') {
            unsigned char hi = (unsigned char)p[2];
            if (c >= lo && c <= hi)
                ok = 1;
            p += 3;
        } else {
            if (c == lo)
                ok = 1;
            p++;
        }
    }
    if (*p != ']') {
        *after = NULL; /* malformed */
        return 0;
    }
    *after = p + 1;
    return ok != neg;
}

/* returns 1 match, 0 no match, -1 malformed pattern; counts backtracks */
static int glob_match(const char *pat, const char *str, long *backtracks) {
    const char *star_p = NULL, *star_s = NULL;
    while (*str) {
        int advanced = 0;
        if (*pat == '*') {
            star_p = ++pat;
            star_s = str;
            continue;
        }
        if (*pat == '?') {
            pat++;
            str++;
            advanced = 1;
        } else if (*pat == '[') {
            const char *after;
            int r = match_class(pat, (unsigned char)*str, &after);
            if (!after)
                return -1;
            if (r) {
                pat = after;
                str++;
                advanced = 1;
            }
        } else if (*pat == '\\' && pat[1]) {
            if (pat[1] == *str) {
                pat += 2;
                str++;
                advanced = 1;
            }
        } else if (*pat && *pat == *str) {
            pat++;
            str++;
            advanced = 1;
        }
        if (!advanced) {
            if (!star_p)
                return 0;
            (*backtracks)++;
            pat = star_p;
            str = ++star_s;
        }
    }
    while (*pat == '*')
        pat++;
    return *pat == 0;
}

/* Recursive reference implementation. */
static int glob_rec(const char *p, const char *s) {
    if (!*p)
        return !*s;
    if (*p == '*')
        return glob_rec(p + 1, s) || (*s && glob_rec(p, s + 1));
    if (!*s)
        return 0;
    if (*p == '?')
        return glob_rec(p + 1, s + 1);
    if (*p == '[') {
        const char *after;
        int r = match_class(p, (unsigned char)*s, &after);
        return after && r && glob_rec(after, s + 1);
    }
    if (*p == '\\' && p[1])
        return p[1] == *s && glob_rec(p + 2, s + 1);
    return *p == *s && glob_rec(p + 1, s + 1);
}

int main(void) {
    static const struct {
        const char *pat, *str;
        int want;
    } cases[] = {
        {"*.c", "main.c", 1},        {"*.c", "main.h", 0},         {"m??n.c", "main.c", 1},
        {"a*b*c", "aXXbYYc", 1},     {"a*b*c", "aXXbYY", 0},       {"[abc]x", "bx", 1},
        {"[!abc]x", "bx", 0},        {"[a-f]*", "dog", 1},         {"[a-f]*", "zebra", 0},
        {"file[0-9][0-9].txt", "file42.txt", 1},                   {"\\*star", "*star", 1},
        {"\\*star", "xstar", 0},     {"***", "", 1},               {"", "", 1},
        {"", "x", 0},                {"*a", "ba", 1},              {"*a", "ab", 0},
        {"[]]x", "]x", 1},           {"a[-b]c", "a-c", 1},         {"*ab*ab*ab", "abababab", 1},
        {"*x*y*z", "xyzxyz", 1},     {"*x*y*z", "zyx", 0},
    };
    long bt_total = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        long bt = 0;
        int r = glob_match(cases[i].pat, cases[i].str, &bt);
        check(r == cases[i].want, "glob known case");
        check(glob_rec(cases[i].pat, cases[i].str) == cases[i].want, "recursive agrees");
        bt_total += bt;
        printf("%-20s %-10s %s (backtracks %ld)\n", cases[i].pat, cases[i].str,
               r ? "match" : "no match", bt);
    }
    /* Fuzz: random patterns against random strings over a small alphabet. */
    int agree = 0, matched = 0;
    for (int t = 0; t < 4000; t++) {
        char pat[48], str[16];
        int pl = (int)(rnd() % 8), sl = (int)(rnd() % 10);
        int k = 0;
        for (int i = 0; i < pl; i++) {
            switch (rnd() % 6) {
            case 0: pat[k++] = 'a'; break;
            case 1: pat[k++] = 'b'; break;
            case 2: pat[k++] = '*'; break;
            case 3: pat[k++] = '?'; break;
            case 4: memcpy(pat + k, "[ab]", 4); k += 4; break;
            default: memcpy(pat + k, "[!a]", 4); k += 4; break;
            }
        }
        pat[k] = 0;
        for (int i = 0; i < sl; i++)
            str[i] = (char)('a' + rnd() % 2);
        str[sl] = 0;
        long bt = 0;
        int r = glob_match(pat, str, &bt);
        check(r == glob_rec(pat, str), "fuzz glob");
        agree++;
        matched += r;
    }
    printf("fuzz agree=%d matched=%d, known-case backtracks=%ld\n", agree, matched, bt_total);
    /* pathological: star-heavy pattern stays polynomial */
    char big[400], pat[400];
    memset(big, 'a', 300);
    big[300] = 0;
    pat[0] = 0;
    for (int i = 0; i < 20; i++)
        strcat(pat, "*a");
    strcat(pat, "b");
    long bt = 0;
    int r = glob_match(pat, big, &bt);
    check(r == 0, "pathological");
    printf("pathological no-match backtracks: %ld\n", bt);
    return 0;
}
