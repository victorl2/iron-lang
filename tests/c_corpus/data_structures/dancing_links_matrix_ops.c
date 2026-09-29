/*
 * title: Dancing links matrix with cover, uncover and row selection
 * topic: data_structures
 * covers: toroidal doubly linked matrix, reversible unlinking, LIFO undo, min-size column heuristic, brute-force model
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 271828u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define MAXR 16
#define MAXC 12
#define MAXN (1 + MAXC + MAXR * MAXC)

typedef struct {
    int L[MAXN], R[MAXN], U[MAXN], D[MAXN], C[MAXN], row[MAXN];
    int size[MAXC + 1];
    int nnodes, nc;
} DLX;

static void dlx_build(DLX *x, int nr, int nc, unsigned char m[MAXR][MAXC]) {
    x->nc = nc;
    for (int c = 0; c <= nc; c++) {
        x->L[c] = c == 0 ? nc : c - 1;
        x->R[c] = c == nc ? 0 : c + 1;
        x->U[c] = x->D[c] = c;
        x->C[c] = c;
        x->row[c] = -1;
        x->size[c] = 0;
    }
    int n = nc + 1;
    for (int r = 0; r < nr; r++) {
        int first = -1;
        for (int c = 0; c < nc; c++) {
            if (!m[r][c]) continue;
            int col = c + 1;
            x->C[n] = col;
            x->row[n] = r;
            x->U[n] = x->U[col];
            x->D[n] = col;
            x->D[x->U[col]] = n;
            x->U[col] = n;
            x->size[col]++;
            if (first < 0) { first = n; x->L[n] = x->R[n] = n; }
            else {
                x->L[n] = x->L[first];
                x->R[n] = first;
                x->R[x->L[first]] = n;
                x->L[first] = n;
            }
            n++;
        }
    }
    x->nnodes = n;
}

static void cover(DLX *x, int c) {
    x->R[x->L[c]] = x->R[c];
    x->L[x->R[c]] = x->L[c];
    for (int i = x->D[c]; i != c; i = x->D[i])
        for (int j = x->R[i]; j != i; j = x->R[j]) {
            x->D[x->U[j]] = x->D[j];
            x->U[x->D[j]] = x->U[j];
            x->size[x->C[j]]--;
        }
}
static void uncover(DLX *x, int c) {
    for (int i = x->U[c]; i != c; i = x->U[i])
        for (int j = x->L[i]; j != i; j = x->L[j]) {
            x->size[x->C[j]]++;
            x->D[x->U[j]] = j;
            x->U[x->D[j]] = j;
        }
    x->R[x->L[c]] = c;
    x->L[x->R[c]] = c;
}

/* one undo record: a list of covered columns, uncovered in reverse order */
typedef struct { int cols[MAXC]; int n; int is_row; int row; } Op;

static int model_row_active(unsigned char m[MAXR][MAXC], int r, unsigned covered) {
    for (int c = 0; c < MAXC; c++)
        if ((covered >> c & 1u) && m[r][c]) return 0;
    return 1;
}

static void check_against_model(DLX *x, int nr, int nc, unsigned char m[MAXR][MAXC], unsigned covered) {
    int c = x->R[0];
    int expected_c = 0;
    for (int k = 0; k < nc; k++) {
        if (covered >> k & 1u) continue;
        expected_c++;
        CHECK(c == k + 1);
        int cnt = 0, r_prev = -1;
        for (int i = x->D[c]; i != c; i = x->D[i]) {
            CHECK(x->C[i] == c);
            CHECK(x->row[i] > r_prev);
            r_prev = x->row[i];
            CHECK(model_row_active(m, x->row[i], covered));
            cnt++;
        }
        int expect = 0;
        for (int r = 0; r < nr; r++) if (m[r][k] && model_row_active(m, r, covered)) expect++;
        CHECK(cnt == expect && x->size[c] == expect);
        /* upward walk sees the same list */
        int cnt_up = 0;
        for (int i = x->U[c]; i != c; i = x->U[i]) cnt_up++;
        CHECK(cnt_up == cnt);
        c = x->R[c];
    }
    CHECK(c == 0);
    (void)expected_c;
}

static int min_col(DLX *x) {
    int best = 0, bs = 1 << 30;
    for (int c = x->R[0]; c != 0; c = x->R[c])
        if (x->size[c] < bs) { bs = x->size[c]; best = c; }
    return best;
}
static int model_min_col(int nr, int nc, unsigned char m[MAXR][MAXC], unsigned covered) {
    int best = 0, bs = 1 << 30;
    for (int k = 0; k < nc; k++) {
        if (covered >> k & 1u) continue;
        int cnt = 0;
        for (int r = 0; r < nr; r++) if (m[r][k] && model_row_active(m, r, covered)) cnt++;
        if (cnt < bs) { bs = cnt; best = k + 1; }
    }
    return best;
}

int main(void) {
    long total_ops = 0, selects = 0, covers = 0, unwinds = 0;
    for (int trial = 0; trial < 40; trial++) {
        int nr = 6 + (int)(rnd() % (MAXR - 6 + 1));
        int nc = 5 + (int)(rnd() % (MAXC - 5 + 1));
        int density = 20 + (int)(rnd() % 30);
        unsigned char m[MAXR][MAXC];
        for (int r = 0; r < nr; r++)
            for (int c = 0; c < nc; c++) m[r][c] = (int)(rnd() % 100) < density;
        DLX x;
        memset(&x, 0, sizeof x);
        dlx_build(&x, nr, nc, m);
        DLX snap = x;
        Op stack[64];
        int sp = 0;
        unsigned covered = 0;
        check_against_model(&x, nr, nc, m, covered);
        for (int step = 0; step < 200; step++) {
            unsigned op = rnd() % 10;
            if (op < 4) {
                int c = (int)(rnd() % (unsigned)nc);
                if ((covered >> c & 1u) || sp >= 64) continue;
                cover(&x, c + 1);
                covered |= 1u << c;
                stack[sp].n = 1; stack[sp].cols[0] = c + 1; stack[sp].is_row = 0; stack[sp].row = -1;
                sp++;
                covers++;
            } else if (op < 7) {
                /* select a random active row: cover all of its columns left to right */
                int r = (int)(rnd() % (unsigned)nr), tries = 0;
                while (tries++ < nr && !model_row_active(m, r, covered)) r = (r + 1) % nr;
                if (!model_row_active(m, r, covered) || sp >= 64) continue;
                int any = 0;
                for (int c = 0; c < nc; c++) any |= m[r][c];
                if (!any) continue;
                Op o;
                o.n = 0; o.is_row = 1; o.row = r;
                for (int c = 0; c < nc; c++) {
                    if (!m[r][c]) continue;
                    cover(&x, c + 1);
                    covered |= 1u << c;
                    o.cols[o.n++] = c + 1;
                }
                stack[sp++] = o;
                selects++;
            } else if (sp > 0) {
                Op o = stack[--sp];
                for (int k = o.n - 1; k >= 0; k--) {
                    uncover(&x, o.cols[k]);
                    covered &= ~(1u << (o.cols[k] - 1));
                }
                unwinds++;
            }
            total_ops++;
            check_against_model(&x, nr, nc, m, covered);
            if (x.R[0] != 0) CHECK(x.size[min_col(&x)] == x.size[model_min_col(nr, nc, m, covered)]);
        }
        while (sp > 0) {
            Op o = stack[--sp];
            for (int k = o.n - 1; k >= 0; k--) uncover(&x, o.cols[k]);
        }
        CHECK(memcmp(&x, &snap, sizeof x) == 0);
        if (trial % 8 == 0) {
            int ones = 0;
            for (int r = 0; r < nr; r++) for (int c = 0; c < nc; c++) ones += m[r][c];
            printf("trial %2d: %2dx%2d matrix, %3d ones, min column %d has %d rows\n", trial, nr, nc, ones, min_col(&x), x.size[min_col(&x)]);
        }
    }
    printf("ops=%ld covers=%ld row selects=%ld unwinds=%ld, every state matched the model and restored exactly\n", total_ops, covers, selects, unwinds);
    return 0;
}
