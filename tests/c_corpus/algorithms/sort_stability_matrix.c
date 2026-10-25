/*
 * title: Stability matrix of classic sorting algorithms
 * topic: algorithms
 * covers: stability testing, sort algorithm table via function pointers, tagged records, counter-example search, adjacent-equal-key check
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int key;
    int seq;
} Rec;

static unsigned st = 5551212u;
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

static void swp(Rec *a, Rec *b) {
    Rec t = *a;
    *a = *b;
    *b = t;
}

static void s_insertion(Rec *a, int n) {
    for (int i = 1; i < n; i++) {
        Rec x = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].key > x.key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

static void s_bubble(Rec *a, int n) {
    for (int p = 0; p < n - 1; p++)
        for (int i = 0; i + 1 < n - p; i++)
            if (a[i].key > a[i + 1].key)
                swp(&a[i], &a[i + 1]);
}

static void s_selection(Rec *a, int n) {
    for (int i = 0; i + 1 < n; i++) {
        int m = i;
        for (int j = i + 1; j < n; j++)
            if (a[j].key < a[m].key)
                m = j;
        swp(&a[i], &a[m]);
    }
}

static void s_shell(Rec *a, int n) {
    for (int h = n / 2; h > 0; h /= 2)
        for (int i = h; i < n; i++) {
            Rec x = a[i];
            int j = i;
            while (j >= h && a[j - h].key > x.key) {
                a[j] = a[j - h];
                j -= h;
            }
            a[j] = x;
        }
}

static void msort(Rec *a, Rec *t, int n) {
    if (n < 2)
        return;
    int m = n / 2;
    msort(a, t, m);
    msort(a + m, t, n - m);
    int i = 0, j = m, k = 0;
    while (i < m && j < n)
        t[k++] = a[j].key < a[i].key ? a[j++] : a[i++];
    while (i < m)
        t[k++] = a[i++];
    while (j < n)
        t[k++] = a[j++];
    memcpy(a, t, sizeof(Rec) * (size_t)n);
}

static void s_merge(Rec *a, int n) {
    Rec *t = malloc(sizeof(Rec) * (size_t)n);
    check(t != NULL, "alloc");
    msort(a, t, n);
    free(t);
}

static void sift(Rec *a, int n, int i) {
    for (;;) {
        int c = 2 * i + 1;
        if (c >= n)
            return;
        if (c + 1 < n && a[c + 1].key > a[c].key)
            c++;
        if (a[c].key <= a[i].key)
            return;
        swp(&a[c], &a[i]);
        i = c;
    }
}

static void s_heap(Rec *a, int n) {
    for (int i = n / 2 - 1; i >= 0; i--)
        sift(a, n, i);
    for (int e = n - 1; e > 0; e--) {
        swp(&a[0], &a[e]);
        sift(a, e, 0);
    }
}

static void qs(Rec *a, int lo, int hi) {
    if (lo >= hi)
        return;
    int p = a[hi].key, i = lo;
    for (int j = lo; j < hi; j++)
        if (a[j].key < p)
            swp(&a[i++], &a[j]);
    swp(&a[i], &a[hi]);
    qs(a, lo, i - 1);
    qs(a, i + 1, hi);
}

static void s_quick(Rec *a, int n) { qs(a, 0, n - 1); }

static void s_counting(Rec *a, int n) {
    int cnt[17] = {0};
    Rec *o = malloc(sizeof(Rec) * (size_t)n);
    check(o != NULL, "alloc");
    for (int i = 0; i < n; i++)
        cnt[a[i].key + 1]++;
    for (int k = 0; k < 16; k++)
        cnt[k + 1] += cnt[k];
    for (int i = 0; i < n; i++)
        o[cnt[a[i].key]++] = a[i];
    memcpy(a, o, sizeof(Rec) * (size_t)n);
    free(o);
}

static void s_gnome(Rec *a, int n) {
    for (int i = 0; i < n;) {
        if (i == 0 || a[i - 1].key <= a[i].key)
            i++;
        else {
            swp(&a[i], &a[i - 1]);
            i--;
        }
    }
}

int main(void) {
    static const struct {
        const char *name;
        void (*fn)(Rec *, int);
        int expect_stable;
    } algs[] = {{"insertion", s_insertion, 1}, {"bubble", s_bubble, 1},   {"gnome", s_gnome, 1},
                {"merge", s_merge, 1},         {"counting", s_counting, 1}, {"selection", s_selection, 0},
                {"shell", s_shell, 0},         {"heap", s_heap, 0},        {"quick", s_quick, 0}};
    enum { TRIALS = 60, N = 64 };
    for (unsigned k = 0; k < sizeof algs / sizeof algs[0]; k++) {
        int unstable_trials = 0, first_bad = -1, first_pair_key = -1;
        st = 5551212u;
        for (int t = 0; t < TRIALS; t++) {
            Rec a[N];
            int range = 2 + t % 14;
            for (int i = 0; i < N; i++) {
                a[i].key = (int)(rng() % (unsigned)range);
                a[i].seq = i;
            }
            algs[k].fn(a, N);
            int bad = 0;
            for (int i = 1; i < N; i++) {
                check(a[i - 1].key <= a[i].key, "sorted");
                if (a[i - 1].key == a[i].key && a[i - 1].seq > a[i].seq) {
                    if (!bad && first_bad < 0) {
                        first_bad = t;
                        first_pair_key = a[i].key;
                    }
                    bad = 1;
                }
            }
            unstable_trials += bad;
        }
        int stable = unstable_trials == 0;
        check(stable == algs[k].expect_stable, "stability matches theory");
        printf("%-10s stable=%-3s unstable_trials=%d/%d", algs[k].name, stable ? "yes" : "no", unstable_trials, TRIALS);
        if (!stable)
            printf(" first_bad_trial=%d key=%d", first_bad, first_pair_key);
        printf("\n");
    }
    return 0;
}
