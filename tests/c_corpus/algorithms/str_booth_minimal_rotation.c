/*
 * title: Minimal rotation and necklace canonical form
 * topic: algorithms
 * covers: Booth algorithm, two-pointer minimal rotation, necklace counting, brute-force comparison
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

/* Booth's algorithm: index of the lexicographically least rotation in O(n). */
static int booth(const char *s, int n) {
    int *f = malloc(sizeof(int) * (size_t)(2 * n));
    for (int i = 0; i < 2 * n; i++)
        f[i] = -1;
    int k = 0;
    for (int j = 1; j < 2 * n; j++) {
        char sj = s[j % n];
        int i = f[j - k - 1];
        while (i != -1 && sj != s[(k + i + 1) % n]) {
            if (sj < s[(k + i + 1) % n])
                k = j - i - 1;
            i = f[i];
        }
        if (sj != s[(k + i + 1) % n]) {
            if (sj < s[k % n])
                k = j;
            f[j - k] = -1;
        } else {
            f[j - k] = i + 1;
        }
    }
    free(f);
    return k % n;
}

/* Two-pointer minimal rotation (Duval-free variant). */
static int min_rot_two_pointer(const char *s, int n) {
    int i = 0, j = 1, k = 0;
    while (i < n && j < n && k < n) {
        char a = s[(i + k) % n], b = s[(j + k) % n];
        if (a == b) {
            k++;
            continue;
        }
        if (a > b)
            i += k + 1;
        else
            j += k + 1;
        if (i == j)
            j++;
        k = 0;
    }
    return i < j ? i : j;
}

static int min_rot_brute(const char *s, int n) {
    int best = 0;
    for (int r = 1; r < n; r++) {
        int c = 0;
        for (int k = 0; k < n && !c; k++)
            c = (unsigned char)s[(r + k) % n] - (unsigned char)s[(best + k) % n];
        if (c < 0)
            best = r;
    }
    return best;
}

static void rotated(const char *s, int n, int r, char *out) {
    for (int k = 0; k < n; k++)
        out[k] = s[(r + k) % n];
    out[n] = 0;
}

int main(void) {
    const char *fixed[] = {"bca", "cabcab", "aaaa", "dcbabcd", "abab", "zyxwv", "baabaa", "x"};
    char out[64];
    for (int c = 0; c < 8; c++) {
        int n = (int)strlen(fixed[c]);
        int a = booth(fixed[c], n), b = min_rot_two_pointer(fixed[c], n);
        char o1[64], o2[64], o3[64];
        rotated(fixed[c], n, a, o1);
        rotated(fixed[c], n, b, o2);
        rotated(fixed[c], n, min_rot_brute(fixed[c], n), o3);
        check(strcmp(o1, o3) == 0 && strcmp(o2, o3) == 0, "rotations equal brute");
        printf("%-8s booth=%d two_pointer=%d -> %s\n", fixed[c], a, b, o1);
    }
    int mism = 0, distinct_canon = 0;
    for (int t = 0; t < 1500; t++) {
        int n = 1 + (int)(rnd() % 30);
        char s[40];
        rand_str(s, n, 1 + t % 3);
        int a = booth(s, n), b = min_rot_two_pointer(s, n), c = min_rot_brute(s, n);
        char ra[48], rb[48], rc[48];
        rotated(s, n, a, ra);
        rotated(s, n, b, rb);
        rotated(s, n, c, rc);
        if (strcmp(ra, rc) != 0 || strcmp(rb, rc) != 0)
            mism++;
    }
    check(mism == 0, "random rotations");
    /* necklace canonicalization: group all binary strings of length 8 */
    int seen[256] = {0}, classes = 0;
    for (int v = 0; v < 256; v++) {
        char s[9];
        for (int i = 0; i < 8; i++)
            s[i] = (char)('0' + ((v >> (7 - i)) & 1));
        s[8] = 0;
        rotated(s, 8, booth(s, 8), out);
        int cv = 0;
        for (int i = 0; i < 8; i++)
            cv = cv * 2 + (out[i] - '0');
        if (!seen[cv]) {
            seen[cv] = 1;
            classes++;
        }
    }
    distinct_canon = classes;
    printf("random mismatches: %d\n", mism);
    printf("binary necklaces of length 8: %d\n", distinct_canon);
    check(distinct_canon == 36, "necklace count");
    return 0;
}
