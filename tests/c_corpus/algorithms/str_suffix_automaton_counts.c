/*
 * title: Suffix automaton: distinct substrings and occurrences
 * topic: algorithms
 * covers: suffix automaton, clone states, suffix link tree counts, longest common substring, brute-force checks
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

/* Suffix automaton: distinct substrings, occurrence counts, longest common substring. */
enum { ALPHA = 26 };

typedef struct {
    int next[ALPHA];
    int link;
    int len;
    long cnt;
    int cloned;
} St;

typedef struct {
    St *st;
    int size, last, cap;
} Sam;

static int sam_new(Sam *a, int len) {
    St *s = &a->st[a->size];
    for (int i = 0; i < ALPHA; i++)
        s->next[i] = -1;
    s->link = -1;
    s->len = len;
    s->cnt = 0;
    s->cloned = 0;
    return a->size++;
}

static void sam_init(Sam *a, int maxlen) {
    a->cap = 2 * maxlen + 2;
    a->st = malloc(sizeof(St) * (size_t)a->cap);
    a->size = 0;
    a->last = sam_new(a, 0);
}

static void sam_extend(Sam *a, int c) {
    int cur = sam_new(a, a->st[a->last].len + 1);
    a->st[cur].cnt = 1;
    int p = a->last;
    while (p != -1 && a->st[p].next[c] < 0) {
        a->st[p].next[c] = cur;
        p = a->st[p].link;
    }
    if (p == -1) {
        a->st[cur].link = 0;
    } else {
        int q = a->st[p].next[c];
        if (a->st[p].len + 1 == a->st[q].len) {
            a->st[cur].link = q;
        } else {
            int cl = sam_new(a, a->st[p].len + 1);
            memcpy(a->st[cl].next, a->st[q].next, sizeof a->st[q].next);
            a->st[cl].link = a->st[q].link;
            a->st[cl].cloned = 1;
            while (p != -1 && a->st[p].next[c] == q) {
                a->st[p].next[c] = cl;
                p = a->st[p].link;
            }
            a->st[q].link = cl;
            a->st[cur].link = cl;
        }
    }
    a->last = cur;
}

static int by_len_cmp(const void *x, const void *y);
static const St *g_st;

static int by_len_cmp(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    if (g_st[a].len != g_st[b].len)
        return g_st[b].len - g_st[a].len; /* descending length */
    return a - b;
}

static void sam_counts(Sam *a) {
    int *order = malloc(sizeof(int) * (size_t)a->size);
    for (int i = 0; i < a->size; i++)
        order[i] = i;
    g_st = a->st;
    qsort(order, (size_t)a->size, sizeof(int), by_len_cmp);
    for (int i = 0; i < a->size; i++) {
        int v = order[i];
        if (a->st[v].link >= 0)
            a->st[a->st[v].link].cnt += a->st[v].cnt;
    }
    free(order);
}

static long sam_distinct(const Sam *a) {
    long t = 0;
    for (int v = 1; v < a->size; v++)
        t += a->st[v].len - a->st[a->st[v].link].len;
    return t;
}

static long sam_occurrences(const Sam *a, const char *p) {
    int v = 0;
    for (; *p; p++) {
        v = a->st[v].next[*p - 'a'];
        if (v < 0)
            return 0;
    }
    return a->st[v].cnt;
}

static int lcsub(const Sam *a, const char *t, int *end) {
    int v = 0, l = 0, best = 0;
    *end = 0;
    for (int i = 0; t[i]; i++) {
        int c = t[i] - 'a';
        while (v && a->st[v].next[c] < 0) {
            v = a->st[v].link;
            l = a->st[v].len;
        }
        if (a->st[v].next[c] >= 0) {
            v = a->st[v].next[c];
            l++;
        }
        if (l > best) {
            best = l;
            *end = i + 1;
        }
    }
    return best;
}

static long occ_brute(const char *s, const char *p) {
    long c = 0;
    size_t m = strlen(p);
    for (size_t i = 0; s[i] && i + m <= strlen(s); i++)
        c += strncmp(s + i, p, m) == 0;
    return c;
}

int main(void) {
    static char s[601], t[601], p[8];
    const char *fixed[] = {"abcbc", "aaaa", "banana", "abcabcabc"};
    for (int c = 0; c < 4; c++) {
        int n = (int)strlen(fixed[c]);
        Sam a;
        sam_init(&a, n);
        for (int i = 0; i < n; i++)
            sam_extend(&a, fixed[c][i] - 'a');
        sam_counts(&a);
        printf("%-10s states=%2d distinct=%2ld\n", fixed[c], a.size, sam_distinct(&a));
        free(a.st);
    }
    for (int r = 0; r < 6; r++) {
        int n = 100 + (int)(rnd() % 400);
        rand_str(s, n, 2 + r % 3);
        Sam a;
        sam_init(&a, n);
        for (int i = 0; i < n; i++)
            sam_extend(&a, s[i] - 'a');
        sam_counts(&a);
        check(a.size <= 2 * n, "state bound");
        /* distinct substrings vs suffix based count */
        long d = sam_distinct(&a);
        long brute = 0;
        {
            /* count via sorted suffix LCP */
            int *sa = malloc(sizeof(int) * (size_t)n);
            for (int i = 0; i < n; i++)
                sa[i] = i;
            /* insertion sort suffixes */
            for (int i = 1; i < n; i++) {
                int x = sa[i], j = i - 1;
                while (j >= 0 && strcmp(s + sa[j], s + x) > 0) {
                    sa[j + 1] = sa[j];
                    j--;
                }
                sa[j + 1] = x;
            }
            for (int i = 0; i < n; i++) {
                brute += n - sa[i];
                if (i > 0) {
                    int k = 0;
                    while (s[sa[i - 1] + k] && s[sa[i - 1] + k] == s[sa[i] + k])
                        k++;
                    brute -= k;
                }
            }
            free(sa);
        }
        check(d == brute, "distinct count");
        long occ_sum = 0;
        for (int q = 0; q < 40; q++) {
            int pl = 1 + (int)(rnd() % 4);
            rand_str(p, pl, 2 + r % 3);
            long o = sam_occurrences(&a, p);
            check(o == occ_brute(s, p), "occurrence count");
            occ_sum += o;
        }
        rand_str(t, 100 + (int)(rnd() % 200), 2 + r % 3);
        int end;
        int l = lcsub(&a, t, &end);
        int bl = 0;
        for (int i = 0; t[i]; i++)
            for (int j = 0; j < n; j++) {
                int k = 0;
                while (t[i + k] && s[j + k] && t[i + k] == s[j + k])
                    k++;
                if (k > bl)
                    bl = k;
            }
        check(l == bl, "longest common substring");
        printf("n=%3d alpha=%d states=%3d distinct=%5ld occ(40 queries)=%ld lcsub=%d\n", n, 2 + r % 3,
               a.size, d, occ_sum, l);
        free(a.st);
    }
    return 0;
}
