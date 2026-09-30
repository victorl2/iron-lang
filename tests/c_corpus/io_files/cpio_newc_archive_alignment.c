/*
 * title: cpio newc archive writer and reader
 * topic: io_files
 * covers: cpio newc ascii headers, hex fields, 4-byte alignment, TRAILER!!!, crc variant checksum, walking with padding
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

typedef struct { const char *name; unsigned mode; const unsigned char *data; size_t len; } File;
typedef struct { char name[64]; unsigned mode, ino, nlink; size_t len; const unsigned char *data; } Got;

static size_t pad4(size_t n) { return (4 - (n & 3)) & 3; }

static void hex8(Buf *b, uint32_t v) {
    char t[16];
    snprintf(t, sizeof t, "%08X", (unsigned)v);
    bstr(b, t);
}

static uint32_t sum_bytes(const unsigned char *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s;
}

static void add(Buf *b, const char *name, unsigned ino, unsigned mode, const unsigned char *data, size_t len, int crc_variant) {
    size_t nl = strlen(name) + 1;
    bstr(b, crc_variant ? "070702" : "070701");
    hex8(b, ino); hex8(b, mode); hex8(b, 1000); hex8(b, 1000);
    hex8(b, 1);                 /* nlink */
    hex8(b, 1700000000u);       /* mtime */
    hex8(b, (uint32_t)len);
    hex8(b, 8); hex8(b, 1);     /* dev major/minor */
    hex8(b, 0); hex8(b, 0);     /* rdev */
    hex8(b, (uint32_t)nl);
    hex8(b, crc_variant ? sum_bytes(data, len) : 0);
    bput(b, name, nl);
    for (size_t k = 0; k < pad4(110 + nl); k++) bbyte(b, 0);
    bput(b, data, len);
    for (size_t k = 0; k < pad4(len); k++) bbyte(b, 0);
}

static int parse_hex(const unsigned char *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        int c = p[i], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else return 0;
        v = v * 16 + (uint32_t)d;
    }
    *out = v;
    return 1;
}

static int read_cpio(const unsigned char *d, size_t n, Got *g, int max, const char **err) {
    size_t pos = 0;
    int cnt = 0;
    for (;;) {
        if (pos + 110 > n) { *err = "truncated header"; return -1; }
        int crcv;
        if (!memcmp(d + pos, "070701", 6)) crcv = 0;
        else if (!memcmp(d + pos, "070702", 6)) crcv = 1;
        else { *err = "bad magic"; return -1; }
        uint32_t f[13];
        for (int i = 0; i < 13; i++)
            if (!parse_hex(d + pos + 6 + 8 * (size_t)i, &f[i])) { *err = "bad hex field"; return -1; }
        /* f: ino mode uid gid nlink mtime filesize devmaj devmin rdevmaj rdevmin namesize check */
        size_t nl = f[11], len = f[6];
        size_t name_at = pos + 110;
        if (nl == 0 || name_at + nl > n) { *err = "bad name size"; return -1; }
        if (d[name_at + nl - 1] != 0) { *err = "name not NUL terminated"; return -1; }
        size_t data_at = name_at + nl + pad4(110 + nl);
        if (data_at + len > n) { *err = "data out of range"; return -1; }
        if (strcmp((const char *)d + name_at, "TRAILER!!!") == 0) return cnt;
        if (crcv && sum_bytes(d + data_at, len) != f[12]) { *err = "data checksum mismatch"; return -1; }
        if (cnt >= max || nl > 63) { *err = "too many or too long"; return -1; }
        memcpy(g[cnt].name, d + name_at, nl);
        g[cnt].ino = f[0]; g[cnt].mode = f[1]; g[cnt].nlink = f[4]; g[cnt].len = len;
        g[cnt].data = d + data_at;
        cnt++;
        pos = data_at + len + pad4(len);
    }
}

int main(void) {
    static unsigned char blobs[6][40];
    static const size_t lens[6] = {0, 1, 2, 3, 4, 37};
    static const char *names[6] = {"a", "bb", "ccc", "dddd", "usr/bin/tool", "etc/config/very/long/name.conf"};
    File files[6];
    for (int i = 0; i < 6; i++) {
        for (size_t k = 0; k < lens[i]; k++) blobs[i][k] = (unsigned char)(rnd() >> 8);
        files[i].name = names[i]; files[i].mode = i == 4 ? 0100755 : 0100644;
        files[i].data = blobs[i]; files[i].len = lens[i];
    }
    for (int variant = 0; variant < 2; variant++) {
        Buf b = {0};
        for (int i = 0; i < 6; i++) add(&b, files[i].name, 100 + (unsigned)i, files[i].mode, files[i].data, files[i].len, variant);
        add(&b, "TRAILER!!!", 0, 0, NULL, 0, variant);
        CHECK(b.n % 4 == 0);
        wfile("a.cpio", b.p, b.n);
        size_t n;
        unsigned char *d = rfile("a.cpio", &n);
        Got g[8];
        const char *err = "";
        int cnt = read_cpio(d, n, g, 8, &err);
        CHECK(cnt == 6);
        printf("variant %s: %zu bytes, %d files\n", variant ? "crc (070702)" : "newc (070701)", n, cnt);
        for (int i = 0; i < cnt; i++) {
            CHECK(!strcmp(g[i].name, names[i]) && g[i].len == lens[i] && !memcmp(g[i].data, blobs[i], lens[i]));
            CHECK(g[i].mode == files[i].mode);
            if (variant == 0) {
                size_t nl = strlen(names[i]) + 1;
                printf("  ino=%u mode=%06o len=%2zu name-pad=%zu data-pad=%zu %s\n", g[i].ino, g[i].mode, g[i].len,
                       pad4(110 + nl), pad4(g[i].len), g[i].name);
            }
        }
        if (variant == 1) {
            /* corrupt a data byte: only the crc variant can notice */
            size_t at = 0;
            for (size_t i = 110; i + 4 < n; i++)
                if (!memcmp(d + i, blobs[5], 8)) { at = i; break; }
            CHECK(at);
            d[at + 3] ^= 0x10;
            int r = read_cpio(d, n, g, 8, &err);
            CHECK(r < 0);
            printf("corrupt payload: %s\n", err);
            d[at + 3] ^= 0x10;
        } else {
            unsigned char *c = malloc(n);
            memcpy(c, d, n);
            c[6 + 8 * 6 + 2] = 'Z';
            CHECK(read_cpio(c, n, g, 8, &err) < 0);
            printf("bad filesize digit: %s\n", err);
            memcpy(c, d, n);
            c[0] = '7';
            c[5] = '9';
            CHECK(read_cpio(c, n, g, 8, &err) < 0);
            printf("bad magic: %s\n", err);
            CHECK(read_cpio(d, n - 20, g, 8, &err) < 0);
            printf("cut trailer: %s\n", err);
            free(c);
        }
        free(d); bfree(&b);
    }
    remove("a.cpio");
    return 0;
}
