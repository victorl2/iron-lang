/*
 * title: TCP option encoding and parsing with SACK and PAWS
 * topic: networking
 * covers: option kinds MSS window scale SACK-permitted SACK blocks timestamps NOP and EOL, TLV walking with unknown kinds, 40 byte limit, data offset padding, sequence number wraparound comparisons, PAWS timestamp check
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    int has_mss, has_ws, sack_perm, has_ts;
    unsigned mss, ws;
    uint32_t tsval, tsecr;
    int nsack;
    uint32_t sack[4][2];
    int unknown, nops;
    unsigned unknown_kinds[8];
} Opts;

static void be32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (24 - 8 * i)); }
static uint32_t r32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* Encodes options as a Linux-style SYN or established-segment block. Returns padded length, or 0 if it does not fit in 40 bytes. */
static size_t encode(unsigned char *o, const Opts *x) {
    size_t n = 0;
    if (x->has_mss) { o[n++] = 2; o[n++] = 4; o[n++] = (unsigned char)(x->mss >> 8); o[n++] = (unsigned char)x->mss; }
    if (x->sack_perm) { o[n++] = 4; o[n++] = 2; }
    if (x->has_ts) { o[n++] = 8; o[n++] = 10; be32(o + n, x->tsval); n += 4; be32(o + n, x->tsecr); n += 4; }
    if (x->has_ws) { o[n++] = 1; o[n++] = 3; o[n++] = 3; o[n++] = (unsigned char)x->ws; }
    if (x->nsack) {
        while (n % 4 != 2) o[n++] = 1; /* NOPs so the block data lands on a 4 byte boundary */
        o[n++] = 5; o[n++] = (unsigned char)(2 + 8 * x->nsack);
        for (int i = 0; i < x->nsack; i++) { be32(o + n, x->sack[i][0]); n += 4; be32(o + n, x->sack[i][1]); n += 4; }
    }
    while (n % 4) o[n++] = 0; /* EOL padding */
    return n > 40 ? 0 : n;
}

typedef enum { O_OK, O_TRUNCATED, O_BAD_LEN, O_BAD_MSS_LEN, O_DUP } OErr;
static const char *oname[] = { "ok", "option-runs-past-end", "length-below-2", "bad-length-for-kind", "duplicate-option" };

static OErr parse(const unsigned char *p, size_t n, Opts *x) {
    memset(x, 0, sizeof *x);
    size_t i = 0;
    while (i < n) {
        unsigned k = p[i];
        if (k == 0) break; /* EOL: rest is padding */
        if (k == 1) { x->nops++; i++; continue; }
        if (i + 1 >= n) return O_TRUNCATED;
        unsigned l = p[i + 1];
        if (l < 2) return O_BAD_LEN;
        if (i + l > n) return O_TRUNCATED;
        const unsigned char *d = p + i + 2;
        switch (k) {
        case 2: if (l != 4) return O_BAD_MSS_LEN; if (x->has_mss) return O_DUP; x->has_mss = 1; x->mss = (unsigned)((d[0] << 8) | d[1]); break;
        case 3: if (l != 3) return O_BAD_MSS_LEN; if (x->has_ws) return O_DUP; x->has_ws = 1; x->ws = d[0] > 14 ? 14 : d[0]; break;
        case 4: if (l != 2) return O_BAD_MSS_LEN; x->sack_perm = 1; break;
        case 5:
            if (l < 10 || (l - 2) % 8 != 0 || (l - 2) / 8 > 4) return O_BAD_MSS_LEN;
            if (x->nsack) return O_DUP;
            x->nsack = (int)((l - 2) / 8);
            for (int b = 0; b < x->nsack; b++) { x->sack[b][0] = r32(d + 8 * b); x->sack[b][1] = r32(d + 8 * b + 4); }
            break;
        case 8: if (l != 10) return O_BAD_MSS_LEN; if (x->has_ts) return O_DUP; x->has_ts = 1; x->tsval = r32(d); x->tsecr = r32(d + 4); break;
        default: if (x->unknown < 8) x->unknown_kinds[x->unknown] = k; x->unknown++; break;
        }
        i += l;
    }
    return O_OK;
}

static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

/* PAWS: reject a segment whose timestamp is older than the last accepted one (modular compare). */
static int paws_ok(uint32_t ts_recent, uint32_t tsval) { return !seq_lt(tsval, ts_recent); }

/* Bytes covered by SACK blocks above a cumulative ACK, merging overlaps modulo 2^32. */
static uint64_t sacked_bytes(uint32_t ack, uint32_t (*b)[2], int n) {
    uint32_t lo[4], hi[4];
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!seq_lt(ack, b[i][0]) || !seq_lt(b[i][0], b[i][1])) continue; /* below ack or empty */
        lo[m] = b[i][0]; hi[m] = b[i][1]; m++;
    }
    /* sort by distance from ack */
    for (int i = 1; i < m; i++) for (int j = i; j > 0 && (lo[j] - ack) < (lo[j - 1] - ack); j--) { uint32_t t = lo[j]; lo[j] = lo[j - 1]; lo[j - 1] = t; t = hi[j]; hi[j] = hi[j - 1]; hi[j - 1] = t; }
    uint64_t total = 0;
    int i = 0;
    while (i < m) {
        uint32_t s = lo[i], e = hi[i];
        i++;
        while (i < m && seq_le(lo[i], e)) { if (seq_lt(e, hi[i])) e = hi[i]; i++; }
        total += (uint32_t)(e - s);
    }
    return total;
}

static void hex(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x%s", p[i], (i % 4 == 3 && i + 1 < n) ? " " : "");
    printf("\n");
}

int main(void) {
    unsigned char b[64];
    /* Typical SYN. */
    Opts syn = { 1, 1, 1, 1, 1460, 7, 0x00c0ffeeu, 0, 0, { { 0 } }, 0, 0, { 0 } };
    size_t n = encode(b, &syn);
    printf("SYN options (%zu bytes): ", n);
    hex(b, n);
    static const unsigned char want[] = { 0x02, 0x04, 0x05, 0xb4, 0x04, 0x02, 0x08, 0x0a, 0x00, 0xc0, 0xff, 0xee, 0, 0, 0, 0, 0x01, 0x03, 0x03, 0x07 };
    CHECK(n == sizeof want && memcmp(b, want, n) == 0);
    Opts got;
    CHECK(parse(b, n, &got) == O_OK);
    printf("  mss=%u ws=%u sack_perm=%d tsval=0x%x nops=%d data offset would be %zu words\n", got.mss, got.ws, got.sack_perm, (unsigned)got.tsval, got.nops, (20 + n) / 4);
    CHECK(got.mss == 1460 && got.ws == 7 && got.has_ts && got.tsval == 0xc0ffeeu);

    /* Data segment with timestamps and 0..4 SACK blocks. */
    for (int nb = 0; nb <= 4; nb++) {
        Opts x;
        memset(&x, 0, sizeof x);
        x.has_ts = 1; x.tsval = 1000 + (uint32_t)nb; x.tsecr = 2000;
        x.nsack = nb;
        for (int i = 0; i < nb; i++) { x.sack[i][0] = 5000 + (uint32_t)i * 3000; x.sack[i][1] = x.sack[i][0] + 1460; }
        n = encode(b, &x);
        if (n == 0) { printf("%d SACK blocks with timestamps: does not fit in 40 bytes\n", nb); continue; }
        Opts y;
        CHECK(parse(b, n, &y) == O_OK && y.nsack == nb && y.has_ts);
        for (int i = 0; i < nb; i++) CHECK(y.sack[i][0] == x.sack[i][0] && y.sack[i][1] == x.sack[i][1]);
        printf("%d SACK block(s) + timestamps: %zu bytes\n", nb, n);
    }
    /* Without timestamps 4 blocks fit (2 NOP + 2 + 32 = 36). */
    { Opts x; memset(&x, 0, sizeof x); x.nsack = 4; for (int i = 0; i < 4; i++) { x.sack[i][0] = (uint32_t)i * 100; x.sack[i][1] = (uint32_t)i * 100 + 50; } n = encode(b, &x); printf("4 SACK blocks alone: %zu bytes\n", n); CHECK(n == 36); }

    /* Unknown options are skipped by length; EOL stops parsing. */
    static const unsigned char unk[] = { 0x01, 0x01, 0x1e, 0x06, 0xaa, 0xbb, 0xcc, 0xdd, 0x02, 0x04, 0x02, 0x18, 0x00, 0x02, 0x04, 0xff };
    parse(unk, sizeof unk, &got);
    printf("skip unknown: unknown=%d kind=%u, mss=%u (option after EOL ignored), nops=%d\n", got.unknown, got.unknown_kinds[0], got.mss, got.nops);
    CHECK(got.unknown == 1 && got.mss == 536);

    /* Malformed blocks. */
    struct { const char *label; size_t n; unsigned char b[12]; } bad[] = {
        { "length 1", 4, { 0x1e, 0x01, 0x00, 0x00 } }, { "length 0", 4, { 0x1e, 0x00, 0x00, 0x00 } }, { "runs past end", 3, { 0x02, 0x04, 0x05 } },
        { "lone kind at end", 3, { 0x01, 0x01, 0x02 } }, { "MSS with length 3", 4, { 0x02, 0x03, 0x05, 0x00 } }, { "duplicate MSS", 8, { 0x02, 0x04, 0x05, 0xb4, 0x02, 0x04, 0x02, 0x18 } },
        { "SACK with 9 bytes", 11, { 0x05, 0x0b, 0, 0, 0, 1, 0, 0, 0, 2, 0 } }, { "timestamp length 8", 8, { 0x08, 0x08, 0, 0, 0, 1, 0, 0 } },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        OErr e = parse(bad[i].b, bad[i].n, &got);
        printf("%-18s -> %s\n", bad[i].label, oname[e]);
        CHECK(e != O_OK);
    }
    /* Wraparound behaviour. */
    printf("seq compare: lt(0xfffffff0, 5)=%d lt(5, 0xfffffff0)=%d\n", seq_lt(0xfffffff0u, 5), seq_lt(5, 0xfffffff0u));
    CHECK(seq_lt(0xfffffff0u, 5) && !seq_lt(5, 0xfffffff0u));
    printf("PAWS: ts_recent=100: tsval 100 ok=%d, 99 ok=%d, wrapped recent=0xfffffffe: tsval 3 ok=%d, tsval 0xfffffff0 ok=%d\n", paws_ok(100, 100), paws_ok(100, 99), paws_ok(0xfffffffeu, 3), paws_ok(0xfffffffeu, 0xfffffff0u));
    CHECK(paws_ok(100, 100) && !paws_ok(100, 99) && paws_ok(0xfffffffeu, 3) && !paws_ok(0xfffffffeu, 0xfffffff0u));
    /* SACK accounting across the wrap point. */
    uint32_t blocks[4][2] = { { 0xfffffff0u, 0x00000010u }, { 0x00000008u, 0x00000020u }, { 0x00000100u, 0x00000180u }, { 0xffffff00u, 0xffffff40u } };
    uint64_t sacked = sacked_bytes(0xffffffe0u, blocks, 4);
    printf("bytes sacked above ack 0xffffffe0: %llu\n", (unsigned long long)sacked);
    CHECK(sacked == 0x30 + 0x80); /* merged [fffffff0,0x20) plus [0x100,0x180); the block below the ack is ignored */
    return 0;
}
