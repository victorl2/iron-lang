/*
 * title: gzip container with stored deflate blocks, optional header fields and concatenated members
 * topic: io_files
 * covers: gzip rfc1952, FNAME FCOMMENT FEXTRA FHCRC, crc32 and isize trailer, stored blocks, multi-member streams, header validation
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

enum { FTEXT = 1, FHCRC = 2, FEXTRA = 4, FNAME = 8, FCOMMENT = 16 };

static void le16(Buf *b, unsigned v) { bbyte(b, v & 255u); bbyte(b, (v >> 8) & 255u); }
static void le32(Buf *b, uint32_t v) { le16(b, v & 0xFFFFu); le16(b, v >> 16); }
static uint32_t rd16(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return rd16(p) | (rd16(p + 2) << 16); }

typedef struct { const char *name, *comment; const unsigned char *extra; unsigned extra_len; int hcrc; uint32_t mtime; } Opts;

static void member(Buf *out, const unsigned char *data, size_t n, const Opts *o, size_t block) {
    size_t start = out->n;
    unsigned flags = (o->name ? FNAME : 0) | (o->comment ? FCOMMENT : 0) | (o->extra ? FEXTRA : 0) | (o->hcrc ? FHCRC : 0);
    bbyte(out, 0x1F); bbyte(out, 0x8B); bbyte(out, 8); bbyte(out, flags);
    le32(out, o->mtime);
    bbyte(out, 0); bbyte(out, 3);         /* XFL, OS = Unix */
    if (o->extra) { le16(out, o->extra_len); bput(out, o->extra, o->extra_len); }
    if (o->name) { bstr(out, o->name); bbyte(out, 0); }
    if (o->comment) { bstr(out, o->comment); bbyte(out, 0); }
    if (o->hcrc) le16(out, crc32_of(out->p + start, out->n - start) & 0xFFFFu);
    size_t pos = 0;
    do {
        size_t l = n - pos < block ? n - pos : block;
        bbyte(out, pos + l >= n ? 1u : 0u);
        le16(out, (unsigned)l); le16(out, ~(unsigned)l & 0xFFFFu);
        bput(out, data + pos, l);
        pos += l;
    } while (pos < n);
    le32(out, crc32_of(data, n));
    le32(out, (uint32_t)n);
}

typedef struct { char name[32], comment[32]; unsigned extra_len; uint32_t mtime; size_t isize; uint32_t crc; int blocks; } Info;

/* Parse one member at d[*pos]; append the payload to out. Returns NULL or error text. */
static const char *read_member(const unsigned char *d, size_t n, size_t *pos, Buf *out, Info *in) {
    size_t p = *pos;
    memset(in, 0, sizeof *in);
    if (n - p < 18) return "too short";
    if (d[p] != 0x1F || d[p + 1] != 0x8B) return "bad magic";
    if (d[p + 2] != 8) return "unsupported method";
    unsigned flags = d[p + 3];
    if (flags & 0xE0) return "reserved flag bits";
    in->mtime = rd32(d + p + 4);
    size_t q = p + 10;
    if (flags & FEXTRA) {
        if (q + 2 > n) return "truncated extra";
        in->extra_len = rd16(d + q);
        q += 2 + in->extra_len;
        if (q > n) return "truncated extra";
    }
    if (flags & FNAME) {
        size_t k = 0;
        while (q < n && d[q]) { if (k + 1 < sizeof in->name) in->name[k++] = (char)d[q]; q++; }
        if (q >= n) return "unterminated name";
        q++;
    }
    if (flags & FCOMMENT) {
        size_t k = 0;
        while (q < n && d[q]) { if (k + 1 < sizeof in->comment) in->comment[k++] = (char)d[q]; q++; }
        if (q >= n) return "unterminated comment";
        q++;
    }
    if (flags & FHCRC) {
        if (q + 2 > n) return "truncated header crc";
        if ((crc32_of(d + p, q - p) & 0xFFFFu) != rd16(d + q)) return "header crc mismatch";
        q += 2;
    }
    size_t out_start = out->n;
    for (;;) {
        if (q + 5 > n) return "truncated block";
        if (((d[q] >> 1) & 3) != 0) return "compressed block (unsupported)";
        int last = d[q] & 1;
        size_t l = rd16(d + q + 1);
        if ((l ^ 0xFFFFu) != rd16(d + q + 3)) return "stored length check failed";
        if (q + 5 + l > n) return "truncated block data";
        bput(out, d + q + 5, l);
        q += 5 + l;
        in->blocks++;
        if (last) break;
    }
    if (q + 8 > n) return "truncated trailer";
    in->crc = rd32(d + q);
    in->isize = rd32(d + q + 4);
    if (in->isize != ((out->n - out_start) & 0xFFFFFFFFu)) return "isize mismatch";
    if (crc32_of(out->p + out_start, out->n - out_start) != in->crc) return "crc mismatch";
    *pos = q + 8;
    return NULL;
}

int main(void) {
    crc_init();
    static unsigned char blob[3][900];
    static const size_t lens[3] = {0, 300, 900};
    for (int i = 0; i < 3; i++)
        for (size_t k = 0; k < lens[i]; k++) blob[i][k] = (unsigned char)(i == 1 ? (uint32_t)(unsigned char)"hello gzip "[k % 11] : (rnd() >> 8));
    static const unsigned char extra[] = {'A', 'b', 4, 0, 1, 2, 3, 4};
    Opts o[3] = {
        {NULL, NULL, NULL, 0, 0, 0},
        {"greeting.txt", "text member", NULL, 0, 1, 1700000000u},
        {"random.bin", NULL, extra, sizeof extra, 1, 1700000123u}
    };
    static const size_t blocks[3] = {65535, 128, 400};
    Buf gz = {0};
    size_t member_start[3];
    for (int i = 0; i < 3; i++) { member_start[i] = gz.n; member(&gz, blob[i], lens[i], &o[i], blocks[i]); }
    wfile("multi.gz", gz.p, gz.n);
    size_t n;
    unsigned char *d = rfile("multi.gz", &n);
    printf("gzip stream: %zu bytes, first header:", n);
    for (int i = 0; i < 10; i++) printf(" %02X", d[i]);
    putchar('\n');
    size_t pos = 0;
    Buf all = {0};
    int idx = 0;
    while (pos < n) {
        Info in;
        size_t before = all.n;
        const char *e = read_member(d, n, &pos, &all, &in);
        CHECK(e == NULL);
        CHECK(all.n - before == lens[idx] && (lens[idx] == 0 || !memcmp(all.p + before, blob[idx], lens[idx])));
        printf("member %d: at %zu, name=\"%s\" comment=\"%s\" extra=%u mtime=%u blocks=%d isize=%zu crc=%08X\n", idx,
               member_start[idx], in.name, in.comment, in.extra_len, (unsigned)in.mtime, in.blocks, in.isize, (unsigned)in.crc);
        idx++;
    }
    CHECK(idx == 3 && all.n == 1200);
    printf("concatenated payload: %zu bytes\n", all.n);
    /* single-member known bytes: gzip of empty input with no fields */
    Buf e0 = {0};
    Opts plain = {NULL, NULL, NULL, 0, 0, 0};
    member(&e0, (const unsigned char *)"", 0, &plain, 100);
    printf("empty member (%zu bytes):", e0.n);
    for (size_t i = 0; i < e0.n; i++) printf(" %02X", e0.p[i]);
    putchar('\n');
    /* damage */
    struct { size_t off; unsigned char val; } dm[] = {
        {0, 0x1E}, {2, 7}, {3, 0x80}, {member_start[1] + 4, 0xFF}, {member_start[1] + 12, 'X'}, {member_start[2] + 30, 9}, {n - 1, 0x55}, {n - 6, 0x11}
    };
    for (int i = 0; i < 8; i++) {
        unsigned char *c = malloc(n);
        memcpy(c, d, n);
        c[dm[i].off] ^= dm[i].val;
        size_t p = 0;
        Buf tmp = {0};
        const char *err = NULL;
        int cnt = 0;
        while (p < n && !err) {
            Info in;
            err = read_member(c, n, &p, &tmp, &in);
            if (!err) cnt++;
        }
        printf("damage @%3zu: %d good members, then %s\n", dm[i].off, cnt, err ? err : "no error?!");
        CHECK(err);
        free(c); bfree(&tmp);
    }
    free(d); bfree(&gz); bfree(&all); bfree(&e0);
    remove("multi.gz");
    return 0;
}
