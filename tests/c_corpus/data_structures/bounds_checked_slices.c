/*
 * title: Bounds-checked slice views with status codes, chunks and windows
 * topic: data_structures
 * covers: fat pointers, subslices, split_at, overlap detection, chunk and window iterators, binary search result encoding, error paths
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed: %s (line %d)\n", #c, __LINE__); exit(1); } } while (0)

static unsigned rs = 1300u;
static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}

typedef enum { S_OK, S_RANGE, S_OVERLAP, S_LEN_MISMATCH, S_EMPTY, S_ZERO_SIZE, S_NOT_FOUND } Status;
static const char *status_name(Status s) {
    switch (s) {
    case S_OK: return "ok";
    case S_RANGE: return "out of range";
    case S_OVERLAP: return "overlap";
    case S_LEN_MISMATCH: return "length mismatch";
    case S_EMPTY: return "empty";
    case S_ZERO_SIZE: return "zero size";
    case S_NOT_FOUND: return "not found";
    }
    return "?";
}
static long status_count[7];
static Status note(Status s) { status_count[s]++; return s; }

typedef struct { int *p; size_t n; } Slice;

static Slice s_from(int *p, size_t n) { Slice s = { p, n }; return s; }
static Status s_get(Slice s, size_t i, int *out) {
    if (i >= s.n) return note(S_RANGE);
    *out = s.p[i];
    return note(S_OK);
}
static Status s_set(Slice s, size_t i, int v) {
    if (i >= s.n) return note(S_RANGE);
    s.p[i] = v;
    return note(S_OK);
}
static Status s_sub(Slice s, size_t from, size_t to, Slice *out) {
    if (from > to || to > s.n) return note(S_RANGE);
    out->p = s.p + from; out->n = to - from;
    return note(S_OK);
}
static Status s_split_at(Slice s, size_t mid, Slice *l, Slice *r) {
    if (mid > s.n) return note(S_RANGE);
    l->p = s.p; l->n = mid;
    r->p = s.p + mid; r->n = s.n - mid;
    return note(S_OK);
}
static Status s_split_first(Slice s, int *first, Slice *rest) {
    if (s.n == 0) return note(S_EMPTY);
    *first = s.p[0];
    rest->p = s.p + 1; rest->n = s.n - 1;
    return note(S_OK);
}
static int overlaps(Slice a, Slice b) {
    if (a.n == 0 || b.n == 0) return 0;
    uintptr_t a0 = (uintptr_t)a.p, a1 = (uintptr_t)(a.p + a.n), b0 = (uintptr_t)b.p, b1 = (uintptr_t)(b.p + b.n);
    return a0 < b1 && b0 < a1;
}
static Status s_copy_from(Slice dst, Slice src) {
    if (dst.n != src.n) return note(S_LEN_MISMATCH);
    if (overlaps(dst, src)) return note(S_OVERLAP);
    memcpy(dst.p, src.p, dst.n * sizeof(int));
    return note(S_OK);
}
/* overlap-safe, like memmove within one slice */
static Status s_copy_within(Slice s, size_t src, size_t count, size_t dst) {
    if (src > s.n || count > s.n - src || dst > s.n || count > s.n - dst) return note(S_RANGE);
    memmove(s.p + dst, s.p + src, count * sizeof(int));
    return note(S_OK);
}
static void s_fill(Slice s, int v) { for (size_t i = 0; i < s.n; i++) s.p[i] = v; }
static void s_reverse(Slice s) { for (size_t i = 0, j = s.n; i + 1 < j; i++, j--) { int t = s.p[i]; s.p[i] = s.p[j - 1]; s.p[j - 1] = t; } }
static Status s_swap(Slice s, size_t i, size_t j) {
    if (i >= s.n || j >= s.n) return note(S_RANGE);
    int t = s.p[i]; s.p[i] = s.p[j]; s.p[j] = t;
    return note(S_OK);
}
static Status s_rotate_left(Slice s, size_t k) {
    if (k > s.n) return note(S_RANGE);
    Slice a, b;
    s_split_at(s, k, &a, &b);
    s_reverse(a); s_reverse(b); s_reverse(s);
    return note(S_OK);
}
/* returns S_OK with *pos = index, or S_NOT_FOUND with *pos = insertion point */
static Status s_binary_search(Slice s, int key, size_t *pos) {
    size_t lo = 0, hi = s.n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (s.p[m] < key) lo = m + 1; else hi = m;
    }
    *pos = lo;
    if (lo < s.n && s.p[lo] == key) return note(S_OK);
    return note(S_NOT_FOUND);
}
typedef struct { Slice rest; size_t size; } Chunks;
static Status chunks_init(Chunks *c, Slice s, size_t size) {
    if (size == 0) return note(S_ZERO_SIZE);
    c->rest = s; c->size = size;
    return note(S_OK);
}
static int chunks_next(Chunks *c, Slice *out) {
    if (c->rest.n == 0) return 0;
    size_t k = c->rest.n < c->size ? c->rest.n : c->size;
    out->p = c->rest.p; out->n = k;
    c->rest.p += k; c->rest.n -= k;
    return 1;
}
typedef struct { Slice s; size_t size, at; } Windows;
static Status windows_init(Windows *w, Slice s, size_t size) {
    if (size == 0) return note(S_ZERO_SIZE);
    w->s = s; w->size = size; w->at = 0;
    return note(S_OK);
}
static int windows_next(Windows *w, Slice *out) {
    if (w->at + w->size > w->s.n) return 0;
    out->p = w->s.p + w->at; out->n = w->size;
    w->at++;
    return 1;
}
static long s_sum(Slice s) { long t = 0; for (size_t i = 0; i < s.n; i++) t += s.p[i]; return t; }

int main(void) {
    enum { N = 64 };
    int buf[N], model[N];
    for (int i = 0; i < N; i++) buf[i] = model[i] = (int)(rnd() % 100);
    Slice all = s_from(buf, N);

    /* explicit error-path tour */
    int tmp = 0;
    Slice a, b, c;
    printf("get[64]: %s\n", status_name(s_get(all, N, &tmp)));
    printf("get[63]: %s\n", status_name(s_get(all, N - 1, &tmp)));
    printf("sub(10,5): %s\n", status_name(s_sub(all, 10, 5, &a)));
    printf("sub(0,65): %s\n", status_name(s_sub(all, 0, N + 1, &a)));
    Status st_end = s_sub(all, N, N, &a);
    printf("sub(64,64): %s (len %zu)\n", status_name(st_end), a.n);
    printf("split_at(65): %s\n", status_name(s_split_at(all, N + 1, &a, &b)));
    Slice empty = s_from(buf, 0);
    printf("split_first(empty): %s\n", status_name(s_split_first(empty, &tmp, &a)));
    CHECK(s_sub(all, 0, 8, &a) == S_OK && s_sub(all, 4, 12, &b) == S_OK && s_sub(all, 20, 28, &c) == S_OK);
    printf("copy overlapping: %s\n", status_name(s_copy_from(a, b)));
    printf("copy mismatched: %s\n", status_name(s_copy_from(a, all)));
    printf("copy disjoint: %s\n", status_name(s_copy_from(a, c)));
    memcpy(model, buf, sizeof buf); /* buf changed through the disjoint copy; refresh the model */
    Chunks ch;
    Windows wd;
    printf("chunks(0): %s, windows(0): %s\n", status_name(chunks_init(&ch, all, 0)), status_name(windows_init(&wd, all, 0)));
    printf("copy_within out of range: %s\n", status_name(s_copy_within(all, 60, 8, 0)));

    /* randomized operations against the model array */
    long ops = 0, errors = 0;
    for (int step = 0; step < 4000; step++) {
        unsigned op = rnd() % 8;
        size_t i = rnd() % (N + 4), j = rnd() % (N + 4);
        if (op == 0) {
            Status st = s_set(all, i, (int)(rnd() % 100));
            CHECK((st == S_OK) == (i < N));
            if (st == S_OK) model[i] = buf[i];
            else errors++;
        } else if (op == 1) {
            Status st = s_swap(all, i, j);
            CHECK((st == S_OK) == (i < N && j < N));
            if (st == S_OK) { int t = model[i]; model[i] = model[j]; model[j] = t; } else errors++;
        } else if (op == 2) {
            size_t from = i > j ? j : i, to = i > j ? i : j;
            Slice sub;
            Status st = s_sub(all, from, to, &sub);
            CHECK((st == S_OK) == (to <= N));
            if (st == S_OK) {
                CHECK(sub.n == to - from && sub.p == buf + from);
                if (rnd() % 2) { s_reverse(sub); for (size_t x = 0, y = to - from; x + 1 < y; x++, y--) { int t = model[from + x]; model[from + x] = model[from + y - 1]; model[from + y - 1] = t; } }
                else { int v = (int)(rnd() % 100); s_fill(sub, v); for (size_t x = from; x < to; x++) model[x] = v; }
            } else errors++;
        } else if (op == 3) {
            size_t count = rnd() % 20;
            Status st = s_copy_within(all, i, count, j);
            CHECK((st == S_OK) == (i <= N && count <= N - i && j <= N && count <= N - j));
            if (st == S_OK) memmove(model + j, model + i, count * sizeof(int)); else errors++;
        } else if (op == 4) {
            size_t k = rnd() % (N + 3);
            Status st = s_rotate_left(all, k);
            CHECK((st == S_OK) == (k <= N));
            if (st == S_OK) { int tmpm[N]; for (size_t x = 0; x < N; x++) tmpm[x] = model[(x + k) % N]; memcpy(model, tmpm, sizeof tmpm); } else errors++;
        } else if (op == 5) {
            /* chunk and window sums against direct loops */
            size_t size = 1 + rnd() % 9;
            Chunks cs;
            CHECK(chunks_init(&cs, all, size) == S_OK);
            Slice ck;
            size_t pos = 0;
            while (chunks_next(&cs, &ck)) {
                size_t expect_n = N - pos < size ? N - pos : size;
                CHECK(ck.n == expect_n);
                long e = 0;
                for (size_t x = 0; x < expect_n; x++) e += model[pos + x];
                CHECK(s_sum(ck) == e);
                pos += ck.n;
            }
            CHECK(pos == N);
            Windows ws;
            CHECK(windows_init(&ws, all, size) == S_OK);
            Slice w;
            size_t count = 0;
            while (windows_next(&ws, &w)) {
                long e = 0;
                for (size_t x = 0; x < size; x++) e += model[count + x];
                CHECK(s_sum(w) == e);
                count++;
            }
            CHECK(count == N - size + 1);
        } else if (op == 6) {
            /* sort a random subslice by insertion, then binary search inside it */
            size_t from = i % N, to = from + rnd() % (N - from + 1);
            Slice sub;
            CHECK(s_sub(all, from, to, &sub) == S_OK);
            for (size_t x = 1; x < sub.n; x++) { int v = sub.p[x]; size_t y = x; while (y > 0 && sub.p[y - 1] > v) { sub.p[y] = sub.p[y - 1]; y--; } sub.p[y] = v; }
            for (size_t x = 1; x < to - from; x++) { int v = model[from + x]; size_t y = x; while (y > 0 && model[from + y - 1] > v) { model[from + y] = model[from + y - 1]; y--; } model[from + y] = v; }
            int key = (int)(rnd() % 100);
            size_t pos;
            Status st = s_binary_search(sub, key, &pos);
            size_t expect = 0;
            for (size_t x = 0; x < sub.n; x++) if (model[from + x] < key) expect++;
            CHECK(pos == expect);
            CHECK((st == S_OK) == (expect < sub.n && model[from + expect] == key));
        } else {
            int first = 0;
            Slice rest;
            size_t from = i % N;
            Slice sub;
            CHECK(s_sub(all, from, N, &sub) == S_OK);
            Status st = s_split_first(sub, &first, &rest);
            CHECK(st == S_OK && first == model[from] && rest.n == sub.n - 1);
        }
        ops++;
        CHECK(memcmp(buf, model, sizeof buf) == 0);
    }
    printf("randomized: %ld ops, %ld rejected by bounds checks, buffer always matched the model\n", ops, errors);
    printf("status counts:");
    for (int s = 0; s < 7; s++) printf(" %s=%ld", status_name((Status)s), status_count[s]);
    printf("\n");
    return 0;
}
