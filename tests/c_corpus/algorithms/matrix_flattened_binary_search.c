/*
 * title: Binary search over a row-major sorted matrix
 * topic: algorithms
 * covers: index flattening, div/mod mapping, 2D as 1D binary search, row-then-column search, non-square shapes
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>

static unsigned st = 31337u;
static unsigned rnd(void) {
    st ^= st << 13;
    st ^= st >> 17;
    st ^= st << 5;
    return st;
}
static void fail(const char *m) {
    fprintf(stderr, "FAIL: %s\n", m);
    exit(1);
}

typedef struct {
    int rows, cols;
    int *cell;
} Matrix;

static int at(const Matrix *m, int r, int c) { return m->cell[r * m->cols + c]; }

/* whole matrix as one sorted array of rows*cols elements */
static int flat_search(const Matrix *m, int key, int *pr, int *pc) {
    int lo = 0, hi = m->rows * m->cols - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int v = at(m, mid / m->cols, mid % m->cols);
        if (v == key) {
            *pr = mid / m->cols;
            *pc = mid % m->cols;
            return 1;
        }
        if (v < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* first pick the row by binary search on last column, then search inside the row */
static int two_phase_search(const Matrix *m, int key) {
    int lo = 0, hi = m->rows - 1, row = -1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (at(m, mid, m->cols - 1) < key)
            lo = mid + 1;
        else {
            row = mid;
            hi = mid - 1;
        }
    }
    if (row < 0)
        return 0;
    lo = 0;
    hi = m->cols - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int v = at(m, row, mid);
        if (v == key)
            return 1;
        if (v < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

int main(void) {
    int shapes[][2] = {{1, 1}, {1, 9}, {9, 1}, {4, 4}, {3, 7}, {7, 3}, {16, 25}};
    for (int s = 0; s < 7; s++) {
        Matrix m = {shapes[s][0], shapes[s][1], NULL};
        int n = m.rows * m.cols;
        m.cell = malloc((size_t)n * sizeof(int));
        if (!m.cell)
            fail("alloc");
        int v = 0;
        for (int i = 0; i < n; i++) {
            v += 1 + (int)(rnd() % 4);
            m.cell[i] = v;
        }
        int hits = 0, sumr = 0, sumc = 0;
        for (int k = 0; k <= v + 2; k++) {
            int r = -1, c = -1;
            int f = flat_search(&m, k, &r, &c);
            int want = 0;
            for (int i = 0; i < n; i++)
                if (m.cell[i] == k)
                    want = 1;
            if (f != want || two_phase_search(&m, k) != want)
                fail("search mismatch");
            if (f) {
                if (at(&m, r, c) != k)
                    fail("position");
                hits++;
                sumr += r;
                sumc += c;
            }
        }
        printf("%2dx%-2d max=%-4d hits=%-3d sum_row=%-5d sum_col=%d\n", m.rows, m.cols, v, hits,
               sumr, sumc);
        free(m.cell);
    }
    return 0;
}
