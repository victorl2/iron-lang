/*
 * title: Hungarian algorithm for assignment
 * topic: algorithms
 * covers: Hungarian method, dual potentials, min-cost perfect matching, brute-force permutation check, rectangular case
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

enum { MAXN = 12, INF = 1 << 28 };

static unsigned st = 5772156u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void check(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "check failed: %s\n", m);
        exit(1);
    }
}

/* rows n <= cols m, 1-indexed arrays as in the classic e-maxx formulation */
static long hungarian(int n, int m, int a[MAXN + 1][MAXN + 1], int *assign_row) {
    int u[MAXN + 1] = {0}, v[MAXN + 1] = {0}, p[MAXN + 1] = {0}, way[MAXN + 1] = {0};
    for (int i = 1; i <= n; i++) {
        p[0] = i;
        int j0 = 0, minv[MAXN + 1];
        char used[MAXN + 1];
        for (int j = 0; j <= m; j++) minv[j] = INF, used[j] = 0;
        do {
            used[j0] = 1;
            int i0 = p[j0], delta = INF, j1 = 0;
            for (int j = 1; j <= m; j++)
                if (!used[j]) {
                    int cur = a[i0][j] - u[i0] - v[j];
                    if (cur < minv[j]) minv[j] = cur, way[j] = j0;
                    if (minv[j] < delta) delta = minv[j], j1 = j;
                }
            for (int j = 0; j <= m; j++) {
                if (used[j]) u[p[j]] += delta, v[j] -= delta;
                else minv[j] -= delta;
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }
    long cost = 0;
    for (int j = 1; j <= m; j++)
        if (p[j]) assign_row[p[j]] = j, cost += a[p[j]][j];
    /* dual feasibility and complementary slackness */
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++) check(a[i][j] - u[i] - v[j] >= 0, "dual feasible");
    for (int i = 1; i <= n; i++) check(a[i][assign_row[i]] - u[i] - v[assign_row[i]] == 0, "tight on assignment");
    return cost;
}

static int A[MAXN + 1][MAXN + 1];
static long best_brute;
static void brute(int n, int m, int row, unsigned used, long acc) {
    if (row > n) {
        if (acc < best_brute) best_brute = acc;
        return;
    }
    for (int j = 1; j <= m; j++)
        if (!(used >> j & 1u)) brute(n, m, row + 1, used | 1u << j, acc + A[row][j]);
}

int main(void) {
    int shapes[6][2] = {{4, 4}, {6, 6}, {7, 7}, {8, 8}, {5, 8}, {3, 9}};
    for (int s = 0; s < 6; s++) {
        int n = shapes[s][0], m = shapes[s][1];
        for (int i = 1; i <= n; i++)
            for (int j = 1; j <= m; j++) A[i][j] = 1 + (int)(rnd() % 99);
        int asg[MAXN + 1];
        long c = hungarian(n, m, A, asg);
        best_brute = 1L << 40;
        brute(n, m, 1, 0, 0);
        check(c == best_brute, "hungarian equals brute force");
        unsigned used = 0;
        for (int i = 1; i <= n; i++) {
            check(!(used >> asg[i] & 1u), "columns distinct");
            used |= 1u << asg[i];
        }
        printf("%dx%d: cost %ld, assignment", n, m, c);
        for (int i = 1; i <= n; i++) printf(" %d", asg[i]);
        printf("\n");
    }
    /* a matrix where identity is optimal by construction */
    for (int i = 1; i <= 10; i++)
        for (int j = 1; j <= 10; j++) A[i][j] = i == j ? 0 : 50 + (int)(rnd() % 50);
    int asg[MAXN + 1];
    long c = hungarian(10, 10, A, asg);
    for (int i = 1; i <= 10; i++) check(asg[i] == i, "diagonal chosen");
    printf("diagonal-optimal 10x10: cost %ld\n", c);
    return 0;
}
