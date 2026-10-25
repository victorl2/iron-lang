/*
 * title: N-dimensional array views with strides, slicing, transpose and broadcast
 * topic: data_structures
 * covers: shape and stride metadata, zero-copy views, negative and zero strides, contiguity test, reshape rules, axis reduction, odometer iteration
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 2357u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

#define MAXD 4
typedef struct { const int *data; int nd; long shape[MAXD], stride[MAXD], off; } View;

static long numel(const View *v) { long n = 1; for (int i = 0; i < v->nd; i++) n *= v->shape[i]; return n; }
static View v_base(const int *data, int nd, const long *shape) {
    View v;
    v.data = data; v.nd = nd; v.off = 0;
    long s = 1;
    for (int i = nd - 1; i >= 0; i--) { v.shape[i] = shape[i]; v.stride[i] = s; s *= shape[i]; }
    return v;
}
static int v_get(const View *v, const long *ix) {
    long p = v->off;
    for (int i = 0; i < v->nd; i++) p += ix[i] * v->stride[i];
    return v->data[p];
}
static View v_transpose(View v, const int *perm) {
    View r = v;
    for (int i = 0; i < v.nd; i++) { r.shape[i] = v.shape[perm[i]]; r.stride[i] = v.stride[perm[i]]; }
    return r;
}
/* elements start, start+step, ... (len of them) along one axis */
static View v_slice(View v, int axis, long start, long step, long len) {
    v.off += start * v.stride[axis];
    v.stride[axis] *= step;
    v.shape[axis] = len;
    return v;
}
static View v_index(View v, int axis, long i) {
    v.off += i * v.stride[axis];
    for (int k = axis; k + 1 < v.nd; k++) { v.shape[k] = v.shape[k + 1]; v.stride[k] = v.stride[k + 1]; }
    v.nd--;
    return v;
}
static View v_expand(View v, int axis) {
    for (int k = v.nd; k > axis; k--) { v.shape[k] = v.shape[k - 1]; v.stride[k] = v.stride[k - 1]; }
    v.shape[axis] = 1; v.stride[axis] = 0;
    v.nd++;
    return v;
}
static View v_broadcast_axis(View v, int axis, long n) {
    CHECK(v.shape[axis] == 1);
    v.shape[axis] = n; v.stride[axis] = 0;
    return v;
}
static int v_contiguous(const View *v) {
    long expect = 1;
    for (int i = v->nd - 1; i >= 0; i--) {
        if (v->shape[i] == 1) continue;
        if (v->stride[i] != expect) return 0;
        expect *= v->shape[i];
    }
    return 1;
}
/* reshape without copying is only possible for contiguous views */
static int v_reshape(View *v, int nd, const long *shape) {
    long n = 1;
    for (int i = 0; i < nd; i++) n *= shape[i];
    if (n != numel(v) || !v_contiguous(v)) return 0;
    long s = 1;
    for (int i = nd - 1; i >= 0; i--) { v->shape[i] = shape[i]; v->stride[i] = s; s *= shape[i]; }
    v->nd = nd;
    return 1;
}
/* row-major walk with an odometer */
static long v_to_array(const View *v, int *out) {
    long n = numel(v), k = 0;
    if (n == 0) return 0;
    long ix[MAXD] = { 0 };
    for (;;) {
        out[k++] = v_get(v, ix);
        int d = v->nd - 1;
        while (d >= 0 && ++ix[d] == v->shape[d]) { ix[d] = 0; d--; }
        if (d < 0) break;
    }
    return k;
}
static long v_sum_axis(const View *v, int axis, int *out) {
    /* result shape = v.shape without axis, flattened row-major */
    View r = v_index(*v, axis, 0);
    long n = numel(&r);
    int tmp[512];
    for (long i = 0; i < n; i++) out[i] = 0;
    for (long a = 0; a < v->shape[axis]; a++) {
        View s = v_index(*v, axis, a);
        long m = v_to_array(&s, tmp);
        CHECK(m == n);
        for (long i = 0; i < n; i++) out[i] += tmp[i];
    }
    return n;
}

/* the model materializes every step into a fresh row-major array through explicit gathers */
typedef struct { int nd; long shape[MAXD]; int v[512]; } Mat;
static long mat_n(const Mat *m) { long n = 1; for (int i = 0; i < m->nd; i++) n *= m->shape[i]; return n; }
static void unravel(const Mat *m, long flat, long *ix) {
    for (int i = m->nd - 1; i >= 0; i--) { ix[i] = flat % m->shape[i]; flat /= m->shape[i]; }
}
static long ravel(const Mat *m, const long *ix) {
    long f = 0;
    for (int i = 0; i < m->nd; i++) f = f * m->shape[i] + ix[i];
    return f;
}

int main(void) {
    int base[3 * 4 * 5];
    for (int i = 0; i < 60; i++) base[i] = i * 7 % 101;
    long shape0[3] = { 3, 4, 5 };
    long views = 0, contiguous_views = 0, reshapes_ok = 0, reshapes_refused = 0, sums = 0;
    long hist_nd[MAXD + 1] = { 0 };
    for (int trial = 0; trial < 300; trial++) {
        View v = v_base(base, 3, shape0);
        Mat m;
        m.nd = 3; m.shape[0] = 3; m.shape[1] = 4; m.shape[2] = 5;
        for (int i = 0; i < 60; i++) m.v[i] = base[i];
        int steps = 1 + (int)(rnd() % 6);
        for (int s = 0; s < steps; s++) {
            unsigned op = rnd() % 6;
            Mat nm;
            long ix[MAXD], src[MAXD];
            if (op == 0 && m.nd > 1) {                   /* transpose by a random rotation of axes */
                int perm[MAXD];
                int r = 1 + (int)(rnd() % (unsigned)(m.nd - 1));
                for (int i = 0; i < m.nd; i++) perm[i] = (i + r) % m.nd;
                v = v_transpose(v, perm);
                nm.nd = m.nd;
                for (int i = 0; i < m.nd; i++) nm.shape[i] = m.shape[perm[i]];
                for (long f = 0; f < mat_n(&nm); f++) {
                    unravel(&nm, f, ix);
                    for (int i = 0; i < m.nd; i++) src[perm[i]] = ix[i];
                    nm.v[f] = m.v[ravel(&m, src)];
                }
                m = nm;
            } else if (op == 1) {                        /* strided slice, possibly reversed */
                int axis = (int)(rnd() % (unsigned)m.nd);
                long n = m.shape[axis];
                static const long steps_tab[5] = { 1, 2, 3, -1, -2 };
                long step = steps_tab[rnd() % 5];
                long start = (long)(rnd() % (unsigned long)n);
                long maxlen = step > 0 ? (n - 1 - start) / step + 1 : start / (-step) + 1;
                long len = 1 + (long)(rnd() % (unsigned long)maxlen);
                v = v_slice(v, axis, start, step, len);
                nm = m;
                nm.shape[axis] = len;
                for (long f = 0; f < mat_n(&nm); f++) {
                    unravel(&nm, f, ix);
                    for (int i = 0; i < m.nd; i++) src[i] = ix[i];
                    src[axis] = start + step * ix[axis];
                    nm.v[f] = m.v[ravel(&m, src)];
                }
                m = nm;
            } else if (op == 2 && m.nd > 1) {            /* fix one coordinate and drop the axis */
                int axis = (int)(rnd() % (unsigned)m.nd);
                long i0 = (long)(rnd() % (unsigned long)m.shape[axis]);
                v = v_index(v, axis, i0);
                nm.nd = m.nd - 1;
                for (int i = 0, k = 0; i < m.nd; i++) if (i != axis) nm.shape[k++] = m.shape[i];
                for (long f = 0; f < mat_n(&nm); f++) {
                    unravel(&nm, f, ix);
                    for (int i = 0, k = 0; i < m.nd; i++) src[i] = i == axis ? i0 : ix[k++];
                    nm.v[f] = m.v[ravel(&m, src)];
                }
                m = nm;
            } else if (op == 3 && m.nd < MAXD) {         /* new length-1 axis, then broadcast it */
                int axis = (int)(rnd() % (unsigned)(m.nd + 1));
                long n = 2 + (long)(rnd() % 3);
                v = v_broadcast_axis(v_expand(v, axis), axis, n);
                nm.nd = m.nd + 1;
                for (int i = 0, k = 0; i < nm.nd; i++) nm.shape[i] = i == axis ? n : m.shape[k++];
                if (mat_n(&nm) > 512) { /* too big for the model buffers: undo by not applying */ v = v_index(v, axis, 0); continue; }
                for (long f = 0; f < mat_n(&nm); f++) {
                    unravel(&nm, f, ix);
                    for (int i = 0, k = 0; i < nm.nd; i++) if (i != axis) src[k++] = ix[i];
                    nm.v[f] = m.v[ravel(&m, src)];
                }
                m = nm;
            }
            /* compare the view against the materialized model */
            CHECK(v.nd == m.nd);
            for (int i = 0; i < v.nd; i++) CHECK(v.shape[i] == m.shape[i]);
            int flat[512];
            long n = v_to_array(&v, flat);
            CHECK(n == mat_n(&m));
            CHECK(memcmp(flat, m.v, (size_t)n * sizeof(int)) == 0);
            views++;
        }
        hist_nd[v.nd]++;
        if (v_contiguous(&v)) {
            contiguous_views++;
            long flat_shape[1] = { numel(&v) };
            View t = v;
            CHECK(v_reshape(&t, 1, flat_shape));
            int a[512], b[512];
            long na = v_to_array(&v, a), nb = v_to_array(&t, b);
            CHECK(na == nb && memcmp(a, b, (size_t)na * sizeof(int)) == 0);
            reshapes_ok++;
        } else {
            long flat_shape[1] = { numel(&v) };
            View t = v;
            CHECK(!v_reshape(&t, 1, flat_shape));
            reshapes_refused++;
        }
        if (v.nd >= 1 && numel(&v) > 0) {
            int axis = (int)(rnd() % (unsigned)v.nd);
            int got[512], exp[512];
            long n = v_sum_axis(&v, axis, got);
            /* model: sum across the same axis of the materialized matrix */
            Mat r;
            r.nd = m.nd - 1;
            for (int i = 0, k = 0; i < m.nd; i++) if (i != axis) r.shape[k++] = m.shape[i];
            long rn = 1;
            for (int i = 0; i < r.nd; i++) rn *= r.shape[i];
            CHECK(rn == n);
            for (long i = 0; i < rn; i++) exp[i] = 0;
            for (long f = 0; f < mat_n(&m); f++) {
                long ix[MAXD], rx[MAXD];
                unravel(&m, f, ix);
                for (int i = 0, k = 0; i < m.nd; i++) if (i != axis) rx[k++] = ix[i];
                long rf = 0;
                for (int i = 0; i < r.nd; i++) rf = rf * r.shape[i] + rx[i];
                exp[rf] += m.v[f];
            }
            CHECK(memcmp(got, exp, (size_t)n * sizeof(int)) == 0);
            sums++;
        }
    }
    printf("views checked=%ld, final dims histogram: 0d=%ld 1d=%ld 2d=%ld 3d=%ld 4d=%ld\n", views, hist_nd[0], hist_nd[1], hist_nd[2], hist_nd[3], hist_nd[4]);
    printf("contiguous finals=%ld (reshape ok=%ld), non-contiguous refused=%ld, axis sums verified=%ld\n", contiguous_views, reshapes_ok, reshapes_refused, sums);
    /* a fixed showcase: transpose then reverse the columns of a 3x4 slab without copying */
    long sh2[2] = { 3, 4 };
    int small[12];
    for (int i = 0; i < 12; i++) small[i] = i;
    View s = v_base(small, 2, sh2);
    int perm[2] = { 1, 0 };
    View t = v_slice(v_transpose(s, perm), 0, 3, -1, 4);
    int out[12];
    long n = v_to_array(&t, out);
    printf("4x3 transpose with reversed rows:");
    for (long i = 0; i < n; i++) printf(" %d", out[i]);
    printf("\n");
    CHECK(out[0] == 3 && out[1] == 7 && out[2] == 11 && out[11] == 8);
    return 0;
}
