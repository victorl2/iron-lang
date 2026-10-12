/*
 * title: Timsort-lite with run stack and minrun
 * topic: algorithms
 * covers: timsort, natural run detection, descending run reversal, minrun computation, run stack invariants, stable merge
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int key;
    int seq;
} Rec;

static unsigned st = 1231231u;
static unsigned rng(void) {
    st = st * 1664525u + 1013904223u;
    return st >> 10;
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

typedef struct {
    int start, len;
} Run;

static long cmps;
static int merges, max_stack;

static int lt(const Rec *a, const Rec *b) {
    cmps++;
    return a->key < b->key;
}

static int compute_minrun(int n) {
    int r = 0;
    while (n >= 32) {
        r |= n & 1;
        n >>= 1;
    }
    return n + r;
}

static void reverse(Rec *a, int n) {
    for (int i = 0, j = n - 1; i < j; i++, j--) {
        Rec t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static int count_run(Rec *a, int n) {
    if (n < 2)
        return n;
    int i = 2;
    if (lt(&a[1], &a[0])) { /* strictly descending only, to keep stability */
        while (i < n && lt(&a[i], &a[i - 1]))
            i++;
        reverse(a, i);
    } else {
        while (i < n && !lt(&a[i], &a[i - 1]))
            i++;
    }
    return i;
}

static void binary_insertion(Rec *a, int n, int sorted_prefix) {
    for (int i = sorted_prefix; i < n; i++) {
        Rec x = a[i];
        int lo = 0, hi = i;
        while (lo < hi) {
            int m = (lo + hi) / 2;
            if (lt(&x, &a[m]))
                hi = m;
            else
                lo = m + 1;
        }
        memmove(a + lo + 1, a + lo, sizeof(Rec) * (size_t)(i - lo));
        a[lo] = x;
    }
}

static void merge_runs(Rec *a, Rec *tmp, Run *x, Run *y) {
    /* y directly follows x; copy the left run out and merge */
    memcpy(tmp, a + x->start, sizeof(Rec) * (size_t)x->len);
    int i = 0, j = y->start, k = x->start, jend = y->start + y->len;
    while (i < x->len && j < jend) {
        if (lt(&a[j], &tmp[i]))
            a[k++] = a[j++];
        else
            a[k++] = tmp[i++];
    }
    while (i < x->len)
        a[k++] = tmp[i++];
    x->len += y->len;
    merges++;
}

static void collapse(Rec *a, Rec *tmp, Run *stack, int *sp, int force) {
    while (*sp > 1) {
        int n = *sp - 2;
        if (n > 0 && stack[n - 1].len <= stack[n].len + stack[n + 1].len) {
            if (stack[n - 1].len < stack[n + 1].len)
                n--;
        } else if (stack[n].len <= stack[n + 1].len) {
            /* merge n and n+1 */
        } else if (!force) {
            break;
        }
        merge_runs(a, tmp, &stack[n], &stack[n + 1]);
        for (int k = n + 1; k + 1 < *sp; k++)
            stack[k] = stack[k + 1];
        (*sp)--;
    }
}

static void timsort(Rec *a, int n) {
    Rec *tmp = malloc(sizeof(Rec) * (size_t)(n + 1));
    Run stack[64];
    int sp = 0, pos = 0;
    check(tmp != NULL, "alloc");
    int minrun = compute_minrun(n);
    while (pos < n) {
        int len = count_run(a + pos, n - pos);
        if (len < minrun) {
            int force = n - pos < minrun ? n - pos : minrun;
            binary_insertion(a + pos, force, len);
            len = force;
        }
        stack[sp].start = pos;
        stack[sp].len = len;
        sp++;
        if (sp > max_stack)
            max_stack = sp;
        pos += len;
        collapse(a, tmp, stack, &sp, 0);
    }
    collapse(a, tmp, stack, &sp, 1);
    check(sp <= 1, "one run left");
    free(tmp);
}

int main(void) {
    static Rec a[6000];
    static const char *shapes[] = {"random", "sorted", "reversed", "sawtooth", "few-runs", "dups"};
    static const int n_of[] = {5000, 5000, 5000, 5000, 5000, 5000};
    printf("minrun(64)=%d minrun(65)=%d minrun(1000)=%d minrun(5000)=%d\n", compute_minrun(64), compute_minrun(65),
           compute_minrun(1000), compute_minrun(5000));
    for (int s = 0; s < 6; s++) {
        int n = n_of[s];
        for (int i = 0; i < n; i++) {
            switch (s) {
            case 0: a[i].key = (int)(rng() % 100000); break;
            case 1: a[i].key = i; break;
            case 2: a[i].key = n - i; break;
            case 3: a[i].key = i % 100; break;
            case 4: a[i].key = (i / 1000) * 1000 + (int)(rng() % 1000) * 0 + (i % 1000) * ((i / 1000) % 2 ? -1 : 1); break;
            default: a[i].key = (int)(rng() % 3); break;
            }
            a[i].seq = i;
        }
        cmps = 0;
        merges = 0;
        max_stack = 0;
        timsort(a, n);
        for (int i = 1; i < n; i++) {
            check(a[i - 1].key <= a[i].key, "sorted");
            if (a[i - 1].key == a[i].key)
                check(a[i - 1].seq < a[i].seq, "stable");
        }
        printf("%-9s n=%d cmps=%-7ld merges=%-3d max_stack=%d\n", shapes[s], n, cmps, merges, max_stack);
    }
    return 0;
}
