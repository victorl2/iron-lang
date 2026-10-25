/*
 * title: Palindromic tree (eertree)
 * topic: algorithms
 * covers: eertree, palindromic substrings, suffix links, occurrence propagation, brute-force cross-check
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

/* Palindromic tree (eertree): one node per distinct palindromic substring. */
enum { ALPHA = 26, MAXN = 4100 };

typedef struct {
    int next[ALPHA];
    int link;
    int len;
    long occ; /* occurrences (filled by propagating along suffix links) */
} PNode;

static PNode T[MAXN + 2];
static int tcount, last_node;

static int new_pnode(int len) {
    for (int i = 0; i < ALPHA; i++)
        T[tcount].next[i] = 0;
    T[tcount].len = len;
    T[tcount].link = 0;
    T[tcount].occ = 0;
    return tcount++;
}

static void eertree_init(void) {
    tcount = 0;
    new_pnode(-1); /* node 0: imaginary root, len -1 */
    new_pnode(0);  /* node 1: empty root */
    T[1].link = 0;
    T[0].link = 0;
    last_node = 1;
}

/* returns nodes added (0 or 1) */
static int eertree_add(const char *s, int i) {
    int c = s[i] - 'a';
    int cur = last_node;
    while (!(i - 1 - T[cur].len >= 0 && s[i - 1 - T[cur].len] == s[i]))
        cur = T[cur].link;
    int added = 0;
    if (!T[cur].next[c]) {
        int nn = new_pnode(T[cur].len + 2);
        if (T[nn].len == 1)
            T[nn].link = 1;
        else {
            int p = T[cur].link;
            while (!(i - 1 - T[p].len >= 0 && s[i - 1 - T[p].len] == s[i]))
                p = T[p].link;
            T[nn].link = T[p].next[c];
        }
        T[cur].next[c] = nn;
        added = 1;
    }
    last_node = T[cur].next[c];
    T[last_node].occ++;
    return added;
}

static int is_pal(const char *s, int l, int r) {
    while (l < r)
        if (s[l++] != s[r--])
            return 0;
    return 1;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(void) {
    static char s[2001];
    const char *fixed[] = {"eertree", "abacaba", "aaaaa", "abcde", "mississippi"};
    for (int c = 0; c < 5; c++) {
        int n = (int)strlen(fixed[c]);
        eertree_init();
        for (int i = 0; i < n; i++)
            eertree_add(fixed[c], i);
        int distinct = tcount - 2;
        long total = 0;
        for (int v = tcount - 1; v >= 2; v--) {
            T[T[v].link].occ += T[v].occ;
        }
        for (int v = 2; v < tcount; v++)
            total += T[v].occ;
        printf("%-12s distinct palindromes=%d, total occurrences=%ld\n", fixed[c], distinct, total);
    }

    /* brute-force validation on random strings, including occurrence counts */
    int alph[] = {1, 2, 2, 3, 4};
    for (int c = 0; c < 5; c++) {
        int n = 30 + (int)(rnd() % 70);
        rand_str(s, n, alph[c]);
        eertree_init();
        for (int i = 0; i < n; i++)
            eertree_add(s, i);
        for (int v = tcount - 1; v >= 2; v--)
            T[T[v].link].occ += T[v].occ;
        long total = 0;
        for (int v = 2; v < tcount; v++)
            total += T[v].occ;

        int maxc = n * (n + 1) / 2;
        char **pals = malloc(sizeof(char *) * (size_t)maxc);
        int np = 0;
        long btotal = 0;
        for (int i = 0; i < n; i++)
            for (int j = i; j < n; j++)
                if (is_pal(s, i, j)) {
                    char *p = malloc((size_t)(j - i + 2));
                    memcpy(p, s + i, (size_t)(j - i + 1));
                    p[j - i + 1] = 0;
                    pals[np++] = p;
                    btotal++;
                }
        qsort(pals, (size_t)np, sizeof(char *), cmp_str);
        int uniq = 0;
        for (int i = 0; i < np; i++)
            if (i == 0 || strcmp(pals[i], pals[i - 1]) != 0)
                uniq++;
        check(uniq == tcount - 2, "distinct palindrome count");
        check(btotal == total, "total occurrences");
        int longest = 0;
        for (int v = 2; v < tcount; v++)
            if (T[v].len > longest)
                longest = T[v].len;
        printf("random n=%d alphabet=%d distinct=%d total=%ld longest=%d\n", n, alph[c], uniq, total,
               longest);
        for (int i = 0; i < np; i++)
            free(pals[i]);
        free(pals);
    }
    /* a string of length n has at most n distinct palindromes */
    rand_str(s, 2000, 2);
    eertree_init();
    for (int i = 0; i < 2000; i++)
        eertree_add(s, i);
    check(tcount - 2 <= 2000, "at most n distinct palindromes");
    printf("n=2000 binary: %d distinct palindromes\n", tcount - 2);
    return 0;
}
