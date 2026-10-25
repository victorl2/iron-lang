/*
 * title: Exact cover with Knuth's Algorithm X and dancing links
 * topic: algorithms
 * covers: exact cover, dancing links, circular doubly linked lists, column heuristic, domino tilings, sudoku as exact cover
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int *L, *R, *U, *D, *C, *S, *row;
    int ncols, nnodes, cap;
    long solutions, updates;
    int keep_first, first[128], first_len, cur[128];
} Dlx;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

/* Node 0 is the root, nodes 1..ncols are column headers. */
static void dlx_init(Dlx *d, int ncols, int cap) {
    memset(d, 0, sizeof *d);
    d->ncols = ncols;
    d->cap = cap + ncols + 1;
    d->L = malloc(sizeof(int) * (size_t)d->cap);
    d->R = malloc(sizeof(int) * (size_t)d->cap);
    d->U = malloc(sizeof(int) * (size_t)d->cap);
    d->D = malloc(sizeof(int) * (size_t)d->cap);
    d->C = malloc(sizeof(int) * (size_t)d->cap);
    d->S = calloc((size_t)d->cap, sizeof(int));
    d->row = malloc(sizeof(int) * (size_t)d->cap);
    for (int i = 0; i <= ncols; i++) {
        d->L[i] = i - 1;
        d->R[i] = i + 1;
        d->U[i] = d->D[i] = i;
        d->C[i] = i;
        d->row[i] = -1;
    }
    d->L[0] = ncols;
    d->R[ncols] = 0;
    d->nnodes = ncols + 1;
}

static void dlx_free(Dlx *d) {
    free(d->L);
    free(d->R);
    free(d->U);
    free(d->D);
    free(d->C);
    free(d->S);
    free(d->row);
}

static void dlx_add_row(Dlx *d, int row_id, const int *cols, int n) {
    int first = -1;
    for (int i = 0; i < n; i++) {
        int c = cols[i] + 1, x = d->nnodes++;
        check(x < d->cap, "node capacity");
        d->C[x] = c;
        d->row[x] = row_id;
        d->U[x] = d->U[c];
        d->D[x] = c;
        d->D[d->U[c]] = x;
        d->U[c] = x;
        d->S[c]++;
        if (first < 0) {
            first = x;
            d->L[x] = d->R[x] = x;
        } else {
            d->L[x] = d->L[first];
            d->R[x] = first;
            d->R[d->L[first]] = x;
            d->L[first] = x;
        }
    }
}

static void cover(Dlx *d, int c) {
    d->L[d->R[c]] = d->L[c];
    d->R[d->L[c]] = d->R[c];
    for (int i = d->D[c]; i != c; i = d->D[i])
        for (int j = d->R[i]; j != i; j = d->R[j]) {
            d->U[d->D[j]] = d->U[j];
            d->D[d->U[j]] = d->D[j];
            d->S[d->C[j]]--;
            d->updates++;
        }
}

static void uncover(Dlx *d, int c) {
    for (int i = d->U[c]; i != c; i = d->U[i])
        for (int j = d->L[i]; j != i; j = d->L[j]) {
            d->S[d->C[j]]++;
            d->U[d->D[j]] = j;
            d->D[d->U[j]] = j;
        }
    d->L[d->R[c]] = c;
    d->R[d->L[c]] = c;
}

static void search(Dlx *d, int depth) {
    if (d->R[0] == 0) {
        if (d->solutions == 0 && d->keep_first) {
            memcpy(d->first, d->cur, sizeof(int) * (size_t)depth);
            d->first_len = depth;
        }
        d->solutions++;
        return;
    }
    int c = d->R[0];
    for (int j = d->R[0]; j != 0; j = d->R[j]) /* smallest column first */
        if (d->S[j] < d->S[c])
            c = j;
    cover(d, c);
    for (int r = d->D[c]; r != c; r = d->D[r]) {
        d->cur[depth] = d->row[r];
        for (int j = d->R[r]; j != r; j = d->R[j])
            cover(d, d->C[j]);
        search(d, depth + 1);
        for (int j = d->L[r]; j != r; j = d->L[j])
            uncover(d, d->C[j]);
    }
    uncover(d, c);
}

static long domino_tilings(int h, int w, long *upd, int *first_rows) {
    Dlx d;
    dlx_init(&d, h * w, h * w * 4 * 2);
    int rid = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int cols[2];
            cols[0] = y * w + x;
            if (x + 1 < w) {
                cols[1] = y * w + x + 1;
                dlx_add_row(&d, rid++, cols, 2);
            }
            if (y + 1 < h) {
                cols[1] = (y + 1) * w + x;
                dlx_add_row(&d, rid++, cols, 2);
            }
        }
    d.keep_first = 1;
    search(&d, 0);
    long s = d.solutions;
    *upd = d.updates;
    if (first_rows)
        *first_rows = d.first_len;
    dlx_free(&d);
    return s;
}

/* Sudoku: 324 constraints (cell, row-digit, col-digit, box-digit), up to 729 candidate rows. */
static int sudoku(const char *puz, int *solution) {
    Dlx d;
    dlx_init(&d, 324, 729 * 4);
    static int meta[729][3];
    int rid = 0;
    for (int r = 0; r < 9; r++)
        for (int c = 0; c < 9; c++) {
            int given = (puz[r * 9 + c] >= '1' && puz[r * 9 + c] <= '9') ? puz[r * 9 + c] - '0' : 0;
            for (int v = 1; v <= 9; v++) {
                if (given && given != v)
                    continue;
                int cols[4] = {r * 9 + c, 81 + r * 9 + (v - 1), 162 + c * 9 + (v - 1),
                               243 + ((r / 3) * 3 + c / 3) * 9 + (v - 1)};
                meta[rid][0] = r;
                meta[rid][1] = c;
                meta[rid][2] = v;
                dlx_add_row(&d, rid++, cols, 4);
            }
        }
    d.keep_first = 1;
    search(&d, 0);
    int n = (int)d.solutions;
    if (n > 0)
        for (int i = 0; i < d.first_len; i++)
            solution[meta[d.first[i]][0] * 9 + meta[d.first[i]][1]] = meta[d.first[i]][2];
    dlx_free(&d);
    return n;
}

int main(void) {
    /* Knuth's example: items A..G, options given as column sets; unique cover is {B, D, F} */
    int opt[6][4] = {{2, 4, 5, -1}, {0, 3, 6, -1}, {1, 2, 5, -1}, {0, 3, -1, -1}, {1, 6, -1, -1}, {3, 4, 6, -1}};
    int lens[6] = {3, 3, 3, 2, 2, 3};
    Dlx d;
    dlx_init(&d, 7, 32);
    for (int i = 0; i < 6; i++)
        dlx_add_row(&d, i, opt[i], lens[i]);
    d.keep_first = 1;
    search(&d, 0);
    printf("knuth example: %ld solution(s), rows:", d.solutions);
    for (int i = 0; i < d.first_len; i++)
        printf(" %d", d.first[i]);
    printf("\n");
    check(d.solutions == 1 && d.first_len == 3, "knuth example");
    dlx_free(&d);

    int dims[][2] = {{2, 2}, {2, 5}, {4, 4}, {4, 5}, {4, 6}, {5, 6}, {6, 6}, {3, 8}};
    long expect[] = {2, 8, 36, 95, 281, 1183, 6728, 153};
    for (int i = 0; i < 8; i++) {
        long upd;
        int fr;
        long n = domino_tilings(dims[i][0], dims[i][1], &upd, &fr);
        check(n == expect[i], "known domino tiling count");
        printf("%dx%d dominoes: %5ld tilings, link updates=%ld\n", dims[i][0], dims[i][1], n, upd);
    }
    int sol[81] = {0};
    int n = sudoku("530070000600195000098000060800060003400803001700020006060000280000419005000080079", sol);
    printf("sudoku as exact cover: %d solution, row 1 = ", n);
    for (int c = 0; c < 9; c++)
        putchar('0' + sol[c]);
    printf(", row 9 = ");
    for (int c = 0; c < 9; c++)
        putchar('0' + sol[72 + c]);
    putchar('\n');
    check(n == 1 && sol[0] == 5 && sol[1] == 3 && sol[2] == 4, "sudoku solved");
    return 0;
}
