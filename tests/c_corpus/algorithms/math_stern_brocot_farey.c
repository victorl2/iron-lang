/*
 * title: Stern-Brocot tree and Farey sequences
 * topic: algorithms
 * covers: mediants, Stern-Brocot descent paths, Farey neighbors, next-term recurrence, rational approximation, path encoding
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef long long i64;

static i64 gcd(i64 a, i64 b) { while (b) { i64 t = a % b; a = b; b = t; } return a; }

/* Farey sequence F_n, next term recurrence starting from 0/1, 1/n */
static int farey(int n, int (*out)[2], int cap) {
    int a = 0, b = 1, c = 1, d = n, cnt = 0;
    out[cnt][0] = a; out[cnt][1] = b; cnt++;
    while (c <= n && cnt < cap) {
        out[cnt][0] = c; out[cnt][1] = d; cnt++;
        int k = (n + b) / d;
        int e = k * c - a, f = k * d - b;
        a = c; b = d; c = e; d = f;
        if (a == 1 && b == 1) break;
    }
    return cnt;
}

/* path to p/q in the Stern-Brocot tree as an L/R string, run-length pairs */
static int sb_path(i64 p, i64 q, char *path, int cap) {
    i64 ln = 0, ld = 1, rn = 1, rd = 0;
    int len = 0;
    for (;;) {
        i64 mn = ln + rn, md = ld + rd;
        if (mn == p && md == q) break;
        if (len + 1 >= cap) return -1;
        if (p * md < mn * q) { path[len++] = 'L'; rn = mn; rd = md; }
        else { path[len++] = 'R'; ln = mn; ld = md; }
    }
    path[len] = 0;
    return len;
}

static void sb_follow(const char *path, i64 *p, i64 *q) {
    i64 ln = 0, ld = 1, rn = 1, rd = 0;
    for (const char *c = path; *c; c++) {
        i64 mn = ln + rn, md = ld + rd;
        if (*c == 'L') { rn = mn; rd = md; } else { ln = mn; ld = md; }
    }
    *p = ln + rn;
    *q = ld + rd;
}

static long long phi_sum(int n) {
    long long s = 1; /* 0/1 */
    for (int k = 1; k <= n; k++) {
        int c = 0;
        for (int j = 1; j <= k; j++) c += gcd(j, k) == 1;
        s += c;
    }
    return s;
}

int main(void) {
    static int seq[2000][2];
    int n = 5;
    int cnt = farey(n, seq, 2000);
    printf("F_5:");
    for (int i = 0; i < cnt; i++) printf(" %d/%d", seq[i][0], seq[i][1]);
    printf("\n");
    for (n = 1; n <= 40; n++) {
        cnt = farey(n, seq, 2000);
        if ((long long)cnt != phi_sum(n)) { fprintf(stderr, "length wrong\n"); return 1; }
        for (int i = 1; i < cnt; i++) {
            /* neighbors: bc - ad = 1 */
            i64 det = (i64)seq[i][0] * seq[i - 1][1] - (i64)seq[i - 1][0] * seq[i][1];
            if (det != 1) { fprintf(stderr, "not neighbors n=%d i=%d\n", n, i); return 1; }
            if (gcd(seq[i][0], seq[i][1]) != 1 || seq[i][1] > n) return 1;
        }
        if (n == 1 || n == 10 || n == 20 || n == 40) printf("|F_%d| = %d, middle term %d/%d\n", n, cnt, seq[cnt / 2][0], seq[cnt / 2][1]);
    }
    /* mediant property: in F_n the term between neighbors a/b, c/d with b+d > n is their mediant in F_{b+d} */
    n = 7;
    cnt = farey(n, seq, 2000);
    int new_terms = 0;
    for (int i = 1; i < cnt; i++) if (seq[i - 1][1] + seq[i][1] == n + 1) new_terms++;
    printf("F_7 -> F_8 inserts %d mediants (phi(8)=4): %s\n", new_terms, new_terms == 4 ? "ok" : "BAD");
    if (new_terms != 4) return 1;

    struct { i64 p, q; } fr[] = {{1, 1}, {1, 2}, {2, 3}, {3, 7}, {5, 3}, {7, 5}, {355, 113}, {13, 8}, {1, 12}, {89, 55}};
    for (size_t i = 0; i < sizeof fr / sizeof fr[0]; i++) {
        char path[512];
        int len = sb_path(fr[i].p, fr[i].q, path, sizeof path);
        i64 p, q;
        sb_follow(path, &p, &q);
        if (p != fr[i].p || q != fr[i].q) { fprintf(stderr, "path round trip failed\n"); return 1; }
        /* run-length encode the path: it matches the continued fraction terms */
        printf("%lld/%lld: ", fr[i].p, fr[i].q);
        if (len == 0) printf("(root)");
        for (int k = 0; k < len;) {
            int j = k;
            while (j < len && path[j] == path[k]) j++;
            printf("%c%d", path[k], j - k);
            k = j;
        }
        printf("  depth %d\n", len);
    }
    /* every reduced fraction with p,q <= 30 appears exactly once with the right inverse symmetry */
    int total = 0, sym_ok = 1;
    for (i64 p = 1; p <= 30; p++) for (i64 q = 1; q <= 30; q++) {
        if (gcd(p, q) != 1) continue;
        char a[512], b[512];
        int la = sb_path(p, q, a, sizeof a), lb = sb_path(q, p, b, sizeof b);
        if (la != lb) sym_ok = 0;
        for (int k = 0; k < la; k++) if ((a[k] == 'L') == (b[k] == 'L')) sym_ok = 0;
        total++;
    }
    printf("coprime pairs up to 30: %d, reciprocal paths mirror L/R: %s\n", total, sym_ok ? "yes" : "no");
    return sym_ok ? 0 : 1;
}
