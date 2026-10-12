/*
 * title: Intel HEX encoder and decoder with extended addressing
 * topic: io_files
 * covers: intel hex, record checksum, extended linear address, 64K wrap, gaps in image, start address, error reporting
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

enum { MEMSZ = 0x30000 };

static unsigned char *mem;      /* image bytes */
static unsigned char *mapped;   /* 1 where the image defines a byte */

static void emit_rec(FILE *f, unsigned len, unsigned addr, unsigned type, const unsigned char *data) {
    unsigned sum = len + (addr >> 8) + (addr & 255u) + type;
    fprintf(f, ":%02X%04X%02X", len, addr & 0xFFFFu, type);
    for (unsigned i = 0; i < len; i++) { fprintf(f, "%02X", data[i]); sum += data[i]; }
    fprintf(f, "%02X\r\n", (0x100u - (sum & 255u)) & 255u);
}

static int write_hex(const char *name, unsigned rec_len, unsigned start_addr) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open");
    unsigned cur_upper = 0xFFFFFFFFu;
    int nrec = 0;
    unsigned a = 0;
    while (a < MEMSZ) {
        if (!mapped[a]) { a++; continue; }
        /* gather a run bounded by rec_len, by unmapped bytes, and by the 64K boundary */
        unsigned len = 0;
        while (len < rec_len && a + len < MEMSZ && mapped[a + len] && ((a + len) >> 16) == (a >> 16)) len++;
        if ((a >> 16) != cur_upper) {
            unsigned char ub[2] = {(unsigned char)(a >> 24), (unsigned char)((a >> 16) & 255u)};
            emit_rec(f, 2, 0, 4, ub);
            cur_upper = a >> 16;
            nrec++;
        }
        emit_rec(f, len, a & 0xFFFFu, 0, mem + a);
        nrec++;
        a += len;
    }
    unsigned char sa[4] = {(unsigned char)(start_addr >> 24), (unsigned char)(start_addr >> 16),
                           (unsigned char)(start_addr >> 8), (unsigned char)start_addr};
    emit_rec(f, 4, 0, 5, sa);
    emit_rec(f, 0, 0, 1, NULL);
    nrec += 2;
    fclose(f);
    return nrec;
}

static int hexn(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

typedef struct { int records, data_bytes, eof; unsigned start; unsigned max_addr; int errline; const char *err; } Stats;

static int decode_hex(const char *text, unsigned char *out_mem, unsigned char *out_map, Stats *st) {
    unsigned upper = 0;
    int line = 0;
    memset(st, 0, sizeof *st);
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        line++;
        if (l && p[l - 1] == '\r') l--;
        const char *s = p;
        p += (e ? (size_t)(e - p) + 1 : strlen(p));
        if (l == 0) continue;
        st->errline = line;
        if (st->eof) { st->err = "record after EOF"; return 0; }
        if (s[0] != ':' || l < 11 || (l - 1) % 2) { st->err = "malformed record"; return 0; }
        unsigned char b[300];
        size_t nb = (l - 1) / 2;
        unsigned sum = 0;
        for (size_t i = 0; i < nb; i++) {
            int hi = hexn((unsigned char)s[1 + 2 * i]), lo = hexn((unsigned char)s[2 + 2 * i]);
            if (hi < 0 || lo < 0) { st->err = "bad hex digit"; return 0; }
            b[i] = (unsigned char)(hi * 16 + lo);
            sum += b[i];
        }
        if (sum & 255u) { st->err = "checksum mismatch"; return 0; }
        unsigned len = b[0], addr = (unsigned)(b[1] << 8) | b[2], type = b[3];
        if (nb != len + 5u) { st->err = "length mismatch"; return 0; }
        st->records++;
        switch (type) {
        case 0:
            for (unsigned i = 0; i < len; i++) {
                unsigned a = (upper << 16) + ((addr + i) & 0xFFFFu);
                if (a >= MEMSZ) { st->err = "address out of range"; return 0; }
                out_mem[a] = b[4 + i]; out_map[a] = 1;
                if (a > st->max_addr) st->max_addr = a;
            }
            st->data_bytes += (int)len;
            break;
        case 1: st->eof = 1; break;
        case 4:
            if (len != 2) { st->err = "bad extended address record"; return 0; }
            upper = (unsigned)(b[4] << 8) | b[5];
            break;
        case 5:
            if (len != 4) { st->err = "bad start record"; return 0; }
            st->start = ((unsigned)b[4] << 24) | ((unsigned)b[5] << 16) | ((unsigned)b[6] << 8) | b[7];
            break;
        default: st->err = "unsupported record type"; return 0;
        }
    }
    st->errline = 0;
    if (!st->eof) { st->err = "missing EOF record"; return 0; }
    return 1;
}

int main(void) {
    mem = calloc(MEMSZ, 1);
    mapped = calloc(MEMSZ, 1);
    unsigned char *m2 = calloc(MEMSZ, 1), *map2 = calloc(MEMSZ, 1);
    /* regions: a boot block, a block straddling the 64K line, and a sparse tail */
    static const struct { unsigned base, len; } reg[] = {{0x0000, 45}, {0xFFF0, 40}, {0x1FF00, 300}, {0x20100, 7}, {0x2FFFE, 2}};
    long total = 0;
    for (int r = 0; r < 5; r++)
        for (unsigned i = 0; i < reg[r].len; i++) {
            mem[reg[r].base + i] = (unsigned char)(rnd() >> 8);
            mapped[reg[r].base + i] = 1;
            total++;
        }
    static const unsigned rec_lens[] = {16, 32, 5};
    for (int t = 0; t < 3; t++) {
        int nrec = write_hex("img.hex", rec_lens[t], 0x0001A000u);
        size_t fs;
        FILE *f = fopen("img.hex", "rb");
        CHECK(f);
        char *text = calloc(1, 1 << 16);
        fs = fread(text, 1, (1 << 16) - 1, f);
        fclose(f);
        memset(m2, 0, MEMSZ); memset(map2, 0, MEMSZ);
        Stats st;
        CHECK(decode_hex(text, m2, map2, &st));
        CHECK(memcmp(map2, mapped, MEMSZ) == 0 && memcmp(m2, mem, MEMSZ) == 0);
        CHECK(st.data_bytes == total && st.start == 0x1A000u && st.eof);
        printf("rec_len=%2u: %3d records, %zu bytes, data=%d, max addr=%05X, start=%08X\n",
               rec_lens[t], nrec, fs, st.data_bytes, st.max_addr, st.start);
        if (t == 0) {
            const char *p = text;
            for (int i = 0; i < 4; i++) {
                const char *e = strchr(p, '\r');
                printf("  %.*s\n", (int)(e - p), p);
                p = e + 2;
            }
        }
        if (t == 1) {
            /* corrupt single characters and see what the decoder says */
            static const size_t pos[] = {5, 20, 0, 3};
            static const char val[] = {'F', '0', ';', 'Z'};
            for (int i = 0; i < 4; i++) {
                char save = text[pos[i]];
                text[pos[i]] = save == val[i] ? '1' : val[i];
                Stats s2;
                int ok = decode_hex(text, m2, map2, &s2);
                CHECK(!ok);
                printf("  corrupt @%zu -> line %d: %s\n", pos[i], s2.errline, s2.err);
                text[pos[i]] = save;
            }
        }
        free(text);
    }
    static const char *bad[] = {
        ":00000001FF\n", ":00000001FE\n", ":0400000500000000F7\n:00000001FF\n:00000001FF\n",
        ":10000000000102030405060708090A0B0C0D0E0F78\n:00000001FF\n", ":020000040003F7\n:0100000000FF\n:00000001FF\n"
    };
    for (int i = 0; i < 5; i++) {
        Stats s;
        int ok = decode_hex(bad[i], m2, map2, &s);
        printf("case %d: %s%s\n", i, ok ? "ok" : s.err, ok ? "" : "");
    }
    free(mem); free(mapped); free(m2); free(map2);
    remove("img.hex");
    return 0;
}
