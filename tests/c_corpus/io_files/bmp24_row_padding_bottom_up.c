/*
 * title: BMP 24-bit writer and reader with row padding
 * topic: io_files
 * covers: bmp, little-endian header, BGR order, row padding to 4 bytes, bottom-up rows, header validation
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

typedef struct { int w, h; unsigned char *rgb; } Pic;

static void le16(Buf *b, unsigned v) { bbyte(b, v & 255u); bbyte(b, (v >> 8) & 255u); }
static void le32(Buf *b, uint32_t v) { le16(b, v & 0xFFFFu); le16(b, v >> 16); }
static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }

static size_t row_stride(int w) { return ((size_t)w * 3 + 3) & ~(size_t)3; }

static void write_bmp(const char *name, const Pic *p) {
    Buf b = {0};
    size_t stride = row_stride(p->w);
    uint32_t img = (uint32_t)(stride * (size_t)p->h);
    bstr(&b, "BM");
    le32(&b, 54 + img);
    le32(&b, 0);            /* reserved */
    le32(&b, 54);           /* pixel data offset */
    le32(&b, 40);           /* BITMAPINFOHEADER size */
    le32(&b, (uint32_t)p->w);
    le32(&b, (uint32_t)p->h);   /* positive: bottom-up */
    le16(&b, 1);
    le16(&b, 24);
    le32(&b, 0);            /* BI_RGB */
    le32(&b, img);
    le32(&b, 2835);
    le32(&b, 2835);
    le32(&b, 0);
    le32(&b, 0);
    for (int y = p->h - 1; y >= 0; y--) {
        for (int x = 0; x < p->w; x++) {
            const unsigned char *px = p->rgb + ((size_t)y * p->w + x) * 3;
            bbyte(&b, px[2]); bbyte(&b, px[1]); bbyte(&b, px[0]);
        }
        for (size_t k = (size_t)p->w * 3; k < stride; k++) bbyte(&b, 0);
    }
    wfile(name, b.p, b.n);
    bfree(&b);
}

static const char *read_bmp(const char *name, Pic *p) {
    size_t n;
    unsigned char *d = rfile(name, &n);
    const char *err = NULL;
    p->rgb = NULL;
    if (n < 54 || d[0] != 'B' || d[1] != 'M') err = "not a BMP";
    else if (rd32(d + 2) != n) err = "file size mismatch";
    else if (rd32(d + 14) != 40) err = "unsupported header";
    else if (rd16(d + 26) != 1 || rd16(d + 28) != 24 || rd32(d + 30) != 0) err = "unsupported format";
    else {
        int w = (int)rd32(d + 18);
        int h = (int)rd32(d + 22);
        int top_down = 0;
        if (h < 0) { h = -h; top_down = 1; }
        size_t off = rd32(d + 10);
        if (w <= 0 || h <= 0) err = "bad dimensions";
        else if (off + row_stride(w) * (size_t)h > n) err = "truncated pixel data";
        else {
            p->w = w; p->h = h;
            p->rgb = malloc((size_t)w * h * 3);
            for (int r = 0; r < h; r++) {
                int y = top_down ? r : h - 1 - r;
                const unsigned char *row = d + off + row_stride(w) * (size_t)r;
                for (int x = 0; x < w; x++) {
                    unsigned char *o = p->rgb + ((size_t)y * w + x) * 3;
                    o[0] = row[x * 3 + 2]; o[1] = row[x * 3 + 1]; o[2] = row[x * 3];
                }
            }
        }
    }
    free(d);
    return err;
}

int main(void) {
    static const int sizes[][2] = {{1, 1}, {2, 3}, {3, 2}, {4, 4}, {5, 7}, {13, 6}};
    for (int t = 0; t < 6; t++) {
        Pic p = {sizes[t][0], sizes[t][1], NULL};
        p.rgb = malloc((size_t)p.w * p.h * 3);
        for (int i = 0; i < p.w * p.h * 3; i++) p.rgb[i] = (unsigned char)(rnd() >> 11);
        write_bmp("pic.bmp", &p);
        size_t fs;
        unsigned char *raw = rfile("pic.bmp", &fs);
        size_t pad = row_stride(p.w) - (size_t)p.w * 3;
        CHECK(fs == 54 + row_stride(p.w) * (size_t)p.h);
        /* bottom row of the image is the first row in the file, stored BGR */
        CHECK(raw[54] == p.rgb[((size_t)(p.h - 1) * p.w) * 3 + 2]);
        Pic q;
        const char *e = read_bmp("pic.bmp", &q);
        CHECK(e == NULL);
        CHECK(q.w == p.w && q.h == p.h && memcmp(q.rgb, p.rgb, (size_t)p.w * p.h * 3) == 0);
        printf("%2dx%-2d stride=%2zu pad=%zu file=%3zu bytes header:", p.w, p.h, row_stride(p.w), pad, fs);
        for (int i = 0; i < 14; i++) printf(" %02X", raw[i]);
        putchar('\n');
        free(raw); free(p.rgb); free(q.rgb);
    }
    /* a top-down BMP (negative height) written by hand for a 2x2 image */
    {
        Buf b = {0};
        bstr(&b, "BM"); le32(&b, 54 + 16); le32(&b, 0); le32(&b, 54); le32(&b, 40);
        le32(&b, 2); le32(&b, (uint32_t)-2); le16(&b, 1); le16(&b, 24); le32(&b, 0); le32(&b, 16);
        le32(&b, 0); le32(&b, 0); le32(&b, 0); le32(&b, 0);
        static const unsigned char rows[16] = {0, 0, 255, 0, 255, 0, 0, 0,   255, 0, 0, 255, 255, 255, 0, 0};
        bput(&b, rows, 16);
        wfile("td.bmp", b.p, b.n);
        bfree(&b);
        Pic q;
        CHECK(read_bmp("td.bmp", &q) == NULL);
        printf("top-down: (0,0)=%d,%d,%d (1,0)=%d,%d,%d (0,1)=%d,%d,%d (1,1)=%d,%d,%d\n",
               q.rgb[0], q.rgb[1], q.rgb[2], q.rgb[3], q.rgb[4], q.rgb[5],
               q.rgb[6], q.rgb[7], q.rgb[8], q.rgb[9], q.rgb[10], q.rgb[11]);
        free(q.rgb);
    }
    /* corruption: patch fields and expect specific errors */
    Pic p = {3, 3, malloc(27)};
    memset(p.rgb, 0x5A, 27);
    write_bmp("c.bmp", &p);
    size_t fs;
    unsigned char *raw = rfile("c.bmp", &fs);
    struct { size_t off; unsigned char val; } patches[] = {{0, 'X'}, {2, 0x77}, {28, 8}, {14, 12}, {18, 0}};
    for (int i = 0; i < 5; i++) {
        unsigned char *copy = malloc(fs);
        memcpy(copy, raw, fs);
        copy[patches[i].off] = patches[i].val;
        if (patches[i].off == 18) memset(copy + 19, 0, 3);
        wfile("c2.bmp", copy, fs);
        Pic q;
        const char *e = read_bmp("c2.bmp", &q);
        CHECK(e != NULL);
        printf("patch at %zu: %s\n", patches[i].off, e);
        free(copy);
    }
    wfile("c3.bmp", raw, fs - 5);
    Pic q;
    const char *e = read_bmp("c3.bmp", &q);
    CHECK(e != NULL);
    printf("truncated: %s\n", e);
    free(raw); free(p.rgb);
    remove("pic.bmp"); remove("td.bmp"); remove("c.bmp"); remove("c2.bmp"); remove("c3.bmp");
    return 0;
}
