/*
 * title: Bucket sort with per-bucket linked lists
 * topic: algorithms
 * covers: bucket sort, uniform vs skewed distributions, insertion into sorted chain, bucket occupancy statistics
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Node {
    unsigned v;
    struct Node *next;
} Node;

static unsigned long long st = 0x123456789ABCDEFULL;
static unsigned rng(void) {
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned)(st >> 33);
}

static void check(int c, const char *w) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", w);
        exit(1);
    }
}

static long chain_steps;

static void insert_sorted(Node **head, Node *x) {
    while (*head && (*head)->v <= x->v) { /* <= keeps insertion order for equal keys */
        head = &(*head)->next;
        chain_steps++;
    }
    x->next = *head;
    *head = x;
}

/* keys in [0, range); nb buckets */
static void bucket_sort(unsigned *a, int n, unsigned range, int nb, int *max_bucket, int *empty) {
    Node **b = calloc((size_t)nb, sizeof *b);
    Node *pool = malloc(sizeof(Node) * (size_t)n);
    int *cnt = calloc((size_t)nb, sizeof(int));
    check(b && pool && cnt, "alloc");
    for (int i = 0; i < n; i++) {
        int k = (int)((unsigned long long)a[i] * (unsigned)nb / range);
        pool[i].v = a[i];
        insert_sorted(&b[k], &pool[i]);
        cnt[k]++;
    }
    int idx = 0;
    *max_bucket = 0;
    *empty = 0;
    for (int k = 0; k < nb; k++) {
        if (cnt[k] > *max_bucket)
            *max_bucket = cnt[k];
        if (!cnt[k])
            (*empty)++;
        for (Node *p = b[k]; p; p = p->next)
            a[idx++] = p->v;
    }
    check(idx == n, "all emitted");
    free(b);
    free(pool);
    free(cnt);
}

int main(void) {
    enum { N = 4000, R = 1000000 };
    static unsigned a[N], ref[N];
    static const char *dist[] = {"uniform", "squared (skewed)", "clustered", "two values"};
    for (int d = 0; d < 4; d++) {
        for (int i = 0; i < N; i++) {
            unsigned u = rng() % R;
            switch (d) {
            case 0: a[i] = u; break;
            case 1: a[i] = (unsigned)((unsigned long long)u * u / R); break;
            case 2: a[i] = 500000 + u % 2000; break;
            default: a[i] = (u & 1) ? 999999 : 0; break;
            }
        }
        memcpy(ref, a, sizeof a);
        chain_steps = 0;
        int mx, em;
        bucket_sort(a, N, R, N / 4, &mx, &em);
        for (int i = 1; i < N; i++)
            check(a[i - 1] <= a[i], "sorted");
        /* cross-check with counting of a few order statistics via brute force rank */
        for (int probe = 0; probe < 5; probe++) {
            int i = (int)(rng() % N);
            int rank = 0;
            for (int j = 0; j < N; j++)
                rank += ref[j] < ref[i];
            check(a[rank] == ref[i], "rank matches");
        }
        printf("%-17s buckets=%d max_bucket=%-4d empty=%-4d chain_steps=%ld\n", dist[d], N / 4, mx, em,
               chain_steps);
    }
    return 0;
}
