/*
 * title: Trie search with dot and star wildcards
 * topic: data_structures
 * covers: trie, wildcard pattern matching, backtracking, query stamps for dedup, glob brute force
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rng_s = 88172645463325252ULL;

static unsigned rnd(unsigned n) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 7;
    rng_s ^= rng_s << 17;
    return (unsigned)((rng_s >> 16) % n);
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

#define AL 4
typedef struct Node {
    struct Node *kid[AL];
    int word;  /* 1 if a word ends here */
    int stamp; /* last query that counted this word */
} Node;

static Node *nnew(void) {
    Node *n = calloc(1, sizeof *n);
    check(n != NULL, "alloc");
    return n;
}

static int add(Node *root, const char *w) {
    Node *n = root;
    for (; *w; w++) {
        int c = *w - 'a';
        if (!n->kid[c])
            n->kid[c] = nnew();
        n = n->kid[c];
    }
    int fresh = !n->word;
    n->word = 1;
    return fresh;
}

static int cur_query = 0;
static long steps = 0;

/* '.' matches one letter, '*' matches any (possibly empty) run */
static int match(Node *n, const char *p) {
    steps++;
    if (*p == 0) {
        if (n->word && n->stamp != cur_query) {
            n->stamp = cur_query;
            return 1;
        }
        return 0;
    }
    int total = 0;
    if (*p == '*') {
        total += match(n, p + 1);
        for (int i = 0; i < AL; i++)
            if (n->kid[i])
                total += match(n->kid[i], p);
        return total;
    }
    if (*p == '.') {
        for (int i = 0; i < AL; i++)
            if (n->kid[i])
                total += match(n->kid[i], p + 1);
        return total;
    }
    Node *k = n->kid[*p - 'a'];
    return k ? match(k, p + 1) : 0;
}

static int glob(const char *p, const char *s) {
    if (*p == 0)
        return *s == 0;
    if (*p == '*') {
        for (const char *t = s;; t++) {
            if (glob(p + 1, t))
                return 1;
            if (!*t)
                return 0;
        }
    }
    if (*s == 0)
        return 0;
    if (*p == '.' || *p == *s)
        return glob(p + 1, s + 1);
    return 0;
}

static void nfree(Node *n) {
    for (int i = 0; i < AL; i++)
        if (n->kid[i])
            nfree(n->kid[i]);
    free(n);
}

static char words[1200][10];
static int nw;

int main(void) {
    Node *root = nnew();
    for (int i = 0; i < 900; i++) {
        char w[10];
        int len = 1 + (int)rnd(7);
        for (int j = 0; j < len; j++)
            w[j] = (char)('a' + rnd(AL));
        w[len] = 0;
        if (add(root, w))
            strcpy(words[nw++], w);
    }
    printf("distinct words=%d\n", nw);
    const char alpha[] = "abcd..*";
    long total = 0;
    int fixed_shown = 0;
    for (int q = 1; q <= 500; q++) {
        char p[8];
        int len = 1 + (int)rnd(5);
        for (int j = 0; j < len; j++)
            p[j] = alpha[rnd(7)];
        p[len] = 0;
        cur_query = q;
        int got = match(root, p);
        int want = 0;
        for (int i = 0; i < nw; i++)
            want += glob(p, words[i]);
        check(got == want, "wildcard count");
        total += got;
        if (q <= 8) {
            printf("pattern %-6s -> %d\n", p, got);
            fixed_shown++;
        }
    }
    printf("total matches=%ld steps=%ld\n", total, steps);
    const char *fixed[] = {"*", ".", "..", "a*", "*a", "a*a", "*ab*", "...."};
    for (int i = 0; i < 8; i++) {
        cur_query = 1000 + i;
        int got = match(root, fixed[i]);
        int want = 0;
        for (int j = 0; j < nw; j++)
            want += glob(fixed[i], words[j]);
        check(got == want, "fixed pattern");
        printf("fixed %-5s -> %d\n", fixed[i], got);
    }
    check(fixed_shown == 8, "shown");
    cur_query = 5000;
    check(match(root, "*") == nw, "star matches every word");
    nfree(root);
    return 0;
}
