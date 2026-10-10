/*
 * title: Binary table of Q16.16 and Q8.24 fixed-point columns
 * topic: io_files
 * covers: fixed point, q16.16, saturating conversion, big-endian columns, column-major layout, random access by offset, rounding, statistics
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void fail(const char *w) {
    fprintf(stderr, "check failed: %s\n", w);
    exit(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

void wfile(const char *name, const void *buf, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open for write");
    if (len && fwrite(buf, 1, len, f) != len) fail("write");
    if (fclose(f) != 0) fail("close");
}

unsigned char *rfile(const char *name, size_t *len) {
    FILE *f = fopen(name, "rb");
    if (!f) fail("open for read");
    size_t cap = 256, n = 0;
    unsigned char *b = malloc(cap);
    if (!b) fail("oom");
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) fail("oom");
        }
        size_t r = fread(b + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b;
}


static uint32_t rng_s = 0x2545F491u;
uint32_t rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}
/* growable byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } Buf;
void bput(Buf *b, const void *s, size_t k) {
    if (b->n + k > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->n + k) nc *= 2;
        b->p = realloc(b->p, nc);
        if (!b->p) fail("oom");
        b->cap = nc;
    }
    if (k) memcpy(b->p + b->n, s, k);
    b->n += k;
}
void bbyte(Buf *b, unsigned v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* Layout: "FXT1", u16 ncols, u16 nrows, per column {u8 frac_bits, char name[7]}, then column-major i32 BE data. */
typedef struct { int frac; char name[8]; } ColDef;
typedef struct { int ncols, nrows; ColDef def[6]; int32_t *v; } Table;  /* v[col * nrows + row] */

static int32_t to_fixed(int64_t num, int64_t den, int frac) {
    /* round half away from zero, saturate to int32 */
    int64_t scaled = num * ((int64_t)1 << frac);
    int64_t q = scaled >= 0 ? (scaled + den / 2) / den : -((-scaled + den / 2) / den);
    if (q > INT32_MAX) return INT32_MAX;
    if (q < INT32_MIN) return INT32_MIN;
    return (int32_t)q;
}

/* value * 10^places, rounded, for printing without floats */
static void fmt_fixed(char *out, size_t cap, int32_t v, int frac, int places) {
    int neg = v < 0;
    uint64_t mag = neg ? (uint64_t)(-(int64_t)v) : (uint64_t)v;
    uint64_t pow10 = 1;
    for (int i = 0; i < places; i++) pow10 *= 10;
    uint64_t scaled = (mag * pow10 + ((uint64_t)1 << (frac - 1))) >> frac;
    snprintf(out, cap, "%s%llu.%0*llu", neg && scaled ? "-" : "", (unsigned long long)(scaled / pow10), places, (unsigned long long)(scaled % pow10));
}

static int32_t fx_mul(int32_t a, int32_t b, int frac) {
    int64_t p = (int64_t)a * b;
    int neg = p < 0;
    uint64_t mag = neg ? (uint64_t)(-p) : (uint64_t)p;
    mag = (mag + ((uint64_t)1 << (frac - 1))) >> frac;   /* round half away from zero, on the magnitude */
    p = neg ? -(int64_t)mag : (int64_t)mag;
    if (p > INT32_MAX) return INT32_MAX;
    if (p < INT32_MIN) return INT32_MIN;
    return (int32_t)p;
}

static void save(const char *name, const Table *t) {
    Buf b = {0};
    bstr(&b, "FXT1");
    bbyte(&b, (unsigned)t->ncols >> 8); bbyte(&b, (unsigned)t->ncols & 255u);
    bbyte(&b, (unsigned)t->nrows >> 8); bbyte(&b, (unsigned)t->nrows & 255u);
    for (int c = 0; c < t->ncols; c++) {
        bbyte(&b, (unsigned)t->def[c].frac);
        char nm[7] = {0};
        memcpy(nm, t->def[c].name, strlen(t->def[c].name) < 7 ? strlen(t->def[c].name) : 7);
        bput(&b, nm, 7);
    }
    for (int c = 0; c < t->ncols; c++)
        for (int r = 0; r < t->nrows; r++) {
            uint32_t u = (uint32_t)t->v[c * t->nrows + r];
            for (int k = 3; k >= 0; k--) bbyte(&b, (u >> (8 * k)) & 255u);
        }
    wfile(name, b.p, b.n);
    bfree(&b);
}

/* Random access: read one cell straight from the file bytes without loading the table. */
static int32_t cell_from_file(const unsigned char *d, size_t n, int col, int row) {
    int ncols = (d[4] << 8) | d[5], nrows = (d[6] << 8) | d[7];
    CHECK(col < ncols && row < nrows);
    size_t off = 8 + (size_t)ncols * 8 + ((size_t)col * (size_t)nrows + (size_t)row) * 4;
    CHECK(off + 4 <= n);
    uint32_t u = ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) | ((uint32_t)d[off + 2] << 8) | d[off + 3];
    return (int32_t)u;
}

int main(void) {
    Table t = {3, 24, {{16, "price"}, {16, "weight"}, {24, "ratio"}}, NULL};
    t.v = malloc((size_t)t.ncols * t.nrows * sizeof(int32_t));
    int64_t ref_num[3][24];
    for (int r = 0; r < t.nrows; r++) {
        int64_t p = (int64_t)(rnd() % 120000) - 20000;         /* cents, may be negative */
        int64_t w = (int64_t)(rnd() % 9000) + 1;               /* grams */
        int64_t q = (int64_t)(rnd() % 2001) - 1000;
        ref_num[0][r] = p; ref_num[1][r] = w; ref_num[2][r] = q;
        t.v[0 * t.nrows + r] = to_fixed(p, 100, 16);
        t.v[1 * t.nrows + r] = to_fixed(w, 1000, 16);
        t.v[2 * t.nrows + r] = to_fixed(q, 1000, 24);
    }
    /* saturation probes */
    CHECK(to_fixed(100000, 1, 16) == INT32_MAX);
    CHECK(to_fixed(-100000, 1, 16) == INT32_MIN);
    CHECK(to_fixed(1, 3, 16) == 21845 && to_fixed(-1, 3, 16) == -21845 && to_fixed(1, 2, 0) == 1 && to_fixed(-1, 2, 0) == -1);
    save("table.fxt", &t);
    size_t n;
    unsigned char *d = rfile("table.fxt", &n);
    CHECK(n == 8 + 3 * 8 + 3 * 24 * 4);
    printf("file %zu bytes, magic %.4s, cols=%d rows=%d\n", n, (char *)d, (d[4] << 8) | d[5], (d[6] << 8) | d[7]);
    for (int c = 0; c < 3; c++) {
        printf("col %d: %-6s frac=%d\n", c, (const char *)d + 8 + c * 8 + 1, d[8 + c * 8]);
    }
    for (int r = 0; r < 24; r++)
        for (int c = 0; c < 3; c++) CHECK(cell_from_file(d, n, c, r) == t.v[c * 24 + r]);
    /* verify the rounding error bound: |fixed - exact| <= half an LSB */
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 24; r++) {
            static const int64_t den[3] = {100, 1000, 1000};
            int frac = t.def[c].frac;
            /* (v / 2^frac - num/den) * den * 2^frac <= den/2 */
            int64_t lhs = (int64_t)t.v[c * 24 + r] * den[c] - ref_num[c][r] * ((int64_t)1 << frac);
            if (lhs < 0) lhs = -lhs;
            CHECK(lhs * 2 <= den[c]);
        }
    for (int r = 0; r < 5; r++) {
        char a[32], b[32], q[32], prod[32];
        fmt_fixed(a, sizeof a, t.v[r], 16, 2);
        fmt_fixed(b, sizeof b, t.v[24 + r], 16, 3);
        fmt_fixed(q, sizeof q, t.v[48 + r], 24, 6);
        /* price * weight in Q16.16 */
        int32_t pw = fx_mul(t.v[r], t.v[24 + r], 16);
        fmt_fixed(prod, sizeof prod, pw, 16, 3);
        printf("row %d: price=%10s weight=%8s ratio=%10s price*weight=%s\n", r, a, b, q, prod);
    }
    /* column sums with 64-bit accumulators, compare to exact rational sums */
    for (int c = 0; c < 3; c++) {
        int64_t acc = 0, exact_num = 0;
        static const int64_t den[3] = {100, 1000, 1000};
        for (int r = 0; r < 24; r++) { acc += t.v[c * 24 + r]; exact_num += ref_num[c][r]; }
        int frac = t.def[c].frac;
        /* acc / 2^frac vs exact_num / den: difference bounded by rows * 0.5 LSB */
        int64_t diff = acc * den[c] - exact_num * ((int64_t)1 << frac);
        if (diff < 0) diff = -diff;
        CHECK(diff * 2 <= den[c] * 24);
        char s[32];
        fmt_fixed(s, sizeof s, (int32_t)(acc >> 8), frac - 8, 4);
        printf("col %s sum ~ %s (exact %lld/%lld)\n", t.def[c].name, s, (long long)exact_num, (long long)den[c]);
    }
    int32_t x = to_fixed(3, 2, 16), y = to_fixed(-5, 4, 16);
    char sx[32], sy[32], sp[32];
    fmt_fixed(sx, sizeof sx, x, 16, 4); fmt_fixed(sy, sizeof sy, y, 16, 4); fmt_fixed(sp, sizeof sp, fx_mul(x, y, 16), 16, 5);
    printf("1.5 * -1.25 = %s * %s = %s\n", sx, sy, sp);
    CHECK(fx_mul(to_fixed(30000, 1, 16), to_fixed(3, 1, 16), 16) == INT32_MAX);
    free(d); free(t.v);
    remove("table.fxt");
    return 0;
}
