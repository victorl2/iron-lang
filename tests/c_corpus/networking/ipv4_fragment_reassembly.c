/*
 * title: IPv4 fragmentation and hole-tracking reassembly
 * topic: networking
 * covers: MTU fragmentation, fragment offset in 8 byte units, more-fragments flag, out of order arrival, duplicates, overlaps, DF handling, incomplete datagrams
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { unsigned id, off8, mf; size_t len; unsigned char data[1500]; } Frag;

static uint32_t rs = 0x0badc0deu;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

/* Returns fragment count, or -1 if DF is set and the packet does not fit. */
static int fragment(const unsigned char *pay, size_t n, unsigned id, size_t mtu, int df, Frag *out, int maxf) {
    size_t room = mtu - 20;
    if (n <= room) {
        if (maxf < 1) return -2;
        out[0].id = id; out[0].off8 = 0; out[0].mf = 0; out[0].len = n; memcpy(out[0].data, pay, n);
        return 1;
    }
    if (df) return -1;
    size_t chunk = room & ~(size_t)7;
    int k = 0;
    for (size_t pos = 0; pos < n; pos += chunk, k++) {
        CHECK(k < maxf);
        size_t l = n - pos < chunk ? n - pos : chunk;
        out[k].id = id;
        out[k].off8 = (unsigned)(pos / 8);
        out[k].mf = (pos + l < n);
        out[k].len = l;
        memcpy(out[k].data, pay + pos, l);
    }
    return k;
}

typedef struct {
    unsigned char buf[8192];
    unsigned char have[8192];
    size_t total; /* known once the last fragment arrives, else 0 */
    size_t got;
    int overlap_bytes, dup_frags;
} Reasm;

static void reasm_init(Reasm *r) { memset(r, 0, sizeof *r); }

/* first-arrival wins on overlapping bytes */
static void reasm_add(Reasm *r, const Frag *f) {
    size_t base = (size_t)f->off8 * 8;
    CHECK(base + f->len <= sizeof r->buf);
    int fresh = 0;
    for (size_t i = 0; i < f->len; i++) {
        if (r->have[base + i]) { r->overlap_bytes++; continue; }
        r->have[base + i] = 1;
        r->buf[base + i] = f->data[i];
        r->got++;
        fresh = 1;
    }
    if (!fresh) r->dup_frags++;
    if (!f->mf) r->total = base + f->len;
}

static int reasm_done(const Reasm *r) { return r->total && r->got == r->total; }

int main(void) {
    unsigned char pay[4000];
    for (size_t i = 0; i < sizeof pay; i++) pay[i] = (unsigned char)(rnd() >> 11);

    /* Sizes vs MTU table. */
    static const struct { size_t n; size_t mtu; int df; } cases[] = {
        { 1480, 1500, 0 }, { 1481, 1500, 0 }, { 3000, 1500, 0 }, { 4000, 576, 0 }, { 3000, 1500, 1 }, { 100, 68, 0 },
    };
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        static Frag fr[600];
        int k = fragment(pay, cases[c].n, 0x1234, cases[c].mtu, cases[c].df, fr, 600);
        if (k < 0) { printf("payload %zu mtu %zu df=%d -> cannot fragment (needs ICMP frag-needed)\n", cases[c].n, cases[c].mtu, cases[c].df); continue; }
        Reasm r;
        reasm_init(&r);
        for (int i = 0; i < k; i++) reasm_add(&r, &fr[i]);
        CHECK(reasm_done(&r) && memcmp(r.buf, pay, cases[c].n) == 0);
        printf("payload %zu mtu %zu -> %d fragments, last off=%u mf=%u len=%zu\n", cases[c].n, cases[c].mtu, k, fr[k - 1].off8, fr[k - 1].mf, fr[k - 1].len);
    }

    /* Shuffled arrival, random duplicates, many trials. */
    static Frag fr[600], seq[900];
    int trials_ok = 0;
    long dups = 0;
    for (int t = 0; t < 200; t++) {
        size_t n = 1 + rnd() % sizeof pay;
        size_t mtu = 68 + rnd() % 1433;
        int k = fragment(pay, n, (unsigned)t, mtu, 0, fr, 600);
        CHECK(k > 0);
        int m = 0;
        for (int i = 0; i < k; i++) {
            seq[m++] = fr[i];
            if (rnd() % 5 == 0) { seq[m++] = fr[i]; dups++; }
        }
        for (int i = m - 1; i > 0; i--) { int j = (int)(rnd() % (uint32_t)(i + 1)); Frag tmp = seq[i]; seq[i] = seq[j]; seq[j] = tmp; }
        Reasm r;
        reasm_init(&r);
        int done_at = -1;
        for (int i = 0; i < m; i++) {
            reasm_add(&r, &seq[i]);
            if (done_at < 0 && reasm_done(&r)) done_at = i;
        }
        CHECK(done_at >= 0 && r.total == n && memcmp(r.buf, pay, n) == 0);
        trials_ok++;
    }
    printf("200 shuffled trials reassembled (%ld duplicate fragments injected)\n", dups);
    CHECK(trials_ok == 200);

    /* Missing fragment leaves a hole. */
    static Frag f3[10];
    int k = fragment(pay, 3000, 7, 1000, 0, f3, 10);
    Reasm r;
    reasm_init(&r);
    for (int i = 0; i < k; i++) if (i != 1) reasm_add(&r, &f3[i]);
    size_t hole_start = 0;
    while (hole_start < r.total && r.have[hole_start]) hole_start++;
    size_t hole_end = hole_start;
    while (hole_end < r.total && !r.have[hole_end]) hole_end++;
    printf("dropped fragment 1 of %d: incomplete, got %zu of %zu bytes, hole [%zu,%zu)\n", k, r.got, r.total, hole_start, hole_end);
    CHECK(!reasm_done(&r));
    reasm_add(&r, &f3[1]);
    CHECK(reasm_done(&r));

    /* Overlapping fragment with conflicting bytes: first arrival wins. */
    Frag a, b;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.off8 = 0; a.len = 32; a.mf = 1; memset(a.data, 0xaa, 32);
    b.off8 = 2; b.len = 32; b.mf = 0; memset(b.data, 0xbb, 32); /* covers bytes 16..47 */
    reasm_init(&r);
    reasm_add(&r, &a);
    reasm_add(&r, &b);
    printf("overlap: overlapped %d bytes, total %zu, byte15=%02x byte16=%02x byte31=%02x byte32=%02x\n", r.overlap_bytes, r.total, r.buf[15], r.buf[16], r.buf[31], r.buf[32]);
    CHECK(reasm_done(&r) && r.buf[16] == 0xaa && r.buf[32] == 0xbb && r.overlap_bytes == 16);
    return 0;
}
