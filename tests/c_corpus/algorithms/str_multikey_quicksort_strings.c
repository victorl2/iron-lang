/*
 * title: Multikey (three-way radix) quicksort for strings
 * topic: algorithms
 * covers: multikey quicksort, three-way partition on character position, shared prefixes, duplicates, qsort cross-check
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

/* Bentley-Sedgewick multikey (three-way radix) quicksort on strings. */
static long char_cmps;

static int at(const char *s, int d) {
    return (unsigned char)s[d]; /* s[len] is the NUL terminator, so it sorts first */
}

static void swap(const char **a, int i, int j) {
    const char *t = a[i];
    a[i] = a[j];
    a[j] = t;
}

static void mkqs(const char **a, int lo, int hi, int d) {
    if (hi - lo < 1)
        return;
    /* median-of-three pivot on the d-th character */
    int m = lo + (hi - lo) / 2;
    int x = at(a[lo], d), y = at(a[m], d), z = at(a[hi], d);
    int piv = (x < y) ? (y < z ? y : (x < z ? z : x)) : (x < z ? x : (y < z ? z : y));
    int lt = lo, gt = hi, i = lo;
    while (i <= gt) {
        int c = at(a[i], d);
        char_cmps++;
        if (c < piv)
            swap(a, lt++, i++);
        else if (c > piv)
            swap(a, i, gt--);
        else
            i++;
    }
    mkqs(a, lo, lt - 1, d);
    if (piv != 0)
        mkqs(a, lt, gt, d + 1);
    mkqs(a, gt + 1, hi, d);
}

static long strcmp_count;

static int cmp_counting(const void *x, const void *y) {
    const char *a = *(const char *const *)x, *b = *(const char *const *)y;
    while (*a && *a == *b) {
        a++;
        b++;
        strcmp_count++;
    }
    strcmp_count++;
    return (unsigned char)*a - (unsigned char)*b;
}

static const char *pool_alloc(char *pool, size_t *used, const char *s, size_t n) {
    char *p = pool + *used;
    memcpy(p, s, n + 1);
    *used += n + 1;
    return p;
}

int main(void) {
    enum { N = 3000 };
    static char pool[N * 40];
    static const char *a[N], *b[N];
    const char *modes[] = {"short random words", "long shared prefixes", "url-like paths", "many duplicates"};
    for (int mode = 0; mode < 4; mode++) {
        size_t used = 0;
        for (int i = 0; i < N; i++) {
            char w[40];
            int len;
            switch (mode) {
            case 0:
                len = 2 + (int)(rnd() % 8);
                rand_str(w, len, 26);
                break;
            case 1:
                memcpy(w, "internationalization", 20);
                len = 20 + (int)(rnd() % 8);
                for (int k = 20; k < len; k++)
                    w[k] = (char)('a' + rnd() % 3);
                w[len] = 0;
                break;
            case 2: {
                const char *dirs[] = {"/usr/lib/", "/usr/bin/", "/var/log/", "/etc/"};
                const char *d = dirs[rnd() % 4];
                size_t dl = strlen(d);
                memcpy(w, d, dl);
                len = (int)dl + 1 + (int)(rnd() % 6);
                for (int k = (int)dl; k < len; k++)
                    w[k] = (char)('a' + rnd() % 5);
                w[len] = 0;
                break;
            }
            default:
                len = 1 + (int)(rnd() % 3);
                rand_str(w, len, 3);
                break;
            }
            a[i] = pool_alloc(pool, &used, w, (size_t)len);
            b[i] = a[i];
        }
        char_cmps = 0;
        mkqs(a, 0, N - 1, 0);
        strcmp_count = 0;
        qsort(b, N, sizeof(char *), cmp_counting);
        long dups = 0;
        for (int i = 0; i < N; i++) {
            check(strcmp(a[i], b[i]) == 0, "multikey vs qsort");
            if (i && strcmp(a[i], a[i - 1]) == 0)
                dups++;
        }
        for (int i = 1; i < N; i++)
            check(strcmp(a[i - 1], a[i]) <= 0, "sorted");
        printf("%-22s first=%-10s last=%-10s dups=%-4ld multikey char compares=%ld\n", modes[mode], a[0],
               a[N - 1], dups, char_cmps);
        (void)strcmp_count;
    }
    return 0;
}
