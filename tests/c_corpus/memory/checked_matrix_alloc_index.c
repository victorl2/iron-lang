/*
 * title: Overflow-checked matrix allocation with checked indexing
 * topic: memory
 * covers: 2D dimension products, single-block row-major layout, index checks, checked multiply, submatrix views, transpose
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { double *d; size_t rows, cols; } Mat;
typedef struct { const Mat *m; size_t r0, c0, rows, cols; } MView;

enum { M_OK, M_ZERO, M_OVERFLOW, M_TOOBIG, M_NOMEM, M_INDEX };
static const char *mn[] = {"ok", "zero-dim", "overflow", "too-big", "no-memory", "index"};

#define MAX_BYTES ((size_t)1 << 24)

static int mat_new(Mat *m, size_t rows, size_t cols) {
    m->d = NULL; m->rows = m->cols = 0;
    if (rows == 0 || cols == 0) return M_ZERO;
    if (rows > SIZE_MAX / cols) return M_OVERFLOW;
    size_t cells = rows * cols;
    if (cells > SIZE_MAX / sizeof(double)) return M_OVERFLOW;
    size_t bytes = cells * sizeof(double);
    if (bytes > MAX_BYTES) return M_TOOBIG;
    m->d = calloc(cells, sizeof(double));
    if (!m->d) return M_NOMEM;
    m->rows = rows; m->cols = cols;
    return M_OK;
}
static void mat_free(Mat *m) { free(m->d); m->d = NULL; m->rows = m->cols = 0; }

static int mat_get(const Mat *m, size_t r, size_t c, double *out) {
    if (r >= m->rows || c >= m->cols) return M_INDEX;
    *out = m->d[r * m->cols + c];
    return M_OK;
}
static int mat_set(Mat *m, size_t r, size_t c, double v) {
    if (r >= m->rows || c >= m->cols) return M_INDEX;
    m->d[r * m->cols + c] = v;
    return M_OK;
}

static int view_make(const Mat *m, size_t r0, size_t c0, size_t rows, size_t cols, MView *v) {
    if (r0 > m->rows || rows > m->rows - r0 || c0 > m->cols || cols > m->cols - c0) return M_INDEX;
    v->m = m; v->r0 = r0; v->c0 = c0; v->rows = rows; v->cols = cols;
    return M_OK;
}
static double view_sum(const MView *v) {
    double s = 0;
    for (size_t r = 0; r < v->rows; r++) for (size_t c = 0; c < v->cols; c++) s += v->m->d[(v->r0 + r) * v->m->cols + v->c0 + c];
    return s;
}

static int mat_transpose(const Mat *a, Mat *out) {
    int e = mat_new(out, a->cols, a->rows);
    if (e) return e;
    for (size_t r = 0; r < a->rows; r++) for (size_t c = 0; c < a->cols; c++) out->d[c * out->cols + r] = a->d[r * a->cols + c];
    return M_OK;
}

int main(void) {
    struct { size_t r, c; } dims[] = {
        {3, 4}, {0, 5}, {5, 0}, {1000, 1000}, {2048, 2048},
        {SIZE_MAX, 2}, {(size_t)1 << 32, (size_t)1 << 32}, {SIZE_MAX / 8 + 1, 1}, {1, SIZE_MAX / 8}, {1, 1},
    };
    for (size_t i = 0; i < sizeof dims / sizeof dims[0]; i++) {
        Mat m;
        int e = mat_new(&m, dims[i].r, dims[i].c);
        printf("new(%zu x %zu): %s\n", dims[i].r > 100000 ? (size_t)99999999 : dims[i].r, dims[i].c > 100000 ? (size_t)99999999 : dims[i].c, mn[e]);
        mat_free(&m);
    }

    Mat a, t;
    if (mat_new(&a, 4, 6)) return 1;
    for (size_t r = 0; r < 4; r++) for (size_t c = 0; c < 6; c++) mat_set(&a, r, c, (double)(r * 10 + c));
    double v;
    printf("get(3,5)=%d, get(4,0)=%d, get(0,6)=%d, get(SIZE_MAX,0)=%d\n",
           mat_get(&a, 3, 5, &v), mat_get(&a, 4, 0, &v), mat_get(&a, 0, 6, &v), mat_get(&a, SIZE_MAX, 0, &v));
    mat_get(&a, 3, 5, &v);
    printf("a[3][5]=%.0f\n", v);

    MView mv;
    printf("view(1,2,2,3): %s", mn[view_make(&a, 1, 2, 2, 3, &mv)]);
    printf(" sum=%.0f\n", view_sum(&mv));
    printf("view(3,0,2,1): %s\n", mn[view_make(&a, 3, 0, 2, 1, &mv)]);
    printf("view(SIZE_MAX,0,1,1): %s\n", mn[view_make(&a, SIZE_MAX, 0, 1, 1, &mv)]);
    printf("view(0,0,4,6): %s", mn[view_make(&a, 0, 0, 4, 6, &mv)]);
    printf(" sum=%.0f\n", view_sum(&mv));

    if (mat_transpose(&a, &t)) return 1;
    double x, y;
    int ok = 1;
    for (size_t r = 0; r < 4; r++) for (size_t c = 0; c < 6; c++) { mat_get(&a, r, c, &x); mat_get(&t, c, r, &y); ok &= x == y; }
    printf("transpose %zux%zu -> %zux%zu consistent=%d\n", a.rows, a.cols, t.rows, t.cols, ok);
    mat_free(&a); mat_free(&t);
    return ok ? 0 : 1;
}
