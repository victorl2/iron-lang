/*
 * title: ZIP stored-method writer and central directory reader
 * topic: io_files
 * covers: zip, crc32, local headers, central directory, end of central directory, extraction, tamper detection
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

static uint32_t crc_tab[256];
void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
}
uint32_t crc32_update(uint32_t crc, const void *buf, size_t n) {
    const unsigned char *p = buf;
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_tab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
uint32_t crc32_of(const void *buf, size_t n) { return crc32_update(0, buf, n); }

static void le16(Buf *b, unsigned v) { bbyte(b, v & 255u); bbyte(b, (v >> 8) & 255u); }
static void le32(Buf *b, uint32_t v) { le16(b, v & 0xFFFFu); le16(b, v >> 16); }
static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }

typedef struct { const char *name; const unsigned char *data; size_t len; } Src;

static void write_zip(Buf *z, const Src *src, int n) {
    uint32_t offsets[16], crcs[16];
    for (int i = 0; i < n; i++) {
        size_t nl = strlen(src[i].name);
        offsets[i] = (uint32_t)z->n;
        crcs[i] = crc32_of(src[i].data, src[i].len);
        le32(z, 0x04034B50u);
        le16(z, 10);            /* version needed */
        le16(z, 0);             /* flags */
        le16(z, 0);             /* method: stored */
        le16(z, 0x6000);        /* dos time 12:00:00 */
        le16(z, 0x5A21);        /* dos date 2025-01-01 */
        le32(z, crcs[i]);
        le32(z, (uint32_t)src[i].len);
        le32(z, (uint32_t)src[i].len);
        le16(z, (unsigned)nl);
        le16(z, 0);
        bput(z, src[i].name, nl);
        bput(z, src[i].data, src[i].len);
    }
    uint32_t cd_start = (uint32_t)z->n;
    for (int i = 0; i < n; i++) {
        size_t nl = strlen(src[i].name);
        le32(z, 0x02014B50u);
        le16(z, 20); le16(z, 10); le16(z, 0); le16(z, 0);
        le16(z, 0x6000); le16(z, 0x5A21);
        le32(z, crcs[i]);
        le32(z, (uint32_t)src[i].len); le32(z, (uint32_t)src[i].len);
        le16(z, (unsigned)nl); le16(z, 0); le16(z, 0);
        le16(z, 0); le16(z, 0);
        le32(z, 0);
        le32(z, offsets[i]);
        bput(z, src[i].name, nl);
    }
    uint32_t cd_size = (uint32_t)z->n - cd_start;
    le32(z, 0x06054B50u);
    le16(z, 0); le16(z, 0);
    le16(z, (unsigned)n); le16(z, (unsigned)n);
    le32(z, cd_size);
    le32(z, cd_start);
    le16(z, 0);
}

/* Returns entry count or -1; verifies every entry, extracting into out[] */
static int read_zip(const unsigned char *d, size_t n, char names[][64], size_t *lens, const unsigned char **datas, const char **err) {
    if (n < 22) { *err = "too short"; return -1; }
    size_t e = n - 22;
    while (rd32(d + e) != 0x06054B50u) {
        if (e == 0) { *err = "no end-of-central-directory"; return -1; }
        e--;
    }
    unsigned count = rd16(d + e + 10);
    uint32_t cd_size = rd32(d + e + 12), cd_off = rd32(d + e + 16);
    if ((size_t)cd_off + cd_size > e) { *err = "central directory out of range"; return -1; }
    size_t p = cd_off;
    for (unsigned i = 0; i < count; i++) {
        if (p + 46 > n || rd32(d + p) != 0x02014B50u) { *err = "bad central header"; return -1; }
        unsigned method = rd16(d + p + 10);
        uint32_t crc = rd32(d + p + 16), csz = rd32(d + p + 20), usz = rd32(d + p + 24);
        unsigned nl = rd16(d + p + 28), xl = rd16(d + p + 30), cl = rd16(d + p + 32);
        uint32_t lho = rd32(d + p + 42);
        if (method != 0) { *err = "unsupported method"; return -1; }
        if (csz != usz) { *err = "size mismatch"; return -1; }
        if (nl >= 64) { *err = "name too long"; return -1; }
        memcpy(names[i], d + p + 46, nl);
        names[i][nl] = 0;
        /* follow the local header, cross-check the name */
        if ((size_t)lho + 30 > n || rd32(d + lho) != 0x04034B50u) { *err = "bad local header"; return -1; }
        unsigned lnl = rd16(d + lho + 26), lxl = rd16(d + lho + 28);
        if (lnl != nl || memcmp(d + lho + 30, names[i], nl)) { *err = "local/central name mismatch"; return -1; }
        size_t ds = (size_t)lho + 30 + lnl + lxl;
        if (ds + usz > n) { *err = "data out of range"; return -1; }
        if (crc32_of(d + ds, usz) != crc) { *err = "crc mismatch"; return -1; }
        lens[i] = usz;
        datas[i] = d + ds;
        p += 46 + nl + xl + cl;
    }
    return (int)count;
}

int main(void) {
    crc_init();
    CHECK(crc32_of("123456789", 9) == 0xCBF43926u);
    static unsigned char blob[5][700];
    static const size_t lens[5] = {0, 13, 700, 256, 91};
    static const char *names[5] = {"empty.txt", "hello.txt", "dir/random.bin", "dir/sub/ramp.bin", "README"};
    for (int i = 0; i < 5; i++)
        for (size_t k = 0; k < lens[i]; k++)
            blob[i][k] = i == 1 ? (unsigned char)"hello, world\n"[k] : i == 3 ? (unsigned char)k : (unsigned char)(rnd() >> 13);
    Src src[5];
    for (int i = 0; i < 5; i++) { src[i].name = names[i]; src[i].data = blob[i]; src[i].len = lens[i]; }
    Buf z = {0};
    write_zip(&z, src, 5);
    wfile("t.zip", z.p, z.n);
    size_t n;
    unsigned char *d = rfile("t.zip", &n);
    CHECK(n == z.n);
    char nm[16][64];
    size_t ln[16];
    const unsigned char *dp[16];
    const char *err = "";
    int cnt = read_zip(d, n, nm, ln, dp, &err);
    CHECK(cnt == 5);
    printf("zip: %zu bytes, %d entries, EOCD at %zu\n", n, cnt, n - 22);
    for (int i = 0; i < cnt; i++) {
        CHECK(strcmp(nm[i], names[i]) == 0 && ln[i] == lens[i] && memcmp(dp[i], blob[i], lens[i]) == 0);
        printf("  %-18s %4zu bytes crc=%08X\n", nm[i], ln[i], (unsigned)crc32_of(dp[i], ln[i]));
    }
    /* tampering: flip one byte in each region and see which check fires */
    size_t local0 = 30 + strlen(names[0]);
    size_t data2 = local0 + 30 + strlen(names[1]) + 13 + 30 + strlen(names[2]) + 100;
    size_t cd_off = rd32(d + n - 6);
    size_t targets[] = {data2, cd_off + 16, cd_off + 10, cd_off + 46 + 2, 0, n - 6};
    static const char *labels[] = {"payload byte", "central crc field", "central method", "central name", "local signature", "cd offset field"};
    for (int i = 0; i < 6; i++) {
        unsigned char *c = malloc(n);
        memcpy(c, d, n);
        c[targets[i]] ^= 0x40;
        int r = read_zip(c, n, nm, ln, dp, &err);
        CHECK(r < 0);
        printf("tamper %-18s -> %s\n", labels[i], err);
        free(c);
    }
    CHECK(read_zip(d, n - 10, nm, ln, dp, &err) < 0);
    printf("truncated tail -> %s\n", err);
    free(d); bfree(&z);
    remove("t.zip");
    return 0;
}
