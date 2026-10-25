/*
 * title: PEM and OpenPGP-style armored blocks with CRC-24
 * topic: io_files
 * covers: ascii armor, BEGIN/END labels, header lines, line wrapping at 64/76, base64 padding cases, crc24 checksum line, multiple blocks per file, tolerant reader
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

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(Buf *o, const unsigned char *d, size_t n, size_t wrap) {
    size_t col = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16;
        if (i + 1 < n) v |= (uint32_t)d[i + 1] << 8;
        if (i + 2 < n) v |= d[i + 2];
        char q[4] = {B64[(v >> 18) & 63], B64[(v >> 12) & 63], i + 1 < n ? B64[(v >> 6) & 63] : '=', i + 2 < n ? B64[v & 63] : '='};
        for (int k = 0; k < 4; k++) {
            bbyte(o, (unsigned char)q[k]);
            if (++col == wrap) { bbyte(o, '\n'); col = 0; }
        }
    }
    if (col) bbyte(o, '\n');
}

/* Returns decoded length, or -1 on bad character / bad padding / bad length. Whitespace ignored. */
static long b64_decode(const char *s, size_t n, unsigned char *out) {
    uint32_t acc = 0;
    int bits = 0, pad = 0;
    size_t o = 0, sym = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (c == '=') { pad++; sym++; continue; }
        if (pad) return -1;                 /* data after padding */
        const char *p = strchr(B64, c);
        if (!p || !c) return -1;
        acc = (acc << 6) | (uint32_t)(p - B64);
        bits += 6;
        sym++;
        if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)((acc >> bits) & 255u); }
    }
    if (sym % 4 || pad > 2) return -1;
    if ((pad == 1 && bits != 2) || (pad == 2 && bits != 4) || (pad == 0 && bits != 0)) return -1;
    if (bits && (acc & ((1u << bits) - 1)) != 0) return -1;   /* non-zero trailing bits */
    return (long)o;
}

/* OpenPGP CRC-24: init 0xB704CE, poly 0x864CFB */
static uint32_t crc24(const unsigned char *d, size_t n) {
    uint32_t crc = 0xB704CEu;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)d[i] << 16;
        for (int k = 0; k < 8; k++) {
            crc <<= 1;
            if (crc & 0x1000000u) crc ^= 0x1864CFBu;
        }
    }
    return crc & 0xFFFFFFu;
}

static void write_block(Buf *o, const char *label, const char *const *hdrs, int nh, const unsigned char *d, size_t n, size_t wrap, int with_crc) {
    bstr(o, "-----BEGIN "); bstr(o, label); bstr(o, "-----\n");
    for (int i = 0; i < nh; i++) { bstr(o, hdrs[i]); bbyte(o, '\n'); }
    if (nh) bbyte(o, '\n');
    b64_encode(o, d, n, wrap);
    if (with_crc) {
        uint32_t c = crc24(d, n);
        unsigned char cb[3] = {(unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c};
        bbyte(o, '=');
        Buf t = {0};
        b64_encode(&t, cb, 3, 100);
        bput(o, t.p, t.n);
        bfree(&t);
    }
    bstr(o, "-----END "); bstr(o, label); bstr(o, "-----\n");
}

typedef struct { char label[32]; char hdr[3][40]; int nh; unsigned char data[512]; size_t len; int crc_state; /* 0 none, 1 ok, 2 bad */ } Block;

/* Scan text for blocks; returns count or -1 with err */
static int read_blocks(const char *text, Block *out, int max, const char **err) {
    int cnt = 0;
    const char *p = text;
    while ((p = strstr(p, "-----BEGIN ")) != NULL) {
        p += 11;
        const char *e = strstr(p, "-----");
        if (!e || e - p >= 32) { *err = "bad BEGIN line"; return -1; }
        if (cnt >= max) { *err = "too many blocks"; return -1; }
        Block *b = &out[cnt];
        memset(b, 0, sizeof *b);
        memcpy(b->label, p, (size_t)(e - p));
        p = e + 5;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        char endline[64];
        snprintf(endline, sizeof endline, "-----END %s-----", b->label);
        const char *end = strstr(p, endline);
        if (!end) { *err = "missing END line"; return -1; }
        /* header lines: "Key: value" until blank line */
        const char *q = p;
        while (q < end) {
            const char *nl = memchr(q, '\n', (size_t)(end - q));
            if (!nl) break;
            const char *colon = memchr(q, ':', (size_t)(nl - q));
            if (!colon) break;
            if (b->nh < 3) snprintf(b->hdr[b->nh], sizeof b->hdr[0], "%.*s", (int)(nl - q), q);
            b->nh++;
            q = nl + 1;
        }
        if (b->nh && q < end && *q == '\n') q++;
        /* body, optionally followed by =CRC line */
        const char *body_end = end;
        const char *eq = NULL;
        for (const char *t = q; t < end; t++) if (*t == '=' && (t == q || t[-1] == '\n') && end - t <= 7) eq = t;
        if (eq) body_end = eq;
        long l = b64_decode(q, (size_t)(body_end - q), b->data);
        if (l < 0) { *err = "bad base64 body"; return -1; }
        b->len = (size_t)l;
        if (eq) {
            unsigned char cb[8];
            long cl = b64_decode(eq + 1, (size_t)(end - eq - 1), cb);
            if (cl != 3) { *err = "bad crc line"; return -1; }
            uint32_t want = ((uint32_t)cb[0] << 16) | ((uint32_t)cb[1] << 8) | cb[2];
            b->crc_state = want == crc24(b->data, b->len) ? 1 : 2;
        }
        cnt++;
        p = end + strlen(endline);
    }
    return cnt;
}

int main(void) {
    CHECK(crc24((const unsigned char *)"123456789", 9) == 0x21CF02u);
    /* base64 padding cases from RFC 4648 */
    static const char *vec[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *want[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        Buf b = {0};
        b64_encode(&b, (const unsigned char *)vec[i], strlen(vec[i]), 76);
        bbyte(&b, 0);
        char *got = (char *)b.p;
        size_t gl = strlen(got);
        if (gl && got[gl - 1] == '\n') got[gl - 1] = 0;
        CHECK(!strcmp(got, want[i]));
        unsigned char back[16];
        CHECK(b64_decode(want[i], strlen(want[i]), back) == (long)strlen(vec[i]));
        bfree(&b);
    }
    printf("rfc4648 vectors ok\n");
    static unsigned char blobs[3][300];
    static const size_t lens[3] = {32, 200, 3};
    for (int i = 0; i < 3; i++) for (size_t k = 0; k < lens[i]; k++) blobs[i][k] = (unsigned char)(rnd() >> 6);
    static const char *h2[] = {"Proc-Type: 4,ENCRYPTED", "DEK-Info: AES-128-CBC,00112233445566778899AABBCCDDEEFF"};
    Buf f = {0};
    write_block(&f, "CERTIFICATE", NULL, 0, blobs[0], lens[0], 64, 0);
    bstr(&f, "some prose between blocks\n");
    write_block(&f, "RSA PRIVATE KEY", h2, 2, blobs[1], lens[1], 64, 0);
    write_block(&f, "PGP MESSAGE", NULL, 0, blobs[2], lens[2], 76, 1);
    bbyte(&f, 0);
    wfile("bundle.pem", f.p, f.n - 1);
    printf("%s", (char *)f.p);
    size_t n;
    unsigned char *raw = rfile("bundle.pem", &n);
    char *text = malloc(n + 1);
    memcpy(text, raw, n);
    text[n] = 0;
    Block bl[6];
    const char *err = "";
    int c = read_blocks(text, bl, 6, &err);
    CHECK(c == 3);
    for (int i = 0; i < c; i++) {
        CHECK(bl[i].len == lens[i] && !memcmp(bl[i].data, blobs[i], lens[i]));
        printf("block %d: label=\"%s\" headers=%d payload=%zu bytes crc=%s\n", i, bl[i].label, bl[i].nh, bl[i].len,
               bl[i].crc_state == 0 ? "absent" : bl[i].crc_state == 1 ? "ok" : "BAD");
    }
    CHECK(bl[2].crc_state == 1);
    /* corrupt one body char of the PGP block: crc must flag it */
    char *mark = strstr(text, "-----BEGIN PGP MESSAGE-----\n");
    CHECK(mark);
    mark[28] = mark[28] == 'A' ? 'B' : 'A';
    c = read_blocks(text, bl, 6, &err);
    CHECK(c == 3 && bl[2].crc_state == 2);
    printf("corrupted body: crc=%s\n", bl[2].crc_state == 2 ? "BAD" : "ok");
    /* bad base64 forms */
    static const char *badb[] = {"Zg=", "Zg==Zg==", "Z$==", "Zh==", "Zm9vY", "===="};
    unsigned char tmp[16];
    for (int i = 0; i < 6; i++) printf("b64 %-9s -> %ld\n", badb[i], b64_decode(badb[i], strlen(badb[i]), tmp));
    const char *nofinal = "-----BEGIN X-----\nAAAA\n";
    CHECK(read_blocks(nofinal, bl, 6, &err) < 0);
    printf("missing END: %s\n", err);
    free(text); free(raw); bfree(&f);
    remove("bundle.pem");
    return 0;
}
