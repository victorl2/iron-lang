/*
 * title: Versioned binary file format with endian flag and migration
 * topic: io_files
 * covers: binary serialization, magic, version migration v1/v2/v3, endian marker, explicit byte order, header crc, forward-compat skip
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

/* One logical record type evolves over three file versions.
 *  v1: id(u32) score(i32)
 *  v2: id(u32) score(i32) flags(u16)
 *  v3: id(u32) score_x100(i64) flags(u16) name_len(u8) name   (fixed-point score, names)
 * Header: "IRNF" magic, u8 version, u8 endian ('L'/'B'), u16 header_len, u32 count, u32 header crc. */
typedef struct { uint32_t id; int64_t score_x100; uint16_t flags; char name[16]; } Rec;

static void put(Buf *b, int be, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        int sh = be ? 8 * (bytes - 1 - i) : 8 * i;
        bbyte(b, (unsigned)((v >> sh) & 255u));
    }
}
static uint64_t get(const unsigned char *p, int be, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) {
        int sh = be ? 8 * (bytes - 1 - i) : 8 * i;
        v |= (uint64_t)p[i] << sh;
    }
    return v;
}

static void write_file(Buf *b, const Rec *r, int n, int version, int be) {
    bstr(b, "IRNF");
    bbyte(b, (unsigned)version);
    bbyte(b, be ? 'B' : 'L');
    put(b, be, 16 + (version >= 3 ? 4 : 0), 2);    /* v3 headers carry 4 extra reserved bytes */
    put(b, be, (uint64_t)n, 4);
    uint32_t c = crc32_of(b->p, b->n);
    put(b, be, c, 4);
    if (version >= 3) put(b, be, 0xC0FFEEu, 4);    /* reserved: readers must skip by header_len */
    for (int i = 0; i < n; i++) {
        put(b, be, r[i].id, 4);
        if (version == 1) put(b, be, (uint64_t)(uint32_t)(int32_t)(r[i].score_x100 / 100), 4);
        else if (version == 2) { put(b, be, (uint64_t)(uint32_t)(int32_t)(r[i].score_x100 / 100), 4); put(b, be, r[i].flags, 2); }
        else {
            put(b, be, (uint64_t)r[i].score_x100, 8);
            put(b, be, r[i].flags, 2);
            size_t nl = strlen(r[i].name);
            bbyte(b, (unsigned)nl);
            bput(b, r[i].name, nl);
        }
    }
}

static const char *read_file(const unsigned char *d, size_t n, Rec *out, int max, int *count, int *version, int *be_out) {
    if (n < 16 || memcmp(d, "IRNF", 4)) return "bad magic";
    *version = d[4];
    if (*version < 1 || *version > 3) return "unknown version";
    if (d[5] != 'L' && d[5] != 'B') return "bad endian marker";
    int be = d[5] == 'B';
    *be_out = be;
    size_t hl = (size_t)get(d + 6, be, 2);
    if (hl < 16 || hl > n) return "bad header length";
    *count = (int)get(d + 8, be, 4);
    if (crc32_of(d, 12) != (uint32_t)get(d + 12, be, 4)) return "header crc mismatch";
    if (*count > max) return "too many records";
    size_t p = hl;
    for (int i = 0; i < *count; i++) {
        Rec *r = &out[i];
        memset(r, 0, sizeof *r);
        size_t need = *version == 1 ? 8 : *version == 2 ? 10 : 15;
        if (p + need > n) return "truncated record";
        r->id = (uint32_t)get(d + p, be, 4);
        if (*version < 3) {
            int32_t s = (int32_t)(uint32_t)get(d + p + 4, be, 4);
            r->score_x100 = (int64_t)s * 100;
            if (*version == 2) r->flags = (uint16_t)get(d + p + 8, be, 2);
            p += need;
        } else {
            r->score_x100 = (int64_t)get(d + p + 4, be, 8);
            r->flags = (uint16_t)get(d + p + 12, be, 2);
            size_t nl = d[p + 14];
            if (nl >= sizeof r->name || p + 15 + nl > n) return "bad name length";
            memcpy(r->name, d + p + 15, nl);
            p += 15 + nl;
        }
    }
    if (p != n) return "trailing bytes";
    return NULL;
}

int main(void) {
    crc_init();
    Rec r[12];
    static const char *names[] = {"ann", "bob", "carol", "dave-the-long", "", "eve"};
    for (int i = 0; i < 12; i++) {
        r[i].id = 1000 + (uint32_t)i * 7;
        r[i].score_x100 = ((int64_t)(rnd() % 2000001) - 1000000) * 100;
        r[i].flags = (uint16_t)(rnd() & 0xFFFF);
        snprintf(r[i].name, sizeof r[i].name, "%s", names[i % 6]);
    }
    for (int version = 1; version <= 3; version++)
        for (int be = 0; be < 2; be++) {
            Buf b = {0};
            write_file(&b, r, 12, version, be);
            wfile("data.irn", b.p, b.n);
            size_t n;
            unsigned char *d = rfile("data.irn", &n);
            Rec back[12];
            int count, ver, ebe;
            const char *e = read_file(d, n, back, 12, &count, &ver, &ebe);
            CHECK(e == NULL && count == 12 && ver == version && ebe == be);
            for (int i = 0; i < 12; i++) {
                CHECK(back[i].id == r[i].id && back[i].score_x100 == r[i].score_x100);
                if (version >= 2) CHECK(back[i].flags == r[i].flags);
                if (version >= 3) CHECK(!strcmp(back[i].name, r[i].name));
            }
            printf("v%d %s-endian: %3zu bytes, first header bytes:", version, be ? "big   " : "little", n);
            for (int i = 0; i < 12; i++) printf(" %02X", d[i]);
            printf("\n      rec[3] id=%u score=%lld.%02lld flags=%04X name=\"%s\"\n", (unsigned)back[3].id,
                   (long long)(back[3].score_x100 / 100), (long long)(back[3].score_x100 < 0 ? -back[3].score_x100 : back[3].score_x100) % 100,
                   (unsigned)back[3].flags, back[3].name);
            free(d); bfree(&b);
        }
    /* damage */
    Buf b = {0};
    write_file(&b, r, 12, 3, 0);
    unsigned char *c = malloc(b.n);
    struct { size_t off; unsigned char val; } dm[] = {{0, 'X'}, {4, 9}, {5, 'Q'}, {8, 99}, {13, 0}, {6, 200}};
    for (int i = 0; i < 6; i++) {
        memcpy(c, b.p, b.n);
        c[dm[i].off] = dm[i].val;
        Rec back[12];
        int cnt, ver, be;
        const char *e = read_file(c, b.n, back, 12, &cnt, &ver, &be);
        CHECK(e);
        printf("damage at %zu: %s\n", dm[i].off, e);
    }
    Rec back[12];
    int cnt, ver, be;
    const char *e = read_file(b.p, b.n - 3, back, 12, &cnt, &ver, &be);
    CHECK(e);
    printf("truncated by 3: %s\n", e);
    free(c); bfree(&b);
    remove("data.irn");
    return 0;
}
