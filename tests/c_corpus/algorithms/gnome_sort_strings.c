/*
 * title: Gnome sort on strings with a comparison callback
 * topic: algorithms
 * covers: gnome sort, string comparison, function pointer comparators, case folding, optimized gnome with jump-back
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 424242u;
static unsigned rng(void) {
    st = st * 1103515245u + 12345u;
    return (st >> 16) & 0x7fffu;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef int (*Cmp)(const char *, const char *);

static int cmp_plain(const char *a, const char *b) { return strcmp(a, b); }

static int cmp_fold(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y)
            return x < y ? -1 : 1;
        if (!x)
            break;
    }
    return strcmp(a, b); /* unreachable difference: strings fold-equal; a == b here */
}

static int cmp_len_first(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb)
        return la < lb ? -1 : 1;
    return strcmp(a, b);
}

static long gnome(const char **v, int n, Cmp cmp) {
    long steps = 0;
    int i = 0;
    while (i < n) {
        steps++;
        if (i == 0 || cmp(v[i - 1], v[i]) <= 0) {
            i++;
        } else {
            const char *t = v[i];
            v[i] = v[i - 1];
            v[i - 1] = t;
            i--;
        }
    }
    return steps;
}

/* Optimized: remember where we came from and jump forward again. */
static long gnome_jump(const char **v, int n, Cmp cmp) {
    long steps = 0;
    int i = 1, back = 2;
    while (i < n) {
        steps++;
        if (cmp(v[i - 1], v[i]) <= 0) {
            i = back;
            back++;
        } else {
            const char *t = v[i];
            v[i] = v[i - 1];
            v[i - 1] = t;
            if (--i == 0) {
                i = back;
                back++;
            }
        }
    }
    return steps;
}

static void mkword(char *buf, int len) {
    for (int i = 0; i < len; i++) {
        unsigned r = rng() % 52;
        buf[i] = (char)(r < 26 ? 'a' + r : 'A' + (r - 26));
    }
    buf[len] = 0;
}

int main(void) {
    enum { N = 60, W = 8 };
    char store[N][W];
    const char *orig[N], *v[N], *w[N];
    for (int i = 0; i < N; i++) {
        mkword(store[i], 2 + (int)(rng() % 5));
        orig[i] = store[i];
    }
    static const struct {
        const char *name;
        Cmp fn;
    } modes[] = {{"plain", cmp_plain}, {"fold", cmp_fold}, {"length", cmp_len_first}};
    for (int m = 0; m < 3; m++) {
        memcpy(v, orig, sizeof v);
        memcpy(w, orig, sizeof w);
        long s1 = gnome(v, N, modes[m].fn);
        long s2 = gnome_jump(w, N, modes[m].fn);
        for (int i = 1; i < N; i++) {
            check(modes[m].fn(v[i - 1], v[i]) <= 0, "sorted");
            check(modes[m].fn(w[i - 1], w[i]) <= 0, "jump sorted");
        }
        printf("%-6s steps=%ld jump_steps=%ld\n", modes[m].name, s1, s2);
        printf("  first:");
        for (int i = 0; i < 6; i++)
            printf(" %s", v[i]);
        printf("\n  last:");
        for (int i = N - 4; i < N; i++)
            printf(" %s", v[i]);
        printf("\n");
    }
    return 0;
}
