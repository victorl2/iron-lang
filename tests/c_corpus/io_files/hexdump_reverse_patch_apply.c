/*
 * title: Reverse hexdump parser applying edited dumps as patches
 * topic: io_files
 * covers: xxd -r semantics, offset seeking, sparse gaps, hexdump -C squeeze lines, plain hex, ascii column ignored, file extension and overwrite
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

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* Dump in xxd-like style: "OFFSET: hhhh hhhh ...  ascii", but only for rows in `rows` (edited dumps are partial). */
static void dump_rows(Buf *out, const unsigned char *d, size_t n, size_t from, size_t to) {
    for (size_t off = from; off < to && off < n; off += 16) {
        char t[16];
        snprintf(t, sizeof t, "%08zx:", off);
        bstr(out, t);
        size_t l = n - off < 16 ? n - off : 16;
        for (size_t i = 0; i < 16; i++) {
            if (i % 2 == 0) bbyte(out, ' ');
            if (i < l) { snprintf(t, sizeof t, "%02x", d[off + i]); bstr(out, t); } else bstr(out, "  ");
        }
        bstr(out, "  ");
        for (size_t i = 0; i < l; i++) bbyte(out, d[off + i] >= 32 && d[off + i] < 127 ? d[off + i] : '.');
        bbyte(out, '\n');
    }
}

/* Apply an xxd-style dump to image (growable). Lines: "offset: hex..." with optional two-space ascii tail. */
static int apply_dump(const char *text, unsigned char **img, size_t *len, int *lines, int *bytes_written) {
    *lines = 0; *bytes_written = 0;
    while (*text) {
        const char *e = strchr(text, '\n');
        size_t l = e ? (size_t)(e - text) : strlen(text);
        char line[200];
        if (l >= sizeof line) return 0;
        memcpy(line, text, l);
        line[l] = 0;
        text += l + (e ? 1 : 0);
        if (!l || line[0] == '#') continue;
        char *colon = strchr(line, ':');
        if (!colon) return 0;
        size_t off = 0;
        for (char *p = line; p < colon; p++) {
            int v = hexv((unsigned char)*p);
            if (v < 0) return 0;
            off = off * 16 + (size_t)v;
        }
        if (off > 1u << 20) return 0;
        char *p = colon + 1;
        size_t at = off;
        for (;;) {
            while (*p == ' ') {
                /* two consecutive spaces end the hex area (ascii column follows) unless another hex pair is right there */
                if (p[1] == ' ') { p = NULL; break; }
                p++;
            }
            if (!p || !*p) break;
            int hi = hexv((unsigned char)p[0]), lo = hexv((unsigned char)p[1]);
            if (hi < 0 || lo < 0) return 0;
            if (at >= *len) {
                *img = realloc(*img, at + 1);
                memset(*img + *len, 0, at + 1 - *len);   /* gap bytes read as zero, like xxd -r */
                *len = at + 1;
            }
            (*img)[at++] = (unsigned char)(hi * 16 + lo);
            (*bytes_written)++;
            p += 2;
        }
        (*lines)++;
    }
    return 1;
}

/* Plain hex ("xxd -p" style): stream of hex digit pairs, whitespace ignored */
static int from_plain(const char *s, Buf *out) {
    int half = -1;
    for (; *s; s++) {
        if (*s == ' ' || *s == '\n') continue;
        int v = hexv((unsigned char)*s);
        if (v < 0) return 0;
        if (half < 0) half = v; else { bbyte(out, (unsigned)(half * 16 + v)); half = -1; }
    }
    return half < 0;
}

/* hexdump -C style with '*' lines meaning "repeat previous row until the next offset" */
static int from_canonical(const char *text, Buf *out) {
    unsigned char prev[16];
    size_t prev_len = 0;
    int squeezing = 0;
    while (*text) {
        const char *e = strchr(text, '\n');
        size_t l = e ? (size_t)(e - text) : strlen(text);
        char line[128];
        memcpy(line, text, l < 127 ? l : 127);
        line[l < 127 ? l : 127] = 0;
        text += l + (e ? 1 : 0);
        if (line[0] == '*') { squeezing = 1; continue; }
        char *end;
        size_t off = strtoul(line, &end, 16);
        if (end - line != 8) return 0;
        if (squeezing) {
            while (out->n + prev_len <= off) bput(out, prev, prev_len);
            squeezing = 0;
        }
        if (off == out->n && line[8] == 0) return 1;        /* final offset-only line */
        if (off != out->n) return 0;
        size_t got = 0;
        for (char *p = line + 8; *p && *p != '|' && got < 16; ) {
            if (*p == ' ') { p++; continue; }
            int hi = hexv((unsigned char)p[0]), lo = hexv((unsigned char)p[1]);
            if (hi < 0 || lo < 0) break;
            prev[got++] = (unsigned char)(hi * 16 + lo);
            p += 2;
        }
        prev_len = got;
        bput(out, prev, got);
    }
    return 1;
}

int main(void) {
    unsigned char *img = malloc(70);
    size_t len = 70;
    for (size_t i = 0; i < len; i++) img[i] = (unsigned char)(i < 20 ? 'A' + i : rnd() >> 9);
    wfile("orig.bin", img, len);
    Buf full = {0};
    dump_rows(&full, img, len, 0, len);
    bbyte(&full, 0);
    printf("%s", (char *)full.p);
    /* 1. reversing the full dump reproduces the file exactly */
    unsigned char *re = NULL;
    size_t rl = 0;
    int lines, bw;
    CHECK(apply_dump((char *)full.p, &re, &rl, &lines, &bw));
    CHECK(rl == len && !memcmp(re, img, len));
    printf("full reverse: %d lines, %d bytes, identical\n", lines, bw);

    /* 2. edited dump applied as a patch to the file on disk */
    const char *patch =
        "# patch: overwrite two bytes and append a tail\n"
        "00000004: 4869\n"
        "00000010: 5858 5858  XXXX\n"
        "00000046: 2121 2121 21  !!!!!\n";
    size_t n;
    unsigned char *disk = rfile("orig.bin", &n);
    CHECK(apply_dump(patch, &disk, &n, &lines, &bw));
    wfile("patched.bin", disk, n);
    unsigned char *chk = rfile("patched.bin", &n);
    printf("patched: %zu bytes (was %zu), %d lines, %d bytes written\n", n, len, lines, bw);
    CHECK(n == 0x46 + 5 && chk[4] == 'H' && chk[5] == 'i' && chk[0x10] == 'X' && chk[0x13] == 'X');
    CHECK(memcmp(chk, img, 4) == 0 && memcmp(chk + 6, img + 6, 10) == 0);
    Buf show = {0};
    dump_rows(&show, chk, n, 0, 16);
    dump_rows(&show, chk, n, 64, n);
    bbyte(&show, 0);
    printf("%s", (char *)show.p);

    /* 3. sparse: writing at a far offset zero-fills the gap */
    unsigned char *sp = NULL;
    size_t sl = 0;
    CHECK(apply_dump("00000100: ff\n", &sp, &sl, &lines, &bw));
    size_t zeros = 0;
    for (size_t i = 0; i < sl; i++) zeros += sp[i] == 0;
    CHECK(sl == 0x101 && zeros == 0x100);
    printf("sparse: %zu bytes, %zu zero-filled\n", sl, zeros);
    free(sp);

    /* 4. plain hex and canonical hexdump -C with squeezing */
    Buf pl = {0};
    CHECK(from_plain("48 65 6c 6c\n6f2c20776f726c64 0a", &pl));
    printf("plain hex -> %.*s", (int)pl.n, (char *)pl.p);
    CHECK(!from_plain("abc", &pl) && !from_plain("zz", &pl));
    const char *canon =
        "00000000  61 62 63 64 65 66 67 68  69 6a 6b 6c 6d 6e 6f 70  |abcdefghijklmnop|\n"
        "00000010  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|\n"
        "*\n"
        "00000050  71 72 73 74                                       |qrst|\n"
        "00000054\n";
    Buf cb = {0};
    CHECK(from_canonical(canon, &cb));
    size_t nz = 0;
    for (size_t i = 0; i < cb.n; i++) nz += cb.p[i] == 0;
    CHECK(cb.n == 0x54 && nz == 0x40);
    printf("canonical with '*': %zu bytes, %zu zero bytes, tail %.4s\n", cb.n, nz, (char *)cb.p + 0x50);
    /* 5. malformed dumps */
    static const char *bad[] = {"zz: 00\n", "0000: 0g\n", "no colon here\n", "00000000: 4\n"};
    for (int i = 0; i < 4; i++) {
        unsigned char *x = NULL;
        size_t xl = 0;
        int ok = apply_dump(bad[i], &x, &xl, &lines, &bw);
        printf("bad %d: %s\n", i, ok ? "accepted" : "rejected");
        free(x);
    }
    free(img); free(re); free(disk); free(chk); bfree(&full); bfree(&show); bfree(&pl); bfree(&cb);
    remove("orig.bin"); remove("patched.bin");
    return 0;
}
