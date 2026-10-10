/*
 * title: TAR ustar writer and reader with header checksums
 * topic: io_files
 * covers: tar, octal fields, 512-byte blocks, checksum with blank field, prefix/name split, end-of-archive blocks, corruption
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

enum { BLK = 512 };

static void put_octal(unsigned char *dst, size_t width, unsigned long v) {
    /* width-1 octal digits, zero padded, then NUL */
    for (size_t i = 0; i < width - 1; i++) {
        dst[width - 2 - i] = (unsigned char)('0' + (v & 7));
        v >>= 3;
    }
    dst[width - 1] = 0;
}

static unsigned long get_octal(const unsigned char *src, size_t width, int *ok) {
    unsigned long v = 0;
    size_t i = 0;
    while (i < width && src[i] == ' ') i++;
    for (; i < width && src[i] >= '0' && src[i] <= '7'; i++) v = v * 8 + (src[i] - '0');
    for (; i < width; i++) if (src[i] != 0 && src[i] != ' ') *ok = 0;
    return v;
}

static unsigned header_sum(const unsigned char *h) {
    unsigned s = 0;
    for (int i = 0; i < BLK; i++) s += (i >= 148 && i < 156) ? ' ' : h[i];
    return s;
}

/* Split a long path into prefix (<=155) and name (<=100) at a slash. */
static int split_path(const char *path, char *prefix, char *name) {
    size_t n = strlen(path);
    prefix[0] = 0;
    if (n <= 100) { strcpy(name, path); return 1; }
    for (size_t i = n; i-- > 0;) {
        if (path[i] == '/' && i <= 155 && n - i - 1 <= 100 && n - i - 1 > 0) {
            memcpy(prefix, path, i); prefix[i] = 0;
            strcpy(name, path + i + 1);
            return 1;
        }
    }
    return 0;
}

static int add_entry(Buf *out, const char *path, char type, unsigned mode, const void *data, size_t len, const char *link) {
    unsigned char h[BLK];
    char prefix[156], name[101];
    memset(h, 0, BLK);
    if (!split_path(path, prefix, name)) return 0;
    memcpy(h, name, strlen(name));
    put_octal(h + 100, 8, mode);
    put_octal(h + 108, 8, 1000);
    put_octal(h + 116, 8, 1000);
    put_octal(h + 124, 12, type == '0' ? len : 0);
    put_octal(h + 136, 12, 1700000000UL);
    memset(h + 148, ' ', 8);
    h[156] = (unsigned char)type;
    if (link) memcpy(h + 157, link, strlen(link));
    memcpy(h + 257, "ustar", 5);
    h[263] = '0'; h[264] = '0';
    memcpy(h + 265, "user", 4);
    memcpy(h + 297, "group", 5);
    memcpy(h + 345, prefix, strlen(prefix));
    unsigned sum = header_sum(h);
    put_octal(h + 148, 7, sum);
    h[155] = ' ';
    bput(out, h, BLK);
    if (type == '0') {
        bput(out, data, len);
        for (size_t k = len; k % BLK; k++) bbyte(out, 0);
    }
    return 1;
}

typedef struct { char path[300]; char type; unsigned mode; size_t size; const unsigned char *data; int sum_ok; } Ent;

static int read_tar(const unsigned char *d, size_t n, Ent *ents, int max, const char **err) {
    size_t pos = 0;
    int cnt = 0, zero_blocks = 0;
    while (pos + BLK <= n) {
        const unsigned char *h = d + pos;
        int allzero = 1;
        for (int i = 0; i < BLK; i++) if (h[i]) { allzero = 0; break; }
        if (allzero) { zero_blocks++; pos += BLK; if (zero_blocks == 2) return cnt; continue; }
        if (zero_blocks) { *err = "data after zero block"; return -1; }
        int ok = 1;
        unsigned stored = (unsigned)get_octal(h + 148, 8, &ok);
        if (!ok || stored != header_sum(h)) { *err = "checksum mismatch"; return -1; }
        if (memcmp(h + 257, "ustar", 5)) { *err = "missing ustar magic"; return -1; }
        if (cnt >= max) { *err = "too many entries"; return -1; }
        Ent *e = &ents[cnt];
        char prefix[156], name[101];
        memcpy(prefix, h + 345, 155); prefix[155] = 0;
        memcpy(name, h, 100); name[100] = 0;
        if (prefix[0]) snprintf(e->path, sizeof e->path, "%s/%s", prefix, name);
        else snprintf(e->path, sizeof e->path, "%s", name);
        e->type = (char)h[156];
        e->mode = (unsigned)get_octal(h + 100, 8, &ok);
        e->size = get_octal(h + 124, 12, &ok);
        e->sum_ok = 1;
        pos += BLK;
        if (e->type == '0') {
            size_t padded = (e->size + BLK - 1) / BLK * BLK;
            if (pos + padded > n) { *err = "truncated data"; return -1; }
            e->data = d + pos;
            pos += padded;
        } else e->data = NULL;
        cnt++;
    }
    *err = "missing end-of-archive";
    return -1;
}

int main(void) {
    Buf tar = {0};
    static unsigned char blobs[6][1500];
    static const size_t sizes[6] = {0, 1, 511, 512, 513, 1500};
    for (int i = 0; i < 6; i++) for (size_t k = 0; k < sizes[i]; k++) blobs[i][k] = (unsigned char)(rnd() >> 9);
    char longpath[200];
    snprintf(longpath, sizeof longpath, "%s/%s/file.txt",
             "deeply/nested/directory/structure/that/goes/on/and/on/for/quite/a/while/before/ending",
             "still/more/components/to/push/the/full/path/past/one/hundred/bytes");
    CHECK(add_entry(&tar, "docs/", '5', 0755, NULL, 0, NULL));
    for (int i = 0; i < 6; i++) {
        char p[64];
        snprintf(p, sizeof p, "docs/blob%d.bin", i);
        CHECK(add_entry(&tar, p, '0', 0644, blobs[i], sizes[i], NULL));
    }
    CHECK(add_entry(&tar, "docs/link", '2', 0777, NULL, 0, "blob3.bin"));
    CHECK(add_entry(&tar, longpath, '0', 0600, "hello\n", 6, NULL));
    char toolong[300];
    memset(toolong, 'x', 299); toolong[299] = 0;
    CHECK(!add_entry(&tar, toolong, '0', 0644, "", 0, NULL));
    for (int i = 0; i < 2 * BLK; i++) bbyte(&tar, 0);
    CHECK(tar.n % BLK == 0);
    wfile("a.tar", tar.p, tar.n);
    size_t n;
    unsigned char *raw = rfile("a.tar", &n);
    static Ent ents[16];
    const char *err = "";
    int cnt = read_tar(raw, n, ents, 16, &err);
    CHECK(cnt == 9);
    printf("archive %zu bytes (%zu blocks), %d entries\n", n, n / BLK, cnt);
    for (int i = 0; i < cnt; i++) {
        printf("%c %04o %5zu %s%s\n", ents[i].type, ents[i].mode, ents[i].size,
               strlen(ents[i].path) > 60 ? "..." : "", ents[i].path + (strlen(ents[i].path) > 60 ? strlen(ents[i].path) - 40 : 0));
        if (ents[i].type == '0' && i >= 1 && i <= 6) {
            CHECK(ents[i].size == sizes[i - 1]);
            CHECK(memcmp(ents[i].data, blobs[i - 1], sizes[i - 1]) == 0);
        }
    }
    CHECK(strcmp(ents[8].path, longpath) == 0);
    /* corruption experiments */
    size_t offs[] = {0, 100, 148, 257, 512 + 10};
    for (int i = 0; i < 5; i++) {
        unsigned char *c = malloc(n);
        memcpy(c, raw, n);
        c[offs[i]] ^= 0x01;
        Ent tmp[16];
        int r = read_tar(c, n, tmp, 16, &err);
        printf("flip byte %3zu: %s\n", offs[i], r < 0 ? err : "not detected (data area)");
        free(c);
    }
    Ent tmp[16];
    CHECK(read_tar(raw, n - BLK, tmp, 16, &err) < 0);
    printf("one end block missing: %s\n", err);
    CHECK(read_tar(raw, n - 2 * BLK - 100, tmp, 16, &err) < 0);
    printf("cut mid-archive: %s\n", err);
    free(raw); bfree(&tar);
    remove("a.tar");
    return 0;
}
