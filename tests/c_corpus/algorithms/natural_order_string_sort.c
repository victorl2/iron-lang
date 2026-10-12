/*
 * title: Natural-order string sorting
 * topic: algorithms
 * covers: natural sort, digit run comparison, leading zeros, case-insensitive ties, comparator totality, qsort
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 3030u;
static unsigned rng(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

/* Compare digit runs by numeric value without converting (handles arbitrary length). */
static int cmp_digits(const char **pa, const char **pb) {
    const char *a = *pa, *b = *pb;
    const char *za = a, *zb = b;
    while (*za == '0')
        za++;
    while (*zb == '0')
        zb++;
    const char *ea = za, *eb = zb;
    while (isdigit((unsigned char)*ea))
        ea++;
    while (isdigit((unsigned char)*eb))
        eb++;
    long la = ea - za, lb = eb - zb;
    int r = 0;
    if (la != lb)
        r = la < lb ? -1 : 1;
    else
        r = strncmp(za, zb, (size_t)la);
    *pa = ea;
    *pb = eb;
    /* remember leading-zero difference separately */
    return r;
}

static int natcmp(const char *a, const char *b) {
    const char *sa = a, *sb = b;
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            int r = cmp_digits(&a, &b);
            if (r)
                return r;
        } else {
            int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
            if (x != y)
                return x < y ? -1 : 1;
            a++;
            b++;
        }
    }
    if (*a || *b)
        return *a ? 1 : -1;
    /* natural-equal: fall back to byte order so the ordering is total */
    return strcmp(sa, sb);
}

static int cmp_qsort(const void *x, const void *y) { return natcmp(*(const char *const *)x, *(const char *const *)y); }
static int cmp_plain(const void *x, const void *y) { return strcmp(*(const char *const *)x, *(const char *const *)y); }

int main(void) {
    const char *files[] = {"img12.png", "img10.png", "img2.png",  "img1.png",  "IMG3.png", "img02.png",
                           "img002.png", "doc9.txt",  "doc10.txt", "doc010.txt", "Doc1.txt", "a1b2",
                           "a1b10",      "a01b3",     "a",         "",          "a0",       "a00",
                           "v1.9.12",    "v1.10.2",   "v1.9.3",    "v1.10",     "x99999999999999999999",
                           "x100000000000000000000"};
    enum { N = sizeof(files) / sizeof(files[0]) };
    const char *nat[N], *plain[N];
    memcpy(nat, files, sizeof nat);
    memcpy(plain, files, sizeof plain);
    qsort(nat, N, sizeof(char *), cmp_qsort);
    qsort(plain, N, sizeof(char *), cmp_plain);
    printf("natural | plain\n");
    for (int i = 0; i < N; i++)
        printf("%-24s| %s\n", nat[i][0] ? nat[i] : "(empty)", plain[i][0] ? plain[i] : "(empty)");

    /* comparator sanity: antisymmetry and transitivity on random names */
    enum { M = 40 };
    char names[M][16];
    for (int i = 0; i < M; i++) {
        int len = 0;
        int parts = 1 + (int)(rng() % 3);
        for (int p = 0; p < parts; p++) {
            names[i][len++] = (char)('a' + rng() % 3);
            int digits = 1 + (int)(rng() % 3);
            for (int d = 0; d < digits; d++)
                names[i][len++] = (char)('0' + rng() % 4);
        }
        names[i][len] = 0;
    }
    for (int i = 0; i < M; i++)
        for (int j = 0; j < M; j++) {
            int c1 = natcmp(names[i], names[j]), c2 = natcmp(names[j], names[i]);
            check((c1 > 0) == (c2 < 0) && (c1 < 0) == (c2 > 0), "antisymmetric");
            for (int k = 0; k < M; k++)
                if (natcmp(names[i], names[j]) <= 0 && natcmp(names[j], names[k]) <= 0)
                    check(natcmp(names[i], names[k]) <= 0, "transitive");
        }
    printf("comparator checks passed for %d random names\n", M);
    return 0;
}
