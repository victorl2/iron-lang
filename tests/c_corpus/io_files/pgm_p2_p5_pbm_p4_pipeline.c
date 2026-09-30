/*
 * title: PGM P2/P5 pipeline with PBM P4 bit-packed export
 * topic: io_files
 * covers: netpbm pgm, pbm, bit packing with row padding, maxval rescale, threshold, histogram, ascii render
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

typedef struct { int w, h, maxv; uint16_t *px; } Gray;

static Gray gnew(int w, int h, int maxv) {
    Gray g = {w, h, maxv, calloc((size_t)w * h, sizeof(uint16_t))};
    if (!g.px) fail("oom");
    return g;
}

/* Synthetic scene: soft diagonal gradient, a disc, and noise. */
static Gray scene(int w, int h, int maxv) {
    Gray g = gnew(w, h, maxv);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            long v = (long)(x + y) * maxv / (w + h - 2);
            long dx = x - w / 2, dy = y - h / 2;
            if (dx * dx + dy * dy < (long)(h * h) / 6) v = maxv - v / 2;
            long noise = (long)(rnd() % 9) - 4;
            v += noise * maxv / 100;
            if (v < 0) v = 0;
            if (v > maxv) v = maxv;
            g.px[y * w + x] = (uint16_t)v;
        }
    return g;
}

static void save_p2(const char *n, const Gray *g) {
    Buf b = {0};
    char t[64];
    snprintf(t, sizeof t, "P2\n%d %d\n%d\n", g->w, g->h, g->maxv);
    bstr(&b, t);
    for (int y = 0; y < g->h; y++) {
        for (int x = 0; x < g->w; x++) {
            snprintf(t, sizeof t, x ? " %u" : "%u", (unsigned)g->px[y * g->w + x]);
            bstr(&b, t);
        }
        bbyte(&b, '\n');
    }
    wfile(n, b.p, b.n);
    bfree(&b);
}

static void save_p5(const char *n, const Gray *g) {
    Buf b = {0};
    char t[64];
    snprintf(t, sizeof t, "P5\n%d %d\n%d\n", g->w, g->h, g->maxv);
    bstr(&b, t);
    for (int i = 0; i < g->w * g->h; i++) {
        if (g->maxv > 255) bbyte(&b, g->px[i] >> 8);
        bbyte(&b, g->px[i] & 255u);
    }
    wfile(n, b.p, b.n);
    bfree(&b);
}

static int num(const unsigned char *d, size_t n, size_t *pos, int *out) {
    for (;;) {
        while (*pos < n && (d[*pos] == ' ' || d[*pos] == '\n' || d[*pos] == '\t')) (*pos)++;
        if (*pos < n && d[*pos] == '#') { while (*pos < n && d[*pos] != '\n') (*pos)++; }
        else break;
    }
    if (*pos >= n || d[*pos] < '0' || d[*pos] > '9') return 0;
    int v = 0;
    while (*pos < n && d[*pos] >= '0' && d[*pos] <= '9') v = v * 10 + (d[(*pos)++] - '0');
    *out = v;
    return 1;
}

static int load(const char *name, Gray *g) {
    size_t n, pos = 2;
    unsigned char *d = rfile(name, &n);
    int w, h, m, ok = 0;
    if (n > 2 && d[0] == 'P' && (d[1] == '2' || d[1] == '5') && num(d, n, &pos, &w) && num(d, n, &pos, &h) && num(d, n, &pos, &m)) {
        *g = gnew(w, h, m);
        if (d[1] == '2') {
            ok = 1;
            for (int i = 0; i < w * h && ok; i++) { int v; ok = num(d, n, &pos, &v); g->px[i] = (uint16_t)v; }
        } else {
            pos++;
            size_t bps = m > 255 ? 2 : 1;
            ok = n - pos == (size_t)w * h * bps;
            for (int i = 0; i < w * h && ok; i++) {
                unsigned v = d[pos++];
                if (bps == 2) v = (v << 8) | d[pos++];
                g->px[i] = (uint16_t)v;
            }
        }
        if (!ok) free(g->px);
    }
    free(d);
    return ok;
}

static Gray rescale(const Gray *g, int newmax) {
    Gray o = gnew(g->w, g->h, newmax);
    for (int i = 0; i < g->w * g->h; i++)
        o.px[i] = (uint16_t)(((uint32_t)g->px[i] * (uint32_t)newmax + (uint32_t)g->maxv / 2) / (uint32_t)g->maxv);
    return o;
}

/* P4: 1 = black. Rows padded to whole bytes, MSB first. */
static void save_p4(const char *n, const Gray *g, int thr) {
    Buf b = {0};
    char t[64];
    snprintf(t, sizeof t, "P4\n%d %d\n", g->w, g->h);
    bstr(&b, t);
    for (int y = 0; y < g->h; y++) {
        unsigned acc = 0;
        for (int x = 0; x < g->w; x++) {
            acc = (acc << 1) | (g->px[y * g->w + x] < thr ? 1u : 0u);
            if (x % 8 == 7) { bbyte(&b, acc); acc = 0; }
        }
        if (g->w % 8) bbyte(&b, acc << (8 - g->w % 8));
    }
    wfile(n, b.p, b.n);
    bfree(&b);
}

int main(void) {
    Gray a = scene(21, 12, 255);
    save_p2("a.pgm", &a);
    Gray b;
    CHECK(load("a.pgm", &b));
    CHECK(memcmp(a.px, b.px, sizeof(uint16_t) * 21 * 12) == 0);
    save_p5("b.pgm", &b);
    Gray c;
    CHECK(load("b.pgm", &c));
    CHECK(memcmp(a.px, c.px, sizeof(uint16_t) * 21 * 12) == 0);
    size_t s2, s5;
    free(rfile("a.pgm", &s2));
    free(rfile("b.pgm", &s5));
    printf("P2 file %zu bytes, P5 file %zu bytes\n", s2, s5);

    /* histogram in 8 buckets */
    int hist[8] = {0};
    for (int i = 0; i < 21 * 12; i++) hist[a.px[i] * 8 / 256]++;
    int sum = 0;
    for (int i = 0; i < 8; i++) { printf("bucket %d: %3d ", i, hist[i]); for (int k = 0; k < hist[i] / 4; k++) putchar('#'); putchar('\n'); sum += hist[i]; }
    CHECK(sum == 21 * 12);

    /* ASCII render */
    static const char ramp[] = " .:-=+*#%@";
    for (int y = 0; y < a.h; y++) {
        for (int x = 0; x < a.w; x++) putchar(ramp[a.px[y * a.w + x] * 10 / 256]);
        putchar('\n');
    }
    /* 16-bit rescale and back is lossy but bounded */
    Gray wide = rescale(&a, 65535);
    save_p5("w.pgm", &wide);
    Gray wback;
    CHECK(load("w.pgm", &wback));
    CHECK(memcmp(wide.px, wback.px, sizeof(uint16_t) * 21 * 12) == 0);
    Gray narrow = rescale(&wback, 255);
    CHECK(memcmp(narrow.px, a.px, sizeof(uint16_t) * 21 * 12) == 0);
    Gray coarse = rescale(&a, 15);
    Gray restored = rescale(&coarse, 255);
    int maxerr = 0;
    for (int i = 0; i < 21 * 12; i++) {
        int e = (int)a.px[i] - (int)restored.px[i];
        if (e < 0) e = -e;
        if (e > maxerr) maxerr = e;
    }
    CHECK(maxerr <= 9);
    printf("16-bit file %ld bytes; 4-bit round trip max error %d\n", (long)(2 * 21 * 12), maxerr);
    save_p4("t.pbm", &a, 128);
    size_t s4;
    unsigned char *p4 = rfile("t.pbm", &s4);
    size_t hdr = 0;
    int nl = 0;
    while (nl < 2) if (p4[hdr++] == '\n') nl++;
    CHECK(s4 - hdr == (size_t)((21 + 7) / 8) * 12);
    int dark = 0;
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 21; x++) {
            int bit = (p4[hdr + (size_t)y * 3 + (size_t)(x / 8)] >> (7 - x % 8)) & 1;
            CHECK(bit == (a.px[y * 21 + x] < 128));
            dark += bit;
        }
    printf("P4 payload %zu bytes (3 per row, %d padding bits), dark pixels %d\n", s4 - hdr, 3 * 8 - 21, dark);
    free(p4);
    free(a.px); free(b.px); free(c.px); free(wide.px); free(wback.px); free(narrow.px); free(coarse.px); free(restored.px);
    remove("a.pgm"); remove("b.pgm"); remove("w.pgm"); remove("t.pbm");
    return 0;
}
