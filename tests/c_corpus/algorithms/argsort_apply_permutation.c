/*
 * title: Argsort, inverse permutation and in-place permutation application
 * topic: algorithms
 * covers: indirect sorting, rank arrays, inverse permutation, cycle decomposition, applying a permutation in place, parallel arrays
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned st = 112358u;
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

/* stable merge sort of indices by key */
static void argsort_rec(const int *key, int *idx, int *tmp, int n) {
    if (n < 2)
        return;
    int m = n / 2;
    argsort_rec(key, idx, tmp, m);
    argsort_rec(key, idx + m, tmp, n - m);
    int i = 0, j = m, k = 0;
    while (i < m && j < n)
        tmp[k++] = key[idx[j]] < key[idx[i]] ? idx[j++] : idx[i++];
    while (i < m)
        tmp[k++] = idx[i++];
    while (j < n)
        tmp[k++] = idx[j++];
    memcpy(idx, tmp, sizeof(int) * (size_t)n);
}

/* After: a[i] = old_a[perm[i]]. Follows cycles, destroys perm (marks visited by negation trick via copy). */
static int apply_perm_int(int *a, const int *perm, int n, int *cycles_out) {
    unsigned char *seen = calloc((size_t)n, 1);
    check(seen != NULL, "alloc");
    int cycles = 0, longest = 0;
    for (int s = 0; s < n; s++) {
        if (seen[s])
            continue;
        int len = 0, i = s, first = a[s];
        for (;;) {
            seen[i] = 1;
            len++;
            int src = perm[i];
            if (src == s) {
                a[i] = first;
                break;
            }
            a[i] = a[src];
            i = src;
        }
        cycles++;
        if (len > longest)
            longest = len;
    }
    free(seen);
    *cycles_out = cycles;
    return longest;
}

static void apply_perm_str(const char **a, const int *perm, int n) {
    const char **t = malloc(sizeof(char *) * (size_t)n);
    check(t != NULL, "alloc");
    for (int i = 0; i < n; i++)
        t[i] = a[perm[i]];
    memcpy(a, t, sizeof(char *) * (size_t)n);
    free(t);
}

int main(void) {
    enum { N = 24 };
    int key[N], val[N], idx[N], tmp[N], inv[N], rank[N];
    char names[N][4];
    const char *nm[N];
    for (int i = 0; i < N; i++) {
        key[i] = (int)(rng() % 12);
        val[i] = key[i] * 100 + i;
        snprintf(names[i], sizeof names[i], "%c%02d", 'a' + i % 26, i);
        nm[i] = names[i];
        idx[i] = i;
    }
    argsort_rec(key, idx, tmp, N);
    for (int i = 0; i < N; i++)
        inv[idx[i]] = i, rank[idx[i]] = i;
    for (int i = 0; i < N; i++) {
        check(inv[idx[i]] == i && idx[inv[i]] == i, "inverse permutation");
        if (i && key[idx[i - 1]] == key[idx[i]])
            check(idx[i - 1] < idx[i], "argsort stable");
        if (i)
            check(key[idx[i - 1]] <= key[idx[i]], "argsort ordered");
    }
    printf("argsort:");
    for (int i = 0; i < N; i++)
        printf(" %d", idx[i]);
    printf("\nranks  :");
    for (int i = 0; i < N; i++)
        printf(" %d", rank[i]);
    printf("\n");

    int keys2[N];
    memcpy(keys2, key, sizeof key);
    int c1, c2;
    int l1 = apply_perm_int(keys2, idx, N, &c1);
    int vals2[N];
    memcpy(vals2, val, sizeof val);
    apply_perm_int(vals2, idx, N, &c2);
    apply_perm_str(nm, idx, N);
    for (int i = 0; i < N; i++) {
        check(keys2[i] == key[idx[i]], "keys permuted");
        check(vals2[i] / 100 == keys2[i], "parallel array stays aligned");
        check(strcmp(nm[i], names[idx[i]]) == 0, "names permuted");
        if (i)
            check(keys2[i - 1] <= keys2[i], "sorted");
    }
    printf("cycles=%d longest=%d\n", c1, l1);
    printf("sorted keys:");
    for (int i = 0; i < N; i++)
        printf(" %d", keys2[i]);
    printf("\nnames:");
    for (int i = 0; i < 8; i++)
        printf(" %s", nm[i]);
    printf("\n");
    /* applying the inverse restores the original order */
    int back[N];
    memcpy(back, keys2, sizeof back);
    apply_perm_int(back, inv, N, &c2);
    check(memcmp(back, key, sizeof key) == 0, "inverse restores original");
    printf("round trip ok\n");
    return 0;
}
