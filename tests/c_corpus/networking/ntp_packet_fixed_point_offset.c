/*
 * title: NTP packet codec with 32.32 fixed-point timestamps and offset/delay filter
 * topic: networking
 * covers: NTPv4 header layout, 32.32 and 16.16 fixed-point conversion, era rollover pivoting, four-timestamp offset and delay math, minimum-delay sample selection, response sanity checks, kiss-o-death codes
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define NTP_UNIX_DELTA 2208988800ull

typedef uint64_t Ts; /* seconds in the high 32 bits, fraction in the low 32 */

typedef struct {
    unsigned li, vn, mode, stratum;
    int poll, precision;
    uint32_t root_delay, root_disp; /* 16.16 */
    unsigned char refid[4];
    Ts ref, orig, rx, tx;
} Ntp;

static Ts from_unix(uint64_t sec, uint32_t usec) {
    uint64_t ntp_sec = (sec + NTP_UNIX_DELTA) & 0xffffffffull; /* era is implicit */
    uint64_t frac = ((uint64_t)usec << 32) / 1000000ull;
    return (ntp_sec << 32) | frac;
}
static uint32_t ts_usec(Ts t) { return (uint32_t)((((t & 0xffffffffull) * 1000000ull) + (1ull << 31)) >> 32); }

/* Unwrap to a unix time nearest the pivot. */
static int64_t to_unix(Ts t, int64_t pivot) {
    int64_t sec = (int64_t)(t >> 32);
    int64_t base = sec - (int64_t)NTP_UNIX_DELTA; /* era 0 interpretation */
    int64_t best = base;
    for (int era = -1; era <= 3; era++) {
        int64_t cand = base + (int64_t)era * 4294967296ll;
        int64_t d1 = cand > pivot ? cand - pivot : pivot - cand;
        int64_t d0 = best > pivot ? best - pivot : pivot - best;
        if (d1 < d0) best = cand;
    }
    return best;
}

static void put32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (24 - 8 * i)); }
static uint32_t get32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void put_ts(unsigned char *p, Ts t) { put32(p, (uint32_t)(t >> 32)); put32(p + 4, (uint32_t)t); }
static Ts get_ts(const unsigned char *p) { return ((Ts)get32(p) << 32) | get32(p + 4); }

static void encode(const Ntp *n, unsigned char *o) {
    memset(o, 0, 48);
    o[0] = (unsigned char)((n->li << 6) | (n->vn << 3) | n->mode);
    o[1] = (unsigned char)n->stratum; o[2] = (unsigned char)n->poll; o[3] = (unsigned char)n->precision;
    put32(o + 4, n->root_delay); put32(o + 8, n->root_disp); memcpy(o + 12, n->refid, 4);
    put_ts(o + 16, n->ref); put_ts(o + 24, n->orig); put_ts(o + 32, n->rx); put_ts(o + 40, n->tx);
}
static void decode(const unsigned char *p, Ntp *n) {
    n->li = p[0] >> 6; n->vn = (p[0] >> 3) & 7; n->mode = p[0] & 7; n->stratum = p[1];
    n->poll = (int8_t)p[2]; n->precision = (int8_t)p[3];
    n->root_delay = get32(p + 4); n->root_disp = get32(p + 8); memcpy(n->refid, p + 12, 4);
    n->ref = get_ts(p + 16); n->orig = get_ts(p + 24); n->rx = get_ts(p + 32); n->tx = get_ts(p + 40);
}

/* signed difference in microseconds, valid within about +-68 years */
static int64_t diff_us(Ts a, Ts b) {
    int64_t d = (int64_t)(a - b);
    int64_t neg = d < 0;
    uint64_t m = neg ? (uint64_t)0 - (uint64_t)d : (uint64_t)d;
    uint64_t secs = m >> 32, frac = m & 0xffffffffull;
    int64_t us = (int64_t)(secs * 1000000ull + ((frac * 1000000ull + (1ull << 31)) >> 32));
    return neg ? -us : us;
}

static const char *sanity(const Ntp *r, Ts client_tx) {
    if (r->mode != 4) return "not-a-server-reply";
    if (r->vn < 3 || r->vn > 4) return "bad-version";
    if (r->stratum == 0) {
        if (!memcmp(r->refid, "RATE", 4)) return "kiss-of-death RATE";
        if (!memcmp(r->refid, "DENY", 4)) return "kiss-of-death DENY";
        return "kiss-of-death other";
    }
    if (r->stratum > 15) return "stratum-unsynchronized";
    if (r->li == 3) return "alarm-condition";
    if (r->orig != client_tx) return "origin-timestamp-mismatch";
    if (r->tx == 0) return "zero-transmit-timestamp";
    if (r->tx < r->rx) return "server-time-went-backwards";
    return "ok";
}

static uint32_t rs = 0x6e747034u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    /* Conversions. */
    CHECK(from_unix(0, 0) >> 32 == NTP_UNIX_DELTA);
    printf("unix 0 -> ntp seconds %llu; 0.5 s -> fraction 0x%08x\n", (unsigned long long)(from_unix(0, 0) >> 32), (unsigned)(from_unix(0, 500000) & 0xffffffffu));
    CHECK((from_unix(0, 500000) & 0xffffffffu) == 0x80000000u);
    for (uint32_t us = 0; us < 1000000; us++) CHECK(ts_usec(from_unix(1, us)) == us);
    printf("all 1000000 microsecond values survive the 32.32 round trip\n");
    /* Era handling: 2038-01-19 03:14:07 and 2036 rollover. */
    static const int64_t unix_times[] = { 0, 1700000000, 2085978495, 2147483647, 4102444800LL, 4294967295LL + 1000 };
    for (size_t i = 0; i < sizeof unix_times / sizeof unix_times[0]; i++) {
        Ts t = from_unix((uint64_t)unix_times[i], 0);
        int64_t back = to_unix(t, unix_times[i] + 1000);
        printf("unix %-11lld -> ntp seconds %-10llu (era %s) -> unix %lld\n", (long long)unix_times[i], (unsigned long long)(t >> 32),
               (unix_times[i] + (int64_t)NTP_UNIX_DELTA) >= 4294967296ll ? "1" : "0", (long long)back);
        CHECK(back == unix_times[i]);
    }
    /* Packet round trip. */
    Ntp a;
    memset(&a, 0, sizeof a);
    a.li = 0; a.vn = 4; a.mode = 4; a.stratum = 2; a.poll = 6; a.precision = -20; a.root_delay = 0x00008000u; a.root_disp = 0x00010000u;
    a.refid[0] = 192; a.refid[1] = 0; a.refid[2] = 2; a.refid[3] = 1;
    a.ref = from_unix(1700000000, 123456); a.orig = from_unix(1700000010, 500000); a.rx = from_unix(1700000010, 520000); a.tx = from_unix(1700000010, 520100);
    unsigned char wire[48];
    encode(&a, wire);
    Ntp b;
    decode(wire, &b);
    CHECK(a.li == b.li && a.vn == b.vn && a.mode == b.mode && a.stratum == b.stratum && a.poll == b.poll && a.precision == b.precision);
    CHECK(a.root_delay == b.root_delay && a.root_disp == b.root_disp && memcmp(a.refid, b.refid, 4) == 0);
    CHECK(a.ref == b.ref && a.orig == b.orig && a.rx == b.rx && a.tx == b.tx);
    printf("header byte 0x%02x (li=%u vn=%u mode=%u), stratum %u, poll %d (%d s), precision %d (%.2f us), root delay %.4f s, root dispersion %.4f s\n", wire[0], b.li, b.vn, b.mode, b.stratum, b.poll, 1 << b.poll, b.precision,
           1e6 / (double)(1 << 20), (double)b.root_delay / 65536.0, (double)b.root_disp / 65536.0);
    printf("refid %u.%u.%u.%u, wire bytes 16..23: ", b.refid[0], b.refid[1], b.refid[2], b.refid[3]);
    for (int i = 16; i < 24; i++) printf("%02x", wire[i]);
    printf("\n");

    /* Simulated exchanges: client clock behind the server by 1.234567 s, asymmetric delays. */
    const int64_t true_offset_us = 1234567;
    int64_t best_delay = 1 << 30, best_offset = 0;
    printf("sample  offset_us   delay_us\n");
    for (int i = 0; i < 8; i++) {
        uint32_t out_us = 15000 + rnd() % 30000;
        uint32_t back_us = 15000 + rnd() % 30000;
        uint32_t proc_us = 50 + rnd() % 200;
        int64_t client_t1_us = 5000000 + i * 1000000 + (int64_t)(rnd() % 1000);
        int64_t server_t2_us = client_t1_us + true_offset_us + out_us;
        int64_t server_t3_us = server_t2_us + proc_us;
        int64_t client_t4_us = server_t3_us - true_offset_us + back_us;
        Ts t1 = from_unix(1700000000 + (uint64_t)(client_t1_us / 1000000), (uint32_t)(client_t1_us % 1000000));
        Ts t2 = from_unix(1700000000 + (uint64_t)(server_t2_us / 1000000), (uint32_t)(server_t2_us % 1000000));
        Ts t3 = from_unix(1700000000 + (uint64_t)(server_t3_us / 1000000), (uint32_t)(server_t3_us % 1000000));
        Ts t4 = from_unix(1700000000 + (uint64_t)(client_t4_us / 1000000), (uint32_t)(client_t4_us % 1000000));
        int64_t offset = (diff_us(t2, t1) + diff_us(t3, t4)) / 2;
        int64_t delay = diff_us(t4, t1) - diff_us(t3, t2);
        /* exact expectation: offset error is half the delay asymmetry */
        int64_t err = ((int64_t)out_us - (int64_t)back_us) / 2;
        CHECK(delay == (int64_t)out_us + (int64_t)back_us);
        CHECK(offset - true_offset_us - err <= 2 && offset - true_offset_us - err >= -2);
        printf("%4d   %9lld  %9lld\n", i, (long long)offset, (long long)delay);
        if (delay < best_delay) { best_delay = delay; best_offset = offset; }
    }
    printf("clock filter picks delay %lld us, offset %lld us (true offset %lld, error %lld us)\n", (long long)best_delay, (long long)best_offset, (long long)true_offset_us, (long long)(best_offset - true_offset_us));

    /* Sanity checks on replies. */
    Ntp good = a;
    Ts ctx = from_unix(1700000010, 500000);
    printf("checks:");
    printf(" %s", sanity(&good, ctx));
    Ntp x = good; x.mode = 3; printf(", %s", sanity(&x, ctx));
    x = good; x.orig ^= 1; printf(", %s", sanity(&x, ctx));
    x = good; x.stratum = 16; printf(", %s", sanity(&x, ctx));
    x = good; x.li = 3; printf(", %s", sanity(&x, ctx));
    x = good; x.stratum = 0; memcpy(x.refid, "RATE", 4); printf(", %s", sanity(&x, ctx));
    x = good; x.stratum = 0; memcpy(x.refid, "DENY", 4); printf(", %s", sanity(&x, ctx));
    x = good; x.tx = 0; printf(", %s", sanity(&x, ctx));
    x = good; x.tx = x.rx - 1; printf(", %s", sanity(&x, ctx));
    x = good; x.vn = 2; printf(", %s\n", sanity(&x, ctx));
    return 0;
}
