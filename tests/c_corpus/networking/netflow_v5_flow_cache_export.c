/*
 * title: NetFlow v5 flow cache, record packing and collector decode
 * topic: networking
 * covers: five-tuple flow aggregation, idle and active timeouts, TCP FIN and RST expiry, fixed 24 byte header and 48 byte record layouts, big-endian packing, flow sequence continuity, collector-side totals and top talkers
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { uint32_t ts, src, dst; uint16_t sport, dport; uint8_t proto, flags; uint16_t len; uint32_t seq; } Pkt;
typedef struct { uint32_t src, dst; uint16_t sport, dport; uint8_t proto, flags; uint32_t pkts, octets, first, last; int live; } Flow;

#define IDLE_MS 15000u
#define ACTIVE_MS 30000u

static uint32_t rs = 0x6e657466u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static void be16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void be32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (24 - 8 * i)); }
static unsigned r16(const unsigned char *p) { return (unsigned)((p[0] << 8) | p[1]); }
static uint32_t r32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

typedef struct { unsigned char b[24 + 30 * 48]; int n; } Export;
typedef struct { Export ex[64]; int nex; Export cur; uint32_t seq; uint32_t total_flows; } Exporter;

static void flush_export(Exporter *e, uint32_t uptime) {
    if (e->cur.n == 0) return;
    unsigned char *h = e->cur.b;
    be16(h, 5); be16(h + 2, (unsigned)e->cur.n); be32(h + 4, uptime); be32(h + 8, 1700000000u + uptime / 1000); be32(h + 12, (uptime % 1000) * 1000000u);
    be32(h + 16, e->seq); h[20] = 0; h[21] = 1; be16(h + 22, 0);
    e->seq += (uint32_t)e->cur.n;
    CHECK(e->nex < 64);
    e->ex[e->nex++] = e->cur;
    e->cur.n = 0;
}

static void emit(Exporter *e, const Flow *f, uint32_t now) {
    unsigned char *r = e->cur.b + 24 + 48 * e->cur.n;
    memset(r, 0, 48);
    be32(r, f->src); be32(r + 4, f->dst); be32(r + 8, 0x0a000001u);
    be16(r + 12, 1); be16(r + 14, 2);
    be32(r + 16, f->pkts); be32(r + 20, f->octets); be32(r + 24, f->first); be32(r + 28, f->last);
    be16(r + 32, f->sport); be16(r + 34, f->dport);
    r[37] = f->flags; r[38] = f->proto; r[39] = 0;
    be16(r + 40, 64512); be16(r + 42, 64513); r[44] = 24; r[45] = 16;
    e->cur.n++;
    e->total_flows++;
    if (e->cur.n == 30) flush_export(e, now);
}

static int cmp_pkt(const void *a, const void *b) {
    const Pkt *x = a, *y = b;
    if (x->ts != y->ts) return x->ts < y->ts ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

#define MAXF 256
typedef struct { Flow f[MAXF]; } Cache;

static Flow *find(Cache *c, const Pkt *p) {
    for (int i = 0; i < MAXF; i++)
        if (c->f[i].live && c->f[i].src == p->src && c->f[i].dst == p->dst && c->f[i].sport == p->sport && c->f[i].dport == p->dport && c->f[i].proto == p->proto) return &c->f[i];
    return NULL;
}

static void sweep(Cache *c, Exporter *e, uint32_t now, int all) {
    for (int i = 0; i < MAXF; i++) {
        Flow *f = &c->f[i];
        if (!f->live) continue;
        if (all || now - f->last >= IDLE_MS || now - f->first >= ACTIVE_MS) { emit(e, f, now); f->live = 0; }
    }
}

typedef struct { uint32_t ip; uint64_t bytes, pkts; uint32_t flows; } Talker;
static int cmp_talker(const void *a, const void *b) {
    const Talker *x = a, *y = b;
    if (x->bytes != y->bytes) return x->bytes > y->bytes ? -1 : 1;
    return x->ip < y->ip ? -1 : x->ip > y->ip;
}

static void ipstr(uint32_t a, char *s) { snprintf(s, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255)); }

int main(void) {
    static Pkt pk[6000];
    int np = 0;
    uint32_t seq = 0;
    uint64_t in_pkts = 0, in_bytes = 0;
    /* Synthesize conversations: each is a client to server flow plus a reverse flow. */
    for (int c = 0; c < 60; c++) {
        uint32_t r1 = rnd(), r2 = rnd(), r3 = rnd(), r4 = rnd();
        uint32_t client = 0xc0a80100u + 1 + r1 % 12;
        uint32_t server = 0x0a140000u + 1 + r2 % 5;
        unsigned k = r3 % 10;
        uint8_t proto = k < 6 ? 6 : k < 9 ? 17 : 1;
        uint16_t dport = proto == 6 ? (uint16_t)((k & 1) ? 443 : 80) : (proto == 17 ? 53 : 0);
        uint16_t sport = proto == 1 ? 0 : (uint16_t)(32768 + r4 % 20000);
        uint32_t start = rnd() % 90000;
        int count = 3 + (int)(rnd() % 40);
        uint32_t t = start;
        for (int i = 0; i < count && np + 2 < 6000; i++) {
            uint32_t gap = proto == 6 ? rnd() % 800 : (uint32_t)((i % 7 == 6) ? 20000 : rnd() % 3000);
            t += gap;
            for (int dir = 0; dir < 2; dir++) {
                Pkt *p = &pk[np++];
                p->ts = t + (uint32_t)dir * 3; p->seq = seq++;
                p->src = dir ? server : client; p->dst = dir ? client : server;
                p->sport = dir ? dport : sport; p->dport = dir ? sport : dport; p->proto = proto;
                p->len = (uint16_t)(dir ? 200 + rnd() % 1300 : 40 + rnd() % 200);
                p->flags = 0;
                if (proto == 6) {
                    p->flags = i == 0 ? (dir ? 0x12 : 0x02) : 0x10;
                    if (i == count - 1 && (c % 3 != 0)) p->flags |= (dir ? 0x01 : 0x04);
                }
                in_pkts++; in_bytes += p->len;
            }
        }
    }
    qsort(pk, (size_t)np, sizeof pk[0], cmp_pkt);
    static Cache cache;
    static Exporter ex;
    memset(&cache, 0, sizeof cache);
    memset(&ex, 0, sizeof ex);
    uint32_t last_sweep = 0, now = 0;
    for (int i = 0; i < np; i++) {
        Pkt *p = &pk[i];
        now = p->ts;
        if (now - last_sweep >= 1000) { sweep(&cache, &ex, now, 0); last_sweep = now; }
        Flow *f = find(&cache, p);
        if (!f) {
            for (int k = 0; k < MAXF; k++) if (!cache.f[k].live) { f = &cache.f[k]; break; }
            CHECK(f != NULL);
            memset(f, 0, sizeof *f);
            f->src = p->src; f->dst = p->dst; f->sport = p->sport; f->dport = p->dport; f->proto = p->proto;
            f->first = p->ts; f->live = 1;
        }
        f->pkts++; f->octets += p->len; f->last = p->ts; f->flags |= p->flags;
        if (p->proto == 6 && (p->flags & 0x05)) { emit(&ex, f, now); f->live = 0; } /* FIN or RST */
    }
    sweep(&cache, &ex, now + 60000, 1);
    flush_export(&ex, now + 60000);
    printf("input: %d packets, %llu bytes; exported %u flow records in %d datagrams\n", np, (unsigned long long)in_bytes, (unsigned)ex.total_flows, ex.nex);

    /* Collector. */
    uint64_t out_pkts = 0, out_bytes = 0;
    uint32_t expect_seq = 0;
    uint64_t proto_bytes[256];
    memset(proto_bytes, 0, sizeof proto_bytes);
    static Talker tk[64];
    int nt = 0;
    uint32_t max_dur = 0;
    for (int e = 0; e < ex.nex; e++) {
        const unsigned char *h = ex.ex[e].b;
        CHECK(r16(h) == 5);
        unsigned cnt = r16(h + 2);
        CHECK(cnt >= 1 && cnt <= 30 && r32(h + 16) == expect_seq);
        expect_seq += cnt;
        if (e < 3 || e == ex.nex - 1) printf("datagram %d: version=%u count=%u uptime=%ums flow_sequence=%u bytes=%u\n", e, r16(h), cnt, (unsigned)r32(h + 4), (unsigned)r32(h + 16), 24 + 48 * cnt);
        for (unsigned i = 0; i < cnt; i++) {
            const unsigned char *r = h + 24 + 48 * i;
            uint32_t src = r32(r), dpk = r32(r + 16), doc = r32(r + 20), first = r32(r + 24), last = r32(r + 28);
            CHECK(last >= first && dpk >= 1 && doc >= 40 * dpk);
            out_pkts += dpk; out_bytes += doc;
            proto_bytes[r[38]] += doc;
            if (last - first > max_dur) max_dur = last - first;
            int k;
            for (k = 0; k < nt; k++) if (tk[k].ip == src) break;
            if (k == nt) { CHECK(nt < 64); tk[nt].ip = src; tk[nt].bytes = tk[nt].pkts = 0; tk[nt].flows = 0; nt++; }
            tk[k].bytes += doc; tk[k].pkts += dpk; tk[k].flows++;
        }
    }
    CHECK(out_pkts == in_pkts && out_bytes == in_bytes && expect_seq == ex.total_flows);
    printf("collector totals match input: %llu packets, %llu bytes, longest flow %ums\n", (unsigned long long)out_pkts, (unsigned long long)out_bytes, (unsigned)max_dur);
    printf("by protocol: tcp=%llu udp=%llu icmp=%llu bytes\n", (unsigned long long)proto_bytes[6], (unsigned long long)proto_bytes[17], (unsigned long long)proto_bytes[1]);
    qsort(tk, (size_t)nt, sizeof tk[0], cmp_talker);
    printf("top talkers (%d sources):\n", nt);
    for (int i = 0; i < 5 && i < nt; i++) {
        char s[16];
        ipstr(tk[i].ip, s);
        printf("  %-15s %8llu bytes %5llu pkts %3u flows\n", s, (unsigned long long)tk[i].bytes, (unsigned long long)tk[i].pkts, (unsigned)tk[i].flows);
    }
    return 0;
}
