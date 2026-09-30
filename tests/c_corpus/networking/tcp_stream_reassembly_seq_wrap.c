/*
 * title: TCP receive-side reassembly across sequence number wraparound
 * topic: networking
 * covers: modular sequence arithmetic, out-of-order segments, duplicate and overlapping retransmissions, window trimming, cumulative ACK and SACK block generation, FIN sequence consumption, retransmission rounds
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

#define WND 4096

typedef struct {
    uint32_t rcv_nxt;
    unsigned char buf[WND];
    unsigned char have[WND];
    unsigned char *out; size_t outn;
    int fin_seen; uint32_t fin_seq;
    int closed;
    unsigned dups, dropped, trimmed_head, trimmed_tail, ooo_peak, delivered_events;
    uint32_t last_lo, last_hi; /* most recent out-of-order block, absolute seq */
} Rx;

typedef struct { uint32_t l, r; } Blk;

static int32_t sdiff(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

static void rx_init(Rx *r, uint32_t isn, unsigned char *out) {
    memset(r, 0, sizeof *r);
    r->rcv_nxt = isn + 1; /* SYN consumed */
    r->out = out;
}

static unsigned ooo_bytes(const Rx *r) { unsigned n = 0; for (int i = 0; i < WND; i++) n += r->have[i]; return n; }

static void deliver(Rx *r) {
    size_t k = 0;
    while (k < WND && r->have[k]) k++;
    if (!k) return;
    memcpy(r->out + r->outn, r->buf, k);
    r->outn += k;
    r->rcv_nxt += (uint32_t)k;
    memmove(r->buf, r->buf + k, WND - k);
    memmove(r->have, r->have + k, WND - k);
    memset(r->buf + WND - k, 0, k);
    memset(r->have + WND - k, 0, k);
    r->delivered_events++;
}

/* Returns SACK blocks (max 3) after processing. */
static int rx_segment(Rx *r, uint32_t seq, const unsigned char *d, uint32_t len, int fin, Blk *sack) {
    int32_t off = sdiff(seq, r->rcv_nxt);
    if (fin) { r->fin_seen = 1; r->fin_seq = seq + len; }
    if (off + (int64_t)len <= 0 && len > 0) { r->dups++; goto report; }
    if (off < 0) { uint32_t cut = (uint32_t)(-off); d += cut; len -= cut; off = 0; r->trimmed_head++; }
    if (off >= WND) { r->dropped++; goto report; }
    if ((uint64_t)off + len > WND) { len = (uint32_t)(WND - off); r->trimmed_tail++; }
    {
        int fresh = 0;
        for (uint32_t i = 0; i < len; i++) {
            if (!r->have[off + i]) { r->have[off + i] = 1; r->buf[off + i] = d[i]; fresh = 1; }
        }
        if (!fresh && len) r->dups++;
        if (off > 0 && len) { r->last_lo = r->rcv_nxt + (uint32_t)off; r->last_hi = r->last_lo + len; }
        deliver(r);
    }
    if (r->fin_seen && r->rcv_nxt == r->fin_seq) { r->rcv_nxt++; r->closed = 1; }
    if (ooo_bytes(r) > r->ooo_peak) r->ooo_peak = ooo_bytes(r);
report:;
    /* build SACK blocks from the bitmap */
    Blk all[16];
    int na = 0;
    for (int i = 0; i < WND && na < 16;) {
        if (!r->have[i]) { i++; continue; }
        int j = i;
        while (j < WND && r->have[j]) j++;
        all[na].l = r->rcv_nxt + (uint32_t)i; all[na].r = r->rcv_nxt + (uint32_t)j; na++;
        i = j;
    }
    int ns = 0;
    /* first block: the one holding the most recent segment */
    for (int i = 0; i < na && ns < 3; i++)
        if (sdiff(r->last_lo, all[i].l) >= 0 && sdiff(r->last_lo, all[i].r) < 0) { sack[ns++] = all[i]; all[i].l = all[i].r; }
    for (int i = 0; i < na && ns < 3; i++) if (all[i].l != all[i].r) sack[ns++] = all[i];
    return ns;
}

static uint32_t rs = 0x53455131u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

typedef struct { uint32_t pos, len; } Seg;

int main(void) {
    static unsigned char stream[20000], out[20000];
    for (size_t i = 0; i < sizeof stream; i++) stream[i] = (unsigned char)(rnd() >> 6);
    uint32_t isn = 0xfffffe00u;

    /* Scripted example around the wrap point. */
    {
        Rx r;
        rx_init(&r, isn, out);
        printf("isn=0x%08x first expected seq=0x%08x\n", (unsigned)isn, (unsigned)r.rcv_nxt);
        struct { uint32_t pos, len; } script[] = { { 0, 100 }, { 200, 100 }, { 100, 100 }, { 600, 100 }, { 300, 50 }, { 350, 250 }, { 300, 50 }, { 250, 200 }, { 650, 100 }, { 700, 100 } };
        for (size_t i = 0; i < sizeof script / sizeof script[0]; i++) {
            Blk sk[3];
            uint32_t seq = isn + 1 + script[i].pos;
            int ns = rx_segment(&r, seq, stream + script[i].pos, script[i].len, 0, sk);
            printf("seg seq=0x%08x len=%3u -> ack=0x%08x", (unsigned)seq, (unsigned)script[i].len, (unsigned)r.rcv_nxt);
            for (int b = 0; b < ns; b++) printf(" sack[0x%08x,0x%08x)", (unsigned)sk[b].l, (unsigned)sk[b].r);
            printf("\n");
        }
        printf("delivered %zu bytes, dups=%u trimmed_head=%u\n", r.outn, r.dups, r.trimmed_head);
        CHECK(r.outn == 800 && memcmp(out, stream, 800) == 0);
    }
    /* First-arrival wins on conflicting overlap. */
    {
        Rx r;
        rx_init(&r, 1000, out);
        Blk sk[3];
        unsigned char a[10], b[10];
        memset(a, 'A', 10); memset(b, 'B', 10);
        rx_segment(&r, 1001 + 5, a, 10, 0, sk);
        rx_segment(&r, 1001, b, 10, 0, sk);
        printf("conflicting overlap: delivered [%.15s] (first arrival kept bytes 5..9 as A)\n", (const char *)out);
        CHECK(r.outn == 15 && memcmp(out, "BBBBBAAAAAAAAAA", 15) == 0);
    }

    /* Randomized transfers with reordering, duplicates, loss and retransmission rounds. */
    int trials = 0;
    unsigned tot_rounds = 0, tot_dups = 0, tot_drop = 0, peak = 0, tot_events = 0;
    for (int t = 0; t < 40; t++) {
        uint32_t base_isn = (t % 4 == 0) ? isn : (t % 4 == 1) ? 0xffffffffu - (rnd() % 3000) : rnd();
        size_t total = 3000 + rnd() % 15000;
        static Seg segs[400];
        int ns = 0;
        uint32_t pos = 0;
        while (pos < total && ns < 400) {
            uint32_t l = 1 + rnd() % 900;
            if (l > total - pos) l = (uint32_t)(total - pos);
            segs[ns].pos = pos; segs[ns].len = l; ns++;
            pos += l;
        }
        if (pos < total) { segs[ns - 1].len += (uint32_t)(total - pos); }
        Rx r;
        memset(out, 0, sizeof out);
        rx_init(&r, base_isn, out);
        int rounds = 0;
        while (!r.closed) {
            rounds++;
            CHECK(rounds < 200);
            /* pick unacked segments, jitter their order, sometimes duplicate or lose */
            static Seg q[800];
            int nq = 0;
            for (int i = 0; i < ns; i++) {
                if (segs[i].pos + segs[i].len <= r.outn) continue;
                if (segs[i].pos >= r.outn + WND) break; /* sender window */
                if (rnd() % 6 == 0) continue; /* lost */
                q[nq++] = segs[i];
                if (rnd() % 8 == 0) q[nq++] = segs[i];
                if (rnd() % 10 == 0 && segs[i].len > 2) { q[nq].pos = segs[i].pos + 1; q[nq].len = segs[i].len - 1; nq++; }
            }
            for (int i = 0; i < nq; i++) { int j = i + (int)(rnd() % 6); if (j < nq) { Seg tmp = q[i]; q[i] = q[j]; q[j] = tmp; } }
            for (int i = 0; i < nq; i++) {
                Blk sk[3];
                uint32_t len = q[i].len;
                int ns2 = rx_segment(&r, base_isn + 1 + q[i].pos, stream + q[i].pos, len, 0, sk);
                for (int b = 0; b < ns2; b++) CHECK(sdiff(sk[b].l, r.rcv_nxt) > 0 && sdiff(sk[b].r, sk[b].l) > 0);
            }
            if (r.outn >= total && !r.fin_seen) {
                Blk sk[3];
                rx_segment(&r, base_isn + 1 + (uint32_t)total, NULL, 0, 1, sk);
            }
        }
        CHECK(r.outn == total && memcmp(out, stream, total) == 0);
        CHECK(r.rcv_nxt == base_isn + 1 + (uint32_t)total + 1);
        tot_rounds += (unsigned)rounds; tot_dups += r.dups; tot_drop += r.dropped; tot_events += r.delivered_events;
        if (r.ooo_peak > peak) peak = r.ooo_peak;
        trials++;
    }
    printf("%d transfers reassembled exactly (mix of wrapping and non-wrapping ISNs)\n", trials);
    printf("retransmission rounds=%u duplicates=%u out-of-window drops=%u delivery bursts=%u peak out-of-order bytes=%u\n", tot_rounds, tot_dups, tot_drop, tot_events, peak);
    return 0;
}
