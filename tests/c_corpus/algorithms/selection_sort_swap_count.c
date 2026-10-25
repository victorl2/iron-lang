/*
 * title: Selection sort, swaps and stability loss
 * topic: algorithms
 * covers: selection sort, minimal swaps, unstable behaviour, stable rotation variant, double-ended selection
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int key;
    char tag;
} Item;

static unsigned st = 99991u;
static unsigned rng(void) {
    st = st * 1664525u + 1013904223u;
    return st >> 8;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static int selection(Item *a, int n) {
    int swaps = 0;
    for (int i = 0; i + 1 < n; i++) {
        int m = i;
        for (int j = i + 1; j < n; j++)
            if (a[j].key < a[m].key)
                m = j;
        if (m != i) {
            Item t = a[i];
            a[i] = a[m];
            a[m] = t;
            swaps++;
        }
    }
    return swaps;
}

/* Stable variant: rotate the minimum into place instead of swapping. */
static long selection_stable(Item *a, int n) {
    long moves = 0;
    for (int i = 0; i + 1 < n; i++) {
        int m = i;
        for (int j = i + 1; j < n; j++)
            if (a[j].key < a[m].key)
                m = j;
        Item t = a[m];
        for (int j = m; j > i; j--) {
            a[j] = a[j - 1];
            moves++;
        }
        a[i] = t;
    }
    return moves;
}

/* Double-ended: pick min and max each round. */
static int double_selection(Item *a, int n) {
    int swaps = 0;
    for (int lo = 0, hi = n - 1; lo < hi; lo++, hi--) {
        int mn = lo, mx = lo;
        for (int j = lo; j <= hi; j++) {
            if (a[j].key < a[mn].key)
                mn = j;
            if (a[j].key > a[mx].key)
                mx = j;
        }
        Item t = a[lo];
        a[lo] = a[mn];
        a[mn] = t;
        swaps++;
        if (mx == lo)
            mx = mn;
        t = a[hi];
        a[hi] = a[mx];
        a[mx] = t;
        swaps++;
    }
    return swaps;
}

static int is_stable(const Item *orig, const Item *a, int n) {
    /* tags are assigned increasing along the original order */
    for (int i = 1; i < n; i++)
        if (a[i - 1].key == a[i].key && a[i - 1].tag > a[i].tag)
            return 0;
    (void)orig;
    return 1;
}

int main(void) {
    enum { N = 26 };
    Item o[N], a[N], b[N], c[N];
    for (int i = 0; i < N; i++) {
        o[i].key = (int)(rng() % 6);
        o[i].tag = (char)('a' + i);
    }
    for (int i = 0; i < N; i++)
        a[i] = b[i] = c[i] = o[i];

    int s1 = selection(a, N);
    long m2 = selection_stable(b, N);
    int s3 = double_selection(c, N);
    for (int i = 1; i < N; i++) {
        check(a[i - 1].key <= a[i].key, "a sorted");
        check(b[i - 1].key <= b[i].key, "b sorted");
        check(c[i - 1].key <= c[i].key, "c sorted");
    }
    check(s1 <= N - 1, "at most n-1 swaps");
    check(is_stable(o, b, N), "rotation variant stable");

    printf("swap selection: swaps=%d stable=%d\n", s1, is_stable(o, a, N));
    printf("rotate selection: moves=%ld stable=%d\n", m2, is_stable(o, b, N));
    printf("double selection: swaps=%d stable=%d\n", s3, is_stable(o, c, N));
    printf("swap order  :");
    for (int i = 0; i < N; i++)
        printf(" %d%c", a[i].key, a[i].tag);
    printf("\nstable order:");
    for (int i = 0; i < N; i++)
        printf(" %d%c", b[i].key, b[i].tag);
    printf("\n");

    Item sorted[8];
    for (int i = 0; i < 8; i++) {
        sorted[i].key = i;
        sorted[i].tag = 'x';
    }
    check(selection(sorted, 8) == 0, "no swaps on sorted input");
    return 0;
}
