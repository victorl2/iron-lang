/*
 * title: fnmatch flag matrix and a from-scratch matcher fuzzed against libc
 * topic: io_files
 * covers: fnmatch, FNM_PATHNAME, FNM_PERIOD, FNM_NOESCAPE, bracket expressions, POSIX classes, escapes, backtracking matcher, randomized differential test
 * deps: libc, posix
 */
#include <fnmatch.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);       \
            exit(1);                                                   \
        }                                                              \
    } while (0)

static inline uint32_t rng_next(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}


typedef struct {
    const char *pat, *str;
    int flags;
    int want; /* 1 = match */
} Case;

static const Case table[] = {
    {"*", "abc", 0, 1},
    {"*", "", 0, 1},
    {"a*c", "abbbc", 0, 1},
    {"a*c", "ab", 0, 0},
    {"a?c", "abc", 0, 1},
    {"a?c", "ac", 0, 0},
    {"[abc]x", "bx", 0, 1},
    {"[a-c]x", "dx", 0, 0},
    {"[!a-c]x", "dx", 0, 1},
    {"[]a]x", "]x", 0, 1},
    {"[[:digit:]]*", "7up", 0, 1},
    {"[[:alpha:]]*", "7up", 0, 0},
    {"[[:upper:]][[:lower:]]", "Ab", 0, 1},
    {"a\\*b", "a*b", 0, 1},
    {"a\\*b", "axb", 0, 0},
    {"a\\*b", "a\\xb", FNM_NOESCAPE, 1},
    {"a\\*b", "a*b", FNM_NOESCAPE, 0},
    {"*", "a/b", 0, 1},
    {"*", "a/b", FNM_PATHNAME, 0},
    {"a?b", "a/b", 0, 1},
    {"a?b", "a/b", FNM_PATHNAME, 0},
    {"a[/]b", "a/b", FNM_PATHNAME, 0},
    {"*/*", "a/b", FNM_PATHNAME, 1},
    {"*/*", "a/b/c", FNM_PATHNAME, 0},
    {"*/*/*", "a/b/c", FNM_PATHNAME, 1},
    {"*", ".hidden", 0, 1},
    {"*", ".hidden", FNM_PERIOD, 0},
    {".*", ".hidden", FNM_PERIOD, 1},
    {"?hidden", ".hidden", FNM_PERIOD, 0},
    {"[.]hidden", ".hidden", FNM_PERIOD, 0},
    {"a/*", "a/.x", FNM_PERIOD, 1},   /* period only special at the start without PATHNAME */
    {"a/*", "a/.x", FNM_PERIOD | FNM_PATHNAME, 0},
    {"a/.*", "a/.x", FNM_PERIOD | FNM_PATHNAME, 1},
    {"*.c", "main.c", FNM_PERIOD | FNM_PATHNAME, 1},
    {"*.c", "dir/main.c", FNM_PATHNAME, 0},
    {"*/*.c", "dir/main.c", FNM_PATHNAME, 1},
    {"**", "a/b", FNM_PATHNAME, 0},   /* no globstar in fnmatch */
    {"**", "ab", FNM_PATHNAME, 1},
    {"a*b*c", "aXbYbZc", 0, 1},
    {"a*b*c", "aXbYbZ", 0, 0},
    {"", "", 0, 1},
    {"", "a", 0, 0},
    {"???", "abc", 0, 1},
    {"???", "ab", 0, 0},
};

static const char *flag_str(int f, char *buf) {
    buf[0] = 0;
    if (f & FNM_PATHNAME) strcat(buf, "P");
    if (f & FNM_PERIOD) strcat(buf, "D");
    if (f & FNM_NOESCAPE) strcat(buf, "E");
    if (!buf[0]) strcat(buf, "-");
    return buf;
}

/* reference matcher: recursive with backtracking; handles * ? [set] [!set] classes and \ escapes */
static int in_class(const char *cls, size_t l, int c) {
    if (l == 5 && !strncmp(cls, "digit", 5)) return c >= '0' && c <= '9';
    if (l == 5 && !strncmp(cls, "alpha", 5)) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (l == 5 && !strncmp(cls, "upper", 5)) return c >= 'A' && c <= 'Z';
    if (l == 5 && !strncmp(cls, "lower", 5)) return c >= 'a' && c <= 'z';
    return 0;
}

static int my_match_here(const char *p, const char *s, int flags, int at_start);

/* bracket: returns 1 match, 0 no match; sets *end past the ']' ; on malformed bracket treat '[' literally */
static int bracket(const char *p, int c, const char **end) {
    const char *q = p + 1;
    int neg = 0, ok = 0;
    if (*q == '!') {
        neg = 1;
        q++;
    }
    const char *first = q;
    while (*q && (*q != ']' || q == first)) {
        if (q[0] == '[' && q[1] == ':') {
            const char *e = strstr(q + 2, ":]");
            if (e) {
                if (in_class(q + 2, (size_t)(e - (q + 2)), c)) ok = 1;
                q = e + 2;
                continue;
            }
        }
        if (q[1] == '-' && q[2] && q[2] != ']') {
            if ((unsigned char)c >= (unsigned char)q[0] && (unsigned char)c <= (unsigned char)q[2]) ok = 1;
            q += 3;
            continue;
        }
        if (c == (unsigned char)*q) ok = 1;
        q++;
    }
    if (*q != ']') {
        *end = NULL;
        return -1;
    }
    *end = q + 1;
    return neg ? !ok : ok;
}

static int my_match_here(const char *p, const char *s, int flags, int at_start) {
    int period_block = (flags & FNM_PERIOD) && *s == '.' &&
                       (at_start || ((flags & FNM_PATHNAME) && s[-1] == '/'));
    for (;;) {
        switch (*p) {
        case 0: return *s == 0;
        case '?':
            if (!*s || period_block || ((flags & FNM_PATHNAME) && *s == '/')) return 0;
            p++;
            s++;
            at_start = 0;
            period_block = 0;
            continue;
        case '*': {
            if (period_block) return 0;
            while (*p == '*') p++;
            for (const char *t = s;; t++) {
                if (my_match_here(p, t, flags, 0)) return 1;
                if (!*t || ((flags & FNM_PATHNAME) && *t == '/')) return 0;
            }
        }
        case '[': {
            if (!*s || period_block || ((flags & FNM_PATHNAME) && *s == '/')) return 0;
            const char *end;
            int r = bracket(p, (unsigned char)*s, &end);
            if (r < 0) {
                if (*s != '[') return 0;
                p++;
                s++;
            } else {
                if (!r) return 0;
                p = end;
                s++;
            }
            at_start = 0;
            period_block = 0;
            continue;
        }
        case '\\':
            if (!(flags & FNM_NOESCAPE) && p[1]) p++;
            /* fallthrough */
        default:
            if (*p != *s) return 0;
            p++;
            s++;
            at_start = 0;
            period_block = 0;
            if (*s == 0 && *p == 0) return 1;
            /* recompute period rule after a separator under PATHNAME */
            period_block = (flags & FNM_PERIOD) && (flags & FNM_PATHNAME) && *s == '.' && s[-1] == '/';
            continue;
        }
    }
}

static int my_fnmatch(const char *pat, const char *str, int flags) {
    return my_match_here(pat, str, flags, 1) ? 0 : FNM_NOMATCH;
}

int main(void) {
    int bad = 0;
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        const Case *c = &table[i];
        int lib = fnmatch(c->pat, c->str, c->flags) == 0;
        int mine = my_fnmatch(c->pat, c->str, c->flags) == 0;
        char fb[8];
        CHECK(lib == c->want);
        CHECK(mine == c->want);
        printf("%-3s %-14s %-9s -> %s\n", flag_str(c->flags, fb), c->pat[0] ? c->pat : "(empty)",
               c->str[0] ? c->str : "(empty)", lib ? "match" : "no");
        bad += lib != mine;
    }
    CHECK(bad == 0);

    /* differential fuzz over a small alphabet, all four flag combinations */
    static const char *ptoks[] = {"a", "b", "*", "?", ".", "/", "[ab]", "[!a]", "[a-b]", "\\*", "\\a"};
    static const char stoks[] = {'a', 'b', '.', '/', '*'};
    uint32_t seed = 0xBEEF;
    long matches[4] = {0, 0, 0, 0};
    static const int fl[4] = {0, FNM_PATHNAME, FNM_PERIOD, FNM_PATHNAME | FNM_PERIOD};
    const int N = 6000;
    for (int t = 0; t < N; t++) {
        char pat[24] = "", str[12];
        uint32_t np = 1 + rng_next(&seed) % 5;
        for (uint32_t i = 0; i < np; i++) {
            uint32_t k = rng_next(&seed) % (uint32_t)(sizeof ptoks / sizeof ptoks[0]);
            strcat(pat, ptoks[k]);
        }
        uint32_t ns = rng_next(&seed) % 6;
        for (uint32_t i = 0; i < ns; i++) str[i] = stoks[rng_next(&seed) % 5];
        str[ns] = 0;
        for (int f = 0; f < 4; f++) {
            int lib = fnmatch(pat, str, fl[f]) == 0;
            int mine = my_fnmatch(pat, str, fl[f]) == 0;
            if (lib != mine) {
                fprintf(stderr, "MISMATCH pat=%s str=%s flags=%d lib=%d mine=%d\n", pat, str, fl[f], lib, mine);
                exit(1);
            }
            matches[f] += lib;
        }
    }
    printf("fuzz: %d patterns x 4 flag sets agree with libc; matches none=%ld pathname=%ld period=%ld both=%ld\n", N,
           matches[0], matches[1], matches[2], matches[3]);
    return 0;
}
