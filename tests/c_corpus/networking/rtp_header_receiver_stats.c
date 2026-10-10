/*
 * title: RTP header codec and RFC 3550 receiver statistics
 * topic: networking
 * covers: RTP fixed header CSRC list and extension parsing, padding validation, sequence number cycles and probation, dropout and misorder handling, cumulative loss and loss fraction, interarrival jitter in fixed point, RTCP report block packing
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    unsigned version, padding, ext, cc, marker, pt;
    uint16_t seq;
    uint32_t ts, ssrc, csrc[15];
    unsigned ext_id, ext_words;
    size_t hdr_len, payload_len;
} Rtp;

static void be16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void be32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (24 - 8 * i)); }
static unsigned r16(const unsigned char *p) { return (unsigned)((p[0] << 8) | p[1]); }
static uint32_t r32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static size_t build(unsigned char *o, const Rtp *h, const unsigned char *pay, size_t pn, unsigned pad) {
    size_t n = 12;
    o[0] = (unsigned char)(2 << 6 | (pad ? 0x20 : 0) | (h->ext ? 0x10 : 0) | h->cc);
    o[1] = (unsigned char)((h->marker ? 0x80 : 0) | h->pt);
    be16(o + 2, h->seq); be32(o + 4, h->ts); be32(o + 8, h->ssrc);
    for (unsigned i = 0; i < h->cc; i++) { be32(o + n, h->csrc[i]); n += 4; }
    if (h->ext) { be16(o + n, h->ext_id); be16(o + n + 2, h->ext_words); n += 4; memset(o + n, 0xee, 4 * h->ext_words); n += 4 * h->ext_words; }
    memcpy(o + n, pay, pn); n += pn;
    if (pad) { memset(o + n, 0, pad - 1); n += pad - 1; o[n++] = (unsigned char)pad; }
    return n;
}

static const char *parse(const unsigned char *p, size_t n, Rtp *h) {
    if (n < 12) return "too-short";
    memset(h, 0, sizeof *h);
    h->version = p[0] >> 6;
    if (h->version != 2) return "bad-version";
    h->padding = (p[0] >> 5) & 1; h->ext = (p[0] >> 4) & 1; h->cc = p[0] & 15;
    h->marker = p[1] >> 7; h->pt = p[1] & 127;
    h->seq = (uint16_t)r16(p + 2); h->ts = r32(p + 4); h->ssrc = r32(p + 8);
    size_t o = 12;
    if (n < o + 4 * h->cc) return "csrc-truncated";
    for (unsigned i = 0; i < h->cc; i++) h->csrc[i] = r32(p + o + 4 * i);
    o += 4 * h->cc;
    if (h->ext) {
        if (n < o + 4) return "extension-truncated";
        h->ext_id = r16(p + o); h->ext_words = r16(p + o + 2);
        o += 4;
        if (n < o + 4 * h->ext_words) return "extension-truncated";
        o += 4 * h->ext_words;
    }
    size_t end = n;
    if (h->padding) {
        unsigned pl = p[n - 1];
        if (pl == 0 || o + pl > n) return "bad-padding";
        end = n - pl;
    }
    h->hdr_len = o; h->payload_len = end - o;
    return NULL;
}

/* RFC 3550 appendix A.1 */
#define RTP_SEQ_MOD (1u << 16)
enum { MAX_DROPOUT = 3000, MAX_MISORDER = 100, MIN_SEQUENTIAL = 2 };
typedef struct {
    uint16_t max_seq; uint32_t cycles, base_seq, bad_seq, probation, received, expected_prior, received_prior;
    uint32_t transit, jitter; /* jitter kept scaled by 16 */
    int have_transit;
    unsigned resyncs;
} Src;

static void init_seq(Src *s, uint16_t seq) {
    s->base_seq = seq; s->max_seq = seq; s->bad_seq = RTP_SEQ_MOD + 1; s->cycles = 0; s->received = 0; s->received_prior = 0; s->expected_prior = 0;
}
static int update_seq(Src *s, uint16_t seq) {
    uint16_t udelta = (uint16_t)(seq - s->max_seq);
    if (s->probation) {
        if (seq == (uint16_t)(s->max_seq + 1)) {
            s->probation--; s->max_seq = seq;
            if (s->probation == 0) { init_seq(s, seq); s->received++; return 1; }
        } else { s->probation = MIN_SEQUENTIAL - 1; s->max_seq = seq; }
        return 0;
    } else if (udelta < MAX_DROPOUT) {
        if (seq < s->max_seq) s->cycles += RTP_SEQ_MOD;
        s->max_seq = seq;
    } else if (udelta <= RTP_SEQ_MOD - MAX_MISORDER) {
        if (seq == s->bad_seq) { init_seq(s, seq); s->resyncs++; }
        else { s->bad_seq = (uint32_t)(seq + 1) & (RTP_SEQ_MOD - 1); return 0; }
    }
    s->received++;
    return 1;
}
static void update_jitter(Src *s, uint32_t arrival, uint32_t rtp_ts) {
    uint32_t transit = arrival - rtp_ts;
    if (s->have_transit) {
        int32_t d = (int32_t)(transit - s->transit);
        if (d < 0) d = -d;
        s->jitter += (uint32_t)d - ((s->jitter + 8) >> 4);
    }
    s->transit = transit; s->have_transit = 1;
}

typedef struct { unsigned fraction; int32_t lost; uint32_t ext_max, jitter; } Report;
static Report make_report(Src *s) {
    Report r;
    uint32_t ext_max = s->cycles + s->max_seq;
    uint32_t expected = ext_max - s->base_seq + 1;
    int32_t lost = (int32_t)expected - (int32_t)s->received;
    if (lost > 0x7fffff) lost = 0x7fffff;
    if (lost < -0x800000) lost = -0x800000;
    uint32_t exp_int = expected - s->expected_prior, rcv_int = s->received - s->received_prior;
    s->expected_prior = expected; s->received_prior = s->received;
    int32_t lost_int = (int32_t)exp_int - (int32_t)rcv_int;
    r.fraction = (exp_int == 0 || lost_int <= 0) ? 0 : (unsigned)(((uint32_t)lost_int << 8) / exp_int);
    r.lost = lost; r.ext_max = ext_max; r.jitter = s->jitter >> 4;
    return r;
}

static void pack_block(unsigned char *o, uint32_t ssrc, const Report *r, uint32_t lsr, uint32_t dlsr) {
    be32(o, ssrc);
    o[4] = (unsigned char)r->fraction;
    uint32_t l = (uint32_t)r->lost & 0xffffffu;
    o[5] = (unsigned char)(l >> 16); o[6] = (unsigned char)(l >> 8); o[7] = (unsigned char)l;
    be32(o + 8, r->ext_max); be32(o + 12, r->jitter); be32(o + 16, lsr); be32(o + 20, dlsr);
}

static uint32_t rs = 0x52545033u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

typedef struct { uint16_t seq; uint32_t ts, arrival; } Arr;
static int by_arrival(const void *a, const void *b) {
    const Arr *x = a, *y = b;
    if (x->arrival != y->arrival) return x->arrival < y->arrival ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

int main(void) {
    unsigned char buf[400], pay[160];
    for (int i = 0; i < 160; i++) pay[i] = (unsigned char)(0xd5 ^ i);
    Rtp h;
    memset(&h, 0, sizeof h);
    h.pt = 0; h.seq = 65530; h.ts = 0xfffffe00u; h.ssrc = 0xdeadbeefu; h.marker = 1;
    size_t n = build(buf, &h, pay, 160, 0);
    Rtp g;
    CHECK(parse(buf, n, &g) == NULL);
    printf("basic: %zu bytes, header %zu, payload %zu, pt=%u marker=%u seq=%u ts=0x%08x ssrc=0x%08x\n", n, g.hdr_len, g.payload_len, g.pt, g.marker, g.seq, (unsigned)g.ts, (unsigned)g.ssrc);
    CHECK(g.hdr_len == 12 && g.payload_len == 160 && g.seq == 65530);
    h.cc = 3; h.csrc[0] = 1; h.csrc[1] = 2; h.csrc[2] = 3; h.ext = 1; h.ext_id = 0xbede; h.ext_words = 2; h.marker = 0; h.pt = 96;
    n = build(buf, &h, pay, 100, 8);
    CHECK(parse(buf, n, &g) == NULL);
    printf("csrc+ext+padding: %zu bytes, header %zu, payload %zu, cc=%u ext=0x%04x/%u words, csrc[2]=%u\n", n, g.hdr_len, g.payload_len, g.cc, g.ext_id, g.ext_words, (unsigned)g.csrc[2]);
    CHECK(g.hdr_len == 12 + 12 + 4 + 8 && g.payload_len == 100);
    unsigned char t[400];
    printf("errors:");
    printf(" %s", parse(buf, 8, &g));
    memcpy(t, buf, n); t[0] = 0x40 | (t[0] & 0x3f); printf(" %s", parse(t, n, &g));
    memcpy(t, buf, n); t[n - 1] = 0; printf(" %s", parse(t, n, &g));
    memcpy(t, buf, n); t[n - 1] = 250; printf(" %s", parse(t, n, &g));
    printf(" %s", parse(buf, 20, &g));
    printf(" %s\n", parse(buf, 30, &g));

    /* Simulate 3000 packets, 20 ms apart (160 ticks at 8 kHz), sequence starting near the wrap. */
    enum { NP = 3000 };
    static Arr arr[NP * 2];
    int na = 0, sent_lost = 0;
    uint16_t seq0 = 65400;
    for (int i = 0; i < NP; i++) {
        uint32_t ts = 0xffffff00u + (uint32_t)i * 160;
        if (rnd() % 20 == 0) { sent_lost++; continue; }
        uint32_t jit = rnd() % 60; /* up to 60 ticks (7.5 ms) of network jitter */
        arr[na].seq = (uint16_t)(seq0 + i); arr[na].ts = ts; arr[na].arrival = ts + 400 + jit; na++;
        if (rnd() % 50 == 0) { arr[na] = arr[na - 1]; arr[na].arrival += 30; na++; } /* duplicate */
        if (rnd() % 40 == 0) arr[na - 1].arrival += 200 + rnd() % 200; /* late, causes reorder */
    }
    qsort(arr, (size_t)na, sizeof arr[0], by_arrival);
    Src s;
    memset(&s, 0, sizeof s);
    init_seq(&s, arr[0].seq);
    s.max_seq = (uint16_t)(arr[0].seq - 1);
    s.probation = MIN_SEQUENTIAL;
    int accepted = 0;
    Report last = { 0, 0, 0, 0 };
    printf("interval reports (fraction is lost/256 for the interval):\n");
    for (int i = 0; i < na; i++) {
        if (update_seq(&s, arr[i].seq)) { accepted++; update_jitter(&s, arr[i].arrival, arr[i].ts); }
        if ((i + 1) % 600 == 0) {
            last = make_report(&s);
            printf("  after %4d pkts: ext_max=%6u lost=%4d fraction=%3u jitter=%2u ticks (%.2f ms)\n", i + 1, (unsigned)last.ext_max, (int)last.lost, last.fraction, (unsigned)last.jitter, (double)last.jitter / 8.0);
        }
    }
    Report fin = make_report(&s);
    uint32_t expected = fin.ext_max - s.base_seq + 1;
    printf("final: received=%u expected=%u cumulative lost=%d cycles=%u (ext_max 0x%08x) resyncs=%u\n", (unsigned)s.received, (unsigned)expected, (int)fin.lost, (unsigned)(s.cycles >> 16), (unsigned)fin.ext_max, s.resyncs);
    CHECK(s.cycles == RTP_SEQ_MOD); /* crossed the 16 bit wrap exactly once */
    CHECK((int)fin.lost == (int)expected - (int)s.received);
    CHECK(expected >= (uint32_t)(NP - 60) && s.received <= (uint32_t)na);
    CHECK(fin.jitter < 60);
    CHECK(s.received == (uint32_t)accepted);
    printf("packets sent %d, dropped by network %d, arrivals incl. duplicates %d, jitter estimate %u ticks\n", NP, sent_lost, na, (unsigned)fin.jitter);

    /* RTCP report block packing. */
    unsigned char blk[24];
    Report rep = { 13, -5, 0x0001fffeu, 21 };
    pack_block(blk, 0xdeadbeefu, &rep, 0x12345678u, 0x00010000u);
    printf("report block:");
    for (int i = 0; i < 24; i++) printf("%s%02x", i % 4 == 0 ? " " : "", blk[i]);
    printf("\n");
    int32_t lost_back = (int32_t)(((uint32_t)blk[5] << 16) | ((uint32_t)blk[6] << 8) | blk[7]);
    if (lost_back & 0x800000) lost_back -= 0x1000000;
    CHECK(lost_back == -5 && blk[4] == 13 && r32(blk + 8) == 0x1fffe);
    return 0;
}
