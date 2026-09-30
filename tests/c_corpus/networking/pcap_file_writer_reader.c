/*
 * title: pcap capture file writer and reader for synthesized packets
 * topic: networking
 * covers: pcap global header and record header layout, microsecond and nanosecond magic, byte-swapped files, snaplen truncation, truncated trailing records, corrupt lengths, protocol dissection with checksum verification
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static uint32_t rs = 0x70636170u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static uint16_t csum(const unsigned char *p, size_t n, uint32_t init) {
    uint32_t s = init;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
    if (n & 1) s += (uint32_t)(p[n - 1] << 8);
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static void w16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void w32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (24 - 8 * i)); }

/* Build Ethernet + IPv4 + L4. Returns frame length. */
static size_t build(unsigned char *f, int proto, uint32_t src, uint32_t dst, unsigned sp, unsigned dp, const unsigned char *pay, size_t pn, unsigned id) {
    memset(f, 0, 14);
    f[0] = 0x02; f[5] = 0x02; f[6] = 0x02; f[11] = 0x01; w16(f + 12, 0x0800);
    unsigned char *ip = f + 14;
    size_t l4 = proto == 17 ? 8 + pn : proto == 6 ? 20 + pn : 8 + pn;
    ip[0] = 0x45; ip[1] = 0; w16(ip + 2, (unsigned)(20 + l4)); w16(ip + 4, id); w16(ip + 6, 0x4000); ip[8] = 64; ip[9] = (unsigned char)proto;
    w32(ip + 12, src); w32(ip + 16, dst);
    w16(ip + 10, 0);
    w16(ip + 10, csum(ip, 20, 0));
    unsigned char *t = ip + 20;
    if (proto == 17) { w16(t, sp); w16(t + 2, dp); w16(t + 4, (unsigned)l4); w16(t + 6, 0); memcpy(t + 8, pay, pn); }
    else if (proto == 6) { w16(t, sp); w16(t + 2, dp); w32(t + 4, 1000 + id); w32(t + 8, 0); t[12] = 0x50; t[13] = 0x18; w16(t + 14, 8192); w16(t + 16, 0); w16(t + 18, 0); memcpy(t + 20, pay, pn); }
    else { t[0] = 8; t[1] = 0; w16(t + 2, 0); w16(t + 4, sp); w16(t + 6, id); memcpy(t + 8, pay, pn); }
    uint32_t ph = 0;
    if (proto != 1) {
        unsigned char pseudo[12];
        w32(pseudo, src); w32(pseudo + 4, dst); pseudo[8] = 0; pseudo[9] = (unsigned char)proto; w16(pseudo + 10, (unsigned)l4);
        for (int i = 0; i < 12; i += 2) ph += (uint32_t)((pseudo[i] << 8) | pseudo[i + 1]);
    }
    uint16_t c = csum(t, l4, ph);
    if (proto == 17) w16(t + 6, c ? c : 0xffff); else if (proto == 6) w16(t + 16, c); else w16(t + 2, c);
    return 14 + 20 + l4;
}

typedef struct { unsigned char *b; size_t n, cap; } Out;
static void put(Out *o, const void *d, size_t n) {
    if (o->n + n > o->cap) { o->cap = (o->n + n) * 2; o->b = realloc(o->b, o->cap); CHECK(o->b); }
    memcpy(o->b + o->n, d, n);
    o->n += n;
}

typedef struct { int big_endian, nano; uint32_t snaplen, link; } PcapInfo;

static void write_header(Out *o, int be, int nano, uint32_t snaplen) {
    unsigned char h[24];
    uint32_t magic = nano ? 0xa1b23c4du : 0xa1b2c3d4u;
    for (int i = 0; i < 4; i++) h[i] = (unsigned char)(be ? magic >> (24 - 8 * i) : magic >> (8 * i));
    unsigned char *p = h + 4;
    unsigned vals16[2] = { 2, 4 };
    for (int k = 0; k < 2; k++, p += 2) { if (be) w16(p, vals16[k]); else { p[0] = (unsigned char)vals16[k]; p[1] = 0; } }
    uint32_t vals32[4] = { 0, 0, snaplen, 1 };
    for (int k = 0; k < 4; k++, p += 4) { if (be) w32(p, vals32[k]); else for (int i = 0; i < 4; i++) p[i] = (unsigned char)(vals32[k] >> (8 * i)); }
    put(o, h, 24);
}

static void write_rec(Out *o, int be, uint32_t sec, uint32_t frac, const unsigned char *d, size_t len, uint32_t snaplen) {
    size_t incl = len > snaplen ? snaplen : len;
    unsigned char h[16];
    uint32_t v[4] = { sec, frac, (uint32_t)incl, (uint32_t)len };
    for (int k = 0; k < 4; k++) { if (be) w32(h + 4 * k, v[k]); else for (int i = 0; i < 4; i++) h[4 * k + i] = (unsigned char)(v[k] >> (8 * i)); }
    put(o, h, 16);
    put(o, d, incl);
}

static uint32_t rd32(const unsigned char *p, int be) {
    return be ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]
              : ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static const char *read_header(const unsigned char *b, size_t n, PcapInfo *pi) {
    if (n < 24) return "short-header";
    uint32_t m = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    if (m == 0xa1b2c3d4u) { pi->big_endian = 1; pi->nano = 0; }
    else if (m == 0xd4c3b2a1u) { pi->big_endian = 0; pi->nano = 0; }
    else if (m == 0xa1b23c4du) { pi->big_endian = 1; pi->nano = 1; }
    else if (m == 0x4d3cb2a1u) { pi->big_endian = 0; pi->nano = 1; }
    else return "bad-magic";
    unsigned maj = pi->big_endian ? (unsigned)((b[4] << 8) | b[5]) : (unsigned)((b[5] << 8) | b[4]);
    if (maj != 2) return "unsupported-version";
    pi->snaplen = rd32(b + 16, pi->big_endian);
    pi->link = rd32(b + 20, pi->big_endian);
    return NULL;
}

typedef struct { unsigned n, udp, tcp, icmp, other, bad_csum, truncated; uint64_t bytes; } Stats;

static void dissect(const unsigned char *f, size_t incl, size_t orig, uint32_t sec, uint32_t frac, int nano, Stats *st, int verbose) {
    st->n++; st->bytes += orig;
    unsigned dsec = sec % 86400;
    char desc[120] = "";
    if (incl < orig) st->truncated++;
    if (incl >= 34 && f[12] == 0x08 && f[13] == 0x00) {
        const unsigned char *ip = f + 14;
        unsigned proto = ip[9];
        size_t tot = (size_t)((ip[2] << 8) | ip[3]);
        int ip_ok = csum(ip, 20, 0) == 0;
        int l4_ok = 1;
        if (incl == orig && incl >= 14 + tot) {
            size_t l4 = tot - 20;
            uint32_t ph = 0;
            if (proto != 1) {
                unsigned char ps[12];
                memcpy(ps, ip + 12, 8); ps[8] = 0; ps[9] = (unsigned char)proto; w16(ps + 10, (unsigned)l4);
                for (int i = 0; i < 12; i += 2) ph += (uint32_t)((ps[i] << 8) | ps[i + 1]);
            }
            l4_ok = csum(ip + 20, l4, ph) == 0;
        }
        if (!ip_ok || !l4_ok) st->bad_csum++;
        const unsigned char *t = ip + 20;
        const char *name = proto == 17 ? "UDP" : proto == 6 ? "TCP" : proto == 1 ? "ICMP" : "IP";
        if (proto == 17) st->udp++; else if (proto == 6) st->tcp++; else if (proto == 1) st->icmp++; else st->other++;
        if (proto == 1) snprintf(desc, sizeof desc, "%u.%u.%u.%u > %u.%u.%u.%u: ICMP echo %s id %u seq %u", ip[12], ip[13], ip[14], ip[15], ip[16], ip[17], ip[18], ip[19], t[0] == 8 ? "request" : "reply", (unsigned)((t[4] << 8) | t[5]), (unsigned)((t[6] << 8) | t[7]));
        else snprintf(desc, sizeof desc, "%u.%u.%u.%u.%u > %u.%u.%u.%u.%u: %s len %zu", ip[12], ip[13], ip[14], ip[15], (unsigned)((t[0] << 8) | t[1]), ip[16], ip[17], ip[18], ip[19], (unsigned)((t[2] << 8) | t[3]), name, tot - 20 - (proto == 6 ? 20 : 8));
        if (!ip_ok || !l4_ok) strcat(desc, " [bad checksum]");
    } else {
        st->other++;
        snprintf(desc, sizeof desc, "non-IPv4 frame");
    }
    if (verbose) printf("  %02u:%02u:%02u.%0*u caplen=%zu len=%zu %s\n", dsec / 3600, (dsec / 60) % 60, dsec % 60, nano ? 9 : 6, (unsigned)frac, incl, orig, desc);
}

static const char *read_all(const unsigned char *b, size_t n, Stats *st, int verbose, PcapInfo *pi) {
    memset(st, 0, sizeof *st);
    const char *e = read_header(b, n, pi);
    if (e) return e;
    size_t pos = 24;
    while (pos < n) {
        if (n - pos < 16) return "truncated-record-header";
        uint32_t sec = rd32(b + pos, pi->big_endian), frac = rd32(b + pos + 4, pi->big_endian);
        uint32_t incl = rd32(b + pos + 8, pi->big_endian), orig = rd32(b + pos + 12, pi->big_endian);
        if (incl > pi->snaplen || incl > orig) return "corrupt-record-length";
        pos += 16;
        if (n - pos < incl) return "truncated-record-data";
        dissect(b + pos, incl, orig, sec, frac, pi->nano, st, verbose);
        pos += incl;
    }
    return NULL;
}

int main(void) {
    /* Write a capture file to the scratch directory, then read it back. */
    Out o = { NULL, 0, 0 };
    write_header(&o, 0, 0, 96);
    unsigned char f[1600], pay[200];
    uint32_t base = 1700000000u + 12345;
    int nudp = 0, ntcp = 0, nicmp = 0;
    unsigned total_bytes = 0;
    for (int i = 0; i < 12; i++) {
        size_t pn = 4 + rnd() % 150;
        for (size_t k = 0; k < pn; k++) pay[k] = (unsigned char)rnd();
        int kind = i % 3;
        int proto = kind == 0 ? 17 : kind == 1 ? 6 : 1;
        size_t len = build(f, proto, 0x0a000001u + (uint32_t)(i % 4), 0x0a000064u, 1024 + (unsigned)i, proto == 17 ? 53 : 80, pay, pn, (unsigned)i);
        write_rec(&o, 0, base + (uint32_t)i, (uint32_t)(i * 137 + 5), f, len, 96);
        total_bytes += (unsigned)len;
        nudp += proto == 17; ntcp += proto == 6; nicmp += proto == 1;
    }
    FILE *fp = fopen("cap.pcap", "wb");
    CHECK(fp && fwrite(o.b, 1, o.n, fp) == o.n);
    fclose(fp);
    printf("wrote %zu bytes: 24 header + 12 records\n", o.n);
    fp = fopen("cap.pcap", "rb");
    CHECK(fp);
    unsigned char *rb = malloc(o.n + 1);
    CHECK(rb && fread(rb, 1, o.n + 1, fp) == o.n);
    fclose(fp);
    remove("cap.pcap");
    Stats st;
    PcapInfo pi;
    const char *e = read_all(rb, o.n, &st, 1, &pi);
    CHECK(e == NULL);
    printf("read: %s-endian, %s timestamps, snaplen %u, link type %u\n", pi.big_endian ? "big" : "little", pi.nano ? "nano" : "micro", (unsigned)pi.snaplen, (unsigned)pi.link);
    printf("packets=%u udp=%u tcp=%u icmp=%u truncated=%u bad_checksums=%u wire bytes=%llu\n", st.n, st.udp, st.tcp, st.icmp, st.truncated, st.bad_csum, (unsigned long long)st.bytes);
    CHECK(st.n == 12 && st.udp == (unsigned)nudp && st.tcp == (unsigned)ntcp && st.icmp == (unsigned)nicmp && st.bytes == total_bytes);
    free(rb);

    /* The same packets in the other three header variants must yield identical statistics. */
    for (int variant = 1; variant < 4; variant++) {
        Out v = { NULL, 0, 0 };
        int be = variant & 1, nano = variant >> 1;
        write_header(&v, be, nano, 65535);
        rs = 0x70636170u ^ 0x5555u;
        for (int i = 0; i < 12; i++) {
            size_t pn = 4 + rnd() % 150;
            for (size_t k = 0; k < pn; k++) pay[k] = (unsigned char)rnd();
            int proto = i % 3 == 0 ? 17 : i % 3 == 1 ? 6 : 1;
            size_t len = build(f, proto, 0x0a000001u + (uint32_t)(i % 4), 0x0a000064u, 1024 + (unsigned)i, 80, pay, pn, (unsigned)i);
            write_rec(&v, be, base + (uint32_t)i, nano ? (uint32_t)i * 1000003u : (uint32_t)i * 999, f, len, 65535);
        }
        Stats s2; PcapInfo p2;
        CHECK(read_all(v.b, v.n, &s2, 0, &p2) == NULL);
        CHECK(p2.big_endian == be && p2.nano == nano && s2.n == 12 && s2.bad_csum == 0 && s2.truncated == 0);
        printf("variant %s-endian %s: %u packets, %u bytes total, snaplen %u\n", be ? "big" : "little", nano ? "nano" : "micro", s2.n, (unsigned)s2.bytes, (unsigned)p2.snaplen);
        free(v.b);
    }
    /* Damage cases. */
    Out g = { NULL, 0, 0 };
    write_header(&g, 0, 0, 96);
    size_t len = build(f, 17, 1, 2, 3, 4, (const unsigned char *)"payload!", 8, 1);
    write_rec(&g, 0, 1, 2, f, len, 96);
    write_rec(&g, 0, 3, 4, f, len, 96);
    Stats s3; PcapInfo p3;
    printf("intact 2-record file: %s\n", read_all(g.b, g.n, &s3, 0, &p3) ? "error" : "ok");
    printf("cut mid-record: %s\n", read_all(g.b, g.n - 5, &s3, 0, &p3));
    printf("cut mid-header: %s\n", read_all(g.b, 24 + 16 + len + 7, &s3, 0, &p3));
    unsigned char save = g.b[0];
    g.b[0] = 0x00;
    printf("bad magic: %s\n", read_all(g.b, g.n, &s3, 0, &p3));
    g.b[0] = save;
    g.b[24 + 8] = 0xff; g.b[24 + 9] = 0xff;
    printf("record longer than snaplen: %s\n", read_all(g.b, g.n, &s3, 0, &p3));
    g.b[24 + 8] = (unsigned char)len; g.b[24 + 9] = 0;
    g.b[24 + 16 + 14 + 22] ^= 0x01; /* flip a bit inside the IP header of record 1 payload */
    CHECK(read_all(g.b, g.n, &s3, 0, &p3) == NULL);
    printf("bit flip inside a frame: bad_checksums=%u of %u\n", s3.bad_csum, s3.n);
    /* Truncation by snaplen keeps the original length. */
    Out s = { NULL, 0, 0 };
    write_header(&s, 0, 0, 60);
    unsigned char bigpay[500];
    memset(bigpay, 'A', sizeof bigpay);
    len = build(f, 17, 1, 2, 3, 4, bigpay, sizeof bigpay, 9);
    write_rec(&s, 0, 100, 1, f, len, 60);
    CHECK(read_all(s.b, s.n, &s3, 1, &p3) == NULL && s3.truncated == 1);
    free(g.b); free(s.b); free(o.b);
    return 0;
}
