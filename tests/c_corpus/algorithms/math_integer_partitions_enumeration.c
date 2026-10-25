/*
 * title: Integer partition enumeration, conjugates and bijections
 * topic: algorithms
 * covers: partition generation in reverse lexicographic order, conjugate partitions, Ferrers diagrams, distinct vs odd parts, bounded parts, Durfee square
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 40

static long long counts_by_k[MAXN + 1][MAXN + 1];

typedef struct { int parts[MAXN]; int len; } Part;

/* next partition in reverse lex order; returns 0 after the last one (1 1 ... 1) */
static int next_partition(Part *p) {
    int *a = p->parts;
    int k = p->len;
    int i = k - 1;
    int rem = 0;
    while (i >= 0 && a[i] == 1) { rem++; i--; }
    if (i < 0) return 0;
    a[i]--;
    rem++;
    int v = a[i];
    k = i + 1;
    while (rem >= v) { a[k++] = v; rem -= v; }
    if (rem) a[k++] = rem;
    p->len = k;
    return 1;
}

static void conjugate(const Part *p, Part *c) {
    c->len = p->parts[0];
    for (int j = 0; j < c->len; j++) {
        int cnt = 0;
        for (int i = 0; i < p->len; i++) cnt += p->parts[i] > j;
        c->parts[j] = cnt;
    }
}

static int equal(const Part *a, const Part *b) {
    if (a->len != b->len) return 0;
    return memcmp(a->parts, b->parts, (size_t)a->len * sizeof(int)) == 0;
}

static void show(const Part *p) {
    for (int i = 0; i < p->len; i++) printf(i ? "+%d" : "%d", p->parts[i]);
}

static int durfee(const Part *p) {
    int d = 0;
    while (d < p->len && p->parts[d] >= d + 1) d++;
    return d;
}

static void ferrers(const Part *p) {
    for (int i = 0; i < p->len; i++) {
        printf("    ");
        for (int j = 0; j < p->parts[i]; j++) putchar('#');
        putchar('\n');
    }
}

int main(void) {
    Part p;
    int n = 7;
    p.len = 1;
    p.parts[0] = n;
    printf("partitions of 7:");
    int cnt = 0;
    do { printf(cnt % 5 == 0 ? "\n  " : "  "); show(&p); cnt++; } while (next_partition(&p));
    printf("\ncount %d\n", cnt);
    if (cnt != 15) return 1;

    /* counts, self-conjugate, distinct, odd-only, and number of parts stats for n = 1..30 */
    printf("n  p(n) selfconj distinct odd\n");
    long long pn_ref[] = {1, 1, 2, 3, 5, 7, 11, 15, 22, 30, 42, 56, 77, 101, 135, 176, 231, 297, 385, 490, 627, 792, 1002,
                          1255, 1575, 1958, 2436, 3010, 3718, 4565, 5604};
    for (n = 1; n <= 30; n++) {
        p.len = 1;
        p.parts[0] = n;
        long long total = 0, self = 0, distinct = 0, odd = 0, mx_len = 0;
        do {
            Part c;
            conjugate(&p, &c);
            if (equal(&p, &c)) self++;
            int d = 1, o = 1;
            for (int i = 0; i < p.len; i++) {
                if (i && p.parts[i] == p.parts[i - 1]) d = 0;
                if (p.parts[i] % 2 == 0) o = 0;
            }
            distinct += d;
            odd += o;
            /* conjugate of conjugate is the original */
            Part cc;
            conjugate(&c, &cc);
            if (!equal(&p, &cc)) { fprintf(stderr, "double conjugate\n"); return 1; }
            if (p.len > mx_len) mx_len = p.len;
            total++;
        } while (next_partition(&p));
        if (total != pn_ref[n]) { fprintf(stderr, "p(%d) wrong\n", n); return 1; }
        if (distinct != odd) { fprintf(stderr, "Euler bijection counts differ\n"); return 1; }
        if (n % 3 == 0 || n == 30) printf("%2d %5lld %5lld %5lld %5lld\n", n, total, self, distinct, odd);
    }
    /* partitions into at most k parts equals partitions with largest part <= k */
    n = 20;
    for (int i = 0; i <= MAXN; i++) for (int j = 0; j <= MAXN; j++) counts_by_k[i][j] = 0;
    p.len = 1; p.parts[0] = n;
    long long by_len[MAXN + 1] = {0}, by_max[MAXN + 1] = {0};
    do { by_len[p.len]++; by_max[p.parts[0]]++; } while (next_partition(&p));
    long long cl = 0, cm = 0;
    for (int k = 1; k <= 8; k++) { cl += by_len[k]; cm += by_max[k]; if (cl != cm) { fprintf(stderr, "at most k mismatch\n"); return 1; } }
    printf("partitions of 20 with at most 8 parts = largest part <= 8: %lld\n", cl);
    /* Durfee square distribution for n = 12 */
    n = 12;
    p.len = 1; p.parts[0] = n;
    long long dh[6] = {0};
    Part example = p;
    do { int d = durfee(&p); dh[d]++; if (d == 3 && example.len == 1) example = p; } while (next_partition(&p));
    printf("Durfee square sizes for n=12: d1=%lld d2=%lld d3=%lld\n", dh[1], dh[2], dh[3]);
    printf("first partition with Durfee 3: "); show(&example); printf("\n");
    ferrers(&example);
    Part c;
    conjugate(&example, &c);
    printf("  conjugate: "); show(&c); printf("\n");
    return 0;
}
