/*
 * title: Compare two files like cmp and diff
 * topic: io_files
 * covers: first differing byte, line and column, differing byte count, prefix files, different lengths, common line diff
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int identical;
    long offset; /* 1-based offset of the first difference, 0 if none */
    long line, col;
    long ndiff;
    long len_a, len_b;
} Cmp;

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static void put(const char *name, const char *s) {
    FILE *f = fopen(name, "wb");
    check(f != NULL, "put open");
    check(fputs(s, f) >= 0, "put");
    fclose(f);
}

static Cmp compare(const char *na, const char *nb) {
    Cmp r = {1, 0, 1, 1, 0, 0, 0};
    FILE *a = fopen(na, "rb"), *b = fopen(nb, "rb");
    check(a && b, "compare open");
    long line = 1, col = 1, pos = 0;
    for (;;) {
        int x = fgetc(a), y = fgetc(b);
        if (x == EOF && y == EOF)
            break;
        pos++;
        if (x != y) {
            if (r.identical) {
                r.identical = 0;
                r.offset = pos;
                r.line = line;
                r.col = col;
            }
            r.ndiff++;
        }
        if (x != EOF)
            r.len_a++;
        if (y != EOF)
            r.len_b++;
        if (x == '\n') {
            line++;
            col = 1;
        } else
            col++;
    }
    fclose(a);
    fclose(b);
    return r;
}

/* Number of lines common to both files in the longest common subsequence. */
static int lcs_lines(const char *na, const char *nb, int *la, int *lb) {
    enum { MAXL = 32, W = 40 };
    static char A[MAXL][W], B[MAXL][W];
    int n = 0, m = 0;
    FILE *f = fopen(na, "r");
    while (n < MAXL && fgets(A[n], W, f))
        n++;
    fclose(f);
    f = fopen(nb, "r");
    while (m < MAXL && fgets(B[m], W, f))
        m++;
    fclose(f);
    static int dp[MAXL + 1][MAXL + 1];
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= m; j++) {
            if (i == 0 || j == 0)
                dp[i][j] = 0;
            else if (strcmp(A[i - 1], B[j - 1]) == 0)
                dp[i][j] = dp[i - 1][j - 1] + 1;
            else
                dp[i][j] = dp[i - 1][j] > dp[i][j - 1] ? dp[i - 1][j] : dp[i][j - 1];
        }
    *la = n;
    *lb = m;
    return dp[n][m];
}

int main(void) {
    struct {
        const char *label, *a, *b;
    } cases[] = {
        {"same", "alpha\nbeta\ngamma\n", "alpha\nbeta\ngamma\n"},
        {"one-byte", "alpha\nbeta\ngamma\n", "alpha\nbeTa\ngamma\n"},
        {"line-end", "alpha\nbeta\n", "alpha\nbeta"},
        {"prefix", "abc", "abcdef"},
        {"empty-vs-data", "", "x"},
        {"both-empty", "", ""},
        {"first-byte", "Zeta\n", "zeta\n"},
        {"many", "aaaaaaaaaa\nbbbbbbbbbb\n", "aaaaXaaaaa\nbbbbbbbbYb\n"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        put("a.txt", cases[i].a);
        put("b.txt", cases[i].b);
        Cmp r = compare("a.txt", "b.txt");
        printf("%-14s ", cases[i].label);
        if (r.identical)
            printf("identical (%ld bytes)\n", r.len_a);
        else
            printf("differ at byte %ld (line %ld col %ld), %ld differing positions, lengths %ld/%ld\n",
                   r.offset, r.line, r.col, r.ndiff, r.len_a, r.len_b);
        /* symmetric */
        Cmp s = compare("b.txt", "a.txt");
        check(s.identical == r.identical && s.offset == r.offset && s.ndiff == r.ndiff, "symmetry");
        check((strcmp(cases[i].a, cases[i].b) == 0) == r.identical, "vs strcmp");
    }

    put("a.txt", "one\ntwo\nthree\nfour\nfive\nsix\n");
    put("b.txt", "one\nthree\nfour\nfour-and-a-half\nsix\nseven\n");
    int la, lb;
    int common = lcs_lines("a.txt", "b.txt", &la, &lb);
    printf("line diff: %d and %d lines, %d in common, %d removed, %d added\n", la, lb, common,
           la - common, lb - common);
    check(common == 4, "lcs");

    remove("a.txt");
    remove("b.txt");
    return 0;
}
