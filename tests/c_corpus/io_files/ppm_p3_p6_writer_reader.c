/*
 * title: PPM P3 and P6 writer and reader with comments and 16-bit
 * topic: io_files
 * covers: netpbm ppm, ascii and binary, header comments, maxval 255 and 65535, big-endian samples, malformed headers
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

typedef struct { int w, h, maxv; uint16_t *px; } Img; /* 3 samples per pixel */

static Img img_new(int w, int h, int maxv) {
    Img im = {w, h, maxv, calloc((size_t)w * h * 3, sizeof(uint16_t))};
    if (!im.px) fail("oom");
    return im;
}

static void write_p3(const char *name, const Img *im) {
    FILE *f = fopen(name, "w");
    if (!f) fail("open");
    fprintf(f, "P3\n# generated test image\n%d %d\n%d\n", im->w, im->h, im->maxv);
    int col = 0;
    for (size_t i = 0; i < (size_t)im->w * im->h * 3; i++) {
        int n = fprintf(f, "%s%u", col ? " " : "", (unsigned)im->px[i]);
        col += n;
        if (col > 60) { fputc('\n', f); col = 0; }
    }
    if (col) fputc('\n', f);
    fclose(f);
}

static void write_p6(const char *name, const Img *im) {
    Buf b = {0};
    char hdr[64];
    snprintf(hdr, sizeof hdr, "P6\n%d %d\n%d\n", im->w, im->h, im->maxv);
    bstr(&b, hdr);
    for (size_t i = 0; i < (size_t)im->w * im->h * 3; i++) {
        if (im->maxv > 255) bbyte(&b, im->px[i] >> 8);
        bbyte(&b, im->px[i] & 0xFF);
    }
    wfile(name, b.p, b.n);
    bfree(&b);
}

typedef struct { const unsigned char *p; size_t n, pos; } Rd;

static void skip_ws_comments(Rd *r) {
    while (r->pos < r->n) {
        if (r->p[r->pos] == '#') { while (r->pos < r->n && r->p[r->pos] != '\n') r->pos++; }
        else if (r->p[r->pos] == ' ' || r->p[r->pos] == '\n' || r->p[r->pos] == '\t' || r->p[r->pos] == '\r') r->pos++;
        else break;
    }
}

static int read_uint(Rd *r, int *out) {
    skip_ws_comments(r);
    if (r->pos >= r->n || r->p[r->pos] < '0' || r->p[r->pos] > '9') return 0;
    long v = 0;
    while (r->pos < r->n && r->p[r->pos] >= '0' && r->p[r->pos] <= '9') {
        v = v * 10 + (r->p[r->pos++] - '0');
        if (v > 1000000) return 0;
    }
    *out = (int)v;
    return 1;
}

/* returns NULL-image (px == NULL) and sets err on failure */
static Img read_pnm(const unsigned char *d, size_t n, const char **err) {
    Img none = {0, 0, 0, NULL};
    Rd r = {d, n, 0};
    if (n < 2 || d[0] != 'P' || (d[1] != '3' && d[1] != '6')) { *err = "bad magic"; return none; }
    int binary = d[1] == '6';
    r.pos = 2;
    int w, h, maxv;
    if (!read_uint(&r, &w) || !read_uint(&r, &h) || !read_uint(&r, &maxv)) { *err = "bad header"; return none; }
    if (w <= 0 || h <= 0 || maxv <= 0 || maxv > 65535) { *err = "bad dimensions or maxval"; return none; }
    Img im = img_new(w, h, maxv);
    size_t total = (size_t)w * h * 3;
    if (binary) {
        r.pos++; /* exactly one whitespace after maxval */
        size_t bps = maxv > 255 ? 2 : 1;
        if (n - r.pos != total * bps) { *err = "wrong payload size"; free(im.px); return none; }
        for (size_t i = 0; i < total; i++) {
            unsigned v = d[r.pos++];
            if (bps == 2) v = (v << 8) | d[r.pos++];
            if ((int)v > maxv) { *err = "sample above maxval"; free(im.px); return none; }
            im.px[i] = (uint16_t)v;
        }
    } else {
        for (size_t i = 0; i < total; i++) {
            int v;
            if (!read_uint(&r, &v)) { *err = "truncated samples"; free(im.px); return none; }
            if (v > maxv) { *err = "sample above maxval"; free(im.px); return none; }
            im.px[i] = (uint16_t)v;
        }
        skip_ws_comments(&r);
        if (r.pos != n) { *err = "trailing data"; free(im.px); return none; }
    }
    return im;
}

static uint32_t checksum(const Img *im) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)im->w * im->h * 3; i++) h = (h ^ im->px[i]) * 16777619u;
    return h;
}

static Img roundtrip(const Img *src, int binary, const char **err, size_t *fsize) {
    const char *name = binary ? "img.p6" : "img.p3";
    if (binary) write_p6(name, src); else write_p3(name, src);
    size_t n;
    unsigned char *raw = rfile(name, &n);
    *fsize = n;
    Img back = read_pnm(raw, n, err);
    free(raw);
    remove(name);
    return back;
}

int main(void) {
    const char *err = "";
    static const int dims[][3] = {{7, 5, 255}, {16, 3, 255}, {4, 4, 1023}, {9, 2, 65535}, {1, 1, 255}};
    for (int t = 0; t < 5; t++) {
        Img im = img_new(dims[t][0], dims[t][1], dims[t][2]);
        for (int y = 0; y < im.h; y++)
            for (int x = 0; x < im.w; x++) {
                size_t o = ((size_t)y * im.w + x) * 3;
                uint32_t a = rnd();
                im.px[o] = (uint16_t)((uint32_t)(x * im.maxv / (im.w > 1 ? im.w - 1 : 1)));
                im.px[o + 1] = (uint16_t)((uint32_t)(y * im.maxv / (im.h > 1 ? im.h - 1 : 1)));
                im.px[o + 2] = (uint16_t)(a % (uint32_t)(im.maxv + 1));
            }
        size_t s3, s6;
        Img a = roundtrip(&im, 0, &err, &s3);
        CHECK(a.px);
        Img b = roundtrip(&im, 1, &err, &s6);
        CHECK(b.px);
        CHECK(memcmp(a.px, im.px, (size_t)im.w * im.h * 3 * sizeof(uint16_t)) == 0);
        CHECK(memcmp(b.px, im.px, (size_t)im.w * im.h * 3 * sizeof(uint16_t)) == 0);
        printf("%dx%d maxv=%d: p3=%zu bytes, p6=%zu bytes, fnv=%08x\n", im.w, im.h, im.maxv, s3, s6, (unsigned)checksum(&im));
        free(im.px); free(a.px); free(b.px);
    }
    /* hand-written P3 with comments in odd places */
    const char *hand = "P3 # magic\n2 # width\n1\n# depth next\n255\n255 0 0  # red\n0 128 255\n";
    Img h = read_pnm((const unsigned char *)hand, strlen(hand), &err);
    CHECK(h.px && h.w == 2 && h.h == 1 && h.px[0] == 255 && h.px[4] == 128 && h.px[5] == 255);
    printf("hand-written: %dx%d first=(%u,%u,%u) second=(%u,%u,%u)\n", h.w, h.h,
           (unsigned)h.px[0], (unsigned)h.px[1], (unsigned)h.px[2], (unsigned)h.px[3], (unsigned)h.px[4], (unsigned)h.px[5]);
    free(h.px);
    static const char *bad[] = {
        "P7\n1 1\n255\n\0\0\0", "P3\n1 1\n255\n1 2\n", "P3\n1 1\n256000\n1 2 3\n", "P3\n1 1\n255\n1 2 300\n",
        "P3\n0 4\n255\n", "P3\n1 1\n255\n1 2 3 4\n", "P6\n1 1\n255\nabcd"
    };
    for (int i = 0; i < 7; i++) {
        size_t bl = i == 0 ? 15 : strlen(bad[i]);
        Img x = read_pnm((const unsigned char *)bad[i], bl, &err);
        CHECK(x.px == NULL);
        printf("bad %d: %s\n", i, err);
    }
    return 0;
}
