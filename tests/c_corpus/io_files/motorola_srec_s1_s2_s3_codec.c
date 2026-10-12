/*
 * title: Motorola S-record codec with S1/S2/S3 address widths
 * topic: io_files
 * covers: srec, ones-complement checksum, address width selection, S0 header, S5 count, S7/S8/S9 start, image merge and diff
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

enum { MAXA = 1 << 18 };

typedef struct { unsigned addr; unsigned len; unsigned char data[32]; } Chunk;

static void put_srec(FILE *f, char kind, int addr_bytes, unsigned addr, const unsigned char *d, unsigned len) {
    unsigned count = (unsigned)addr_bytes + len + 1;
    unsigned sum = count;
    fprintf(f, "S%c%02X", kind, count);
    for (int i = addr_bytes - 1; i >= 0; i--) {
        unsigned b = (addr >> (8 * i)) & 255u;
        fprintf(f, "%02X", b);
        sum += b;
    }
    for (unsigned i = 0; i < len; i++) { fprintf(f, "%02X", d[i]); sum += d[i]; }
    fprintf(f, "%02X\n", (~sum) & 255u);
}

static int width_for(unsigned max_addr) { return max_addr < 0x10000u ? 2 : max_addr < 0x1000000u ? 3 : 4; }

static int write_srec(const char *name, const Chunk *c, int n, unsigned start, int force, int *width_out) {
    unsigned max_addr = start;
    for (int i = 0; i < n; i++) if (c[i].addr + c[i].len - 1 > max_addr) max_addr = c[i].addr + c[i].len - 1;
    int w = width_for(max_addr);
    if (force > w) w = force;   /* callers may demand wider addresses than needed */
    *width_out = w;
    FILE *f = fopen(name, "w");
    if (!f) fail("open");
    put_srec(f, '0', 2, 0, (const unsigned char *)"demo.s19", 8);
    for (int i = 0; i < n; i++) put_srec(f, (char)('1' + w - 2), w, c[i].addr, c[i].data, c[i].len);
    put_srec(f, '5', 2, (unsigned)n, NULL, 0);
    put_srec(f, (char)('9' - (w - 2)), w, start, NULL, 0);
    fclose(f);
    return w;
}

static int hx(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

typedef struct { int s0, data_recs, s5, has_start; unsigned start; char header[24]; const char *err; int line; unsigned bytes; } Info;

static int read_srec(const char *text, unsigned char *mem, unsigned char *map, Info *in) {
    memset(in, 0, sizeof *in);
    int line = 0;
    while (*text) {
        const char *e = strchr(text, '\n');
        size_t l = e ? (size_t)(e - text) : strlen(text);
        const char *s = text;
        text += l + (e ? 1 : 0);
        line++;
        in->line = line;
        if (!l) continue;
        if (s[0] != 'S' || l < 10 || s[1] < '0' || s[1] > '9' || l % 2) { in->err = "malformed record"; return 0; }
        unsigned char b[80];
        size_t nb = (l - 2) / 2;
        if (nb > sizeof b) { in->err = "record too long"; return 0; }
        unsigned sum = 0;
        for (size_t i = 0; i < nb; i++) {
            int h = hx((unsigned char)s[2 + 2 * i]), lo = hx((unsigned char)s[3 + 2 * i]);
            if (h < 0 || lo < 0) { in->err = "bad hex digit"; return 0; }
            b[i] = (unsigned char)(h * 16 + lo);
            if (i + 1 < nb) sum += b[i];
        }
        if (b[0] != nb - 1) { in->err = "byte count mismatch"; return 0; }
        if (((~sum) & 255u) != b[nb - 1]) { in->err = "checksum mismatch"; return 0; }
        int kind = s[1] - '0';
        int aw = kind == 0 || kind == 1 || kind == 5 || kind == 9 ? 2 : kind == 2 || kind == 8 ? 3 : kind == 3 || kind == 7 ? 4 : 0;
        if (!aw) { in->err = "unsupported record type"; return 0; }
        unsigned addr = 0;
        for (int i = 0; i < aw; i++) addr = (addr << 8) | b[1 + i];
        size_t dl = nb - 2 - (size_t)aw;
        if (kind == 0) { in->s0 = 1; snprintf(in->header, sizeof in->header, "%.*s", (int)dl, (const char *)b + 1 + aw); }
        else if (kind >= 1 && kind <= 3) {
            if (addr + dl > MAXA) { in->err = "address out of range"; return 0; }
            for (size_t i = 0; i < dl; i++) { mem[addr + i] = b[1 + aw + i]; map[addr + i] = 1; }
            in->data_recs++;
            in->bytes += (unsigned)dl;
        } else if (kind == 5) in->s5 = (int)addr;
        else { in->has_start = 1; in->start = addr; }
    }
    in->err = NULL;
    in->line = 0;
    if (!in->has_start) { in->err = "no termination record"; return 0; }
    if (in->s5 != in->data_recs) { in->err = "record count mismatch"; return 0; }
    return 1;
}

int main(void) {
    unsigned char *mem = calloc(MAXA, 1), *map = calloc(MAXA, 1);
    unsigned char *m2 = calloc(MAXA, 1), *map2 = calloc(MAXA, 1);
    static const unsigned bases[3][3] = {{0x100, 0x2000, 0xF000}, {0x10000, 0x20040, 0x8000}, {0x100, 0x2000, 0x3F000}};
    static const char *label[] = {"S1/S9 (16-bit)", "S2/S8 (24-bit)", "S3/S7 (32-bit)"};
    for (int t = 0; t < 3; t++) {
        memset(mem, 0, MAXA); memset(map, 0, MAXA);
        Chunk ch[12];
        int n = 0;
        for (int r = 0; r < 3; r++)
            for (int k = 0; k < 4; k++) {
                Chunk *c = &ch[n++];
                c->addr = bases[t][r] + (unsigned)k * 32u;
                c->len = 32 - (unsigned)(rnd() % 5);
                for (unsigned i = 0; i < c->len; i++) c->data[i] = (unsigned char)(rnd() >> 12);
            }
        int w;
        int width = write_srec("out.s19", ch, n, bases[t][0], t + 2, &w);
        for (int i = 0; i < n; i++) { memcpy(mem + ch[i].addr, ch[i].data, ch[i].len); memset(map + ch[i].addr, 1, ch[i].len); }
        FILE *f = fopen("out.s19", "r");
        CHECK(f);
        static char text[8192];
        size_t tl = fread(text, 1, sizeof text - 1, f);
        text[tl] = 0;
        fclose(f);
        Info in;
        memset(m2, 0, MAXA); memset(map2, 0, MAXA);
        int ok = read_srec(text, m2, map2, &in);
        CHECK(ok);
        CHECK(!memcmp(map, map2, MAXA) && !memcmp(mem, m2, MAXA));
        printf("%s: addr width %d, %zu bytes, header \"%s\", records=%d (S5=%d) bytes=%u start=%X\n",
               label[t], width, tl, in.header, in.data_recs, in.s5, in.bytes, in.start);
        const char *nl = strchr(text, '\n');
        nl = strchr(nl + 1, '\n');
        const char *second = strchr(text, '\n') + 1;
        printf("  %.*s\n", (int)(nl - second), second);
        const char *last = text + tl - 1;
        while (last > text && last[-1] != '\n') last--;
        printf("  %s", last);
    }
    static const char *bad[] = {
        "S00600004844521B\nS9030000FC\n",       /* header only, zero data records: valid */
        "S1130000285F245F2212226A000424290008237C2A\nS9030000FB\n",
        "S1130000285F245F2212226A000424290008237C2B\nS9030000FC\n",
        "S1090000ABCD\nS9030000FC\n",
        "S5030000FC\n",
        "S4030000FB\n",   /* reserved type with a wrong checksum */
        "S1 30000\n"
    };
    for (int i = 0; i < 7; i++) {
        Info in;
        int ok = read_srec(bad[i], m2, map2, &in);
        if (ok) printf("bad %d: accepted\n", i);
        else printf("bad %d: line %d %s\n", i, in.line, in.err);
    }
    free(mem); free(map); free(m2); free(map2);
    remove("out.s19");
    return 0;
}
