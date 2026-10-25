/*
 * title: Ethernet frames with 802.1Q and QinQ tags plus ARP packets
 * topic: networking
 * covers: Ethernet II framing, VLAN tag PCP/DEI/VID, stacked tags, minimum frame padding, CRC-32 frame check sequence, ARP request reply and gratuitous forms, decode validation
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static uint32_t crc32_bytes(const unsigned char *p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

typedef struct { unsigned tpid, pcp, dei, vid; } Tag;
typedef struct {
    unsigned char dst[6], src[6];
    Tag tags[2];
    int ntags;
    unsigned ethertype;
    const unsigned char *payload;
    size_t plen;
} Frame;

static size_t encode(const Frame *f, unsigned char *o) {
    size_t n = 0;
    memcpy(o, f->dst, 6); memcpy(o + 6, f->src, 6); n = 12;
    for (int i = 0; i < f->ntags; i++) {
        o[n++] = (unsigned char)(f->tags[i].tpid >> 8); o[n++] = (unsigned char)f->tags[i].tpid;
        unsigned tci = (f->tags[i].pcp << 13) | (f->tags[i].dei << 12) | f->tags[i].vid;
        o[n++] = (unsigned char)(tci >> 8); o[n++] = (unsigned char)tci;
    }
    o[n++] = (unsigned char)(f->ethertype >> 8); o[n++] = (unsigned char)f->ethertype;
    memcpy(o + n, f->payload, f->plen); n += f->plen;
    while (n < 60) o[n++] = 0; /* pad to minimum frame without FCS */
    uint32_t fcs = crc32_bytes(o, n);
    for (int i = 0; i < 4; i++) o[n++] = (unsigned char)(fcs >> (8 * i)); /* FCS goes out least significant byte first */
    return n;
}

static const char *decode(const unsigned char *p, size_t n, Frame *f, size_t *pay_off) {
    if (n < 64) return "runt";
    uint32_t want = crc32_bytes(p, n - 4);
    uint32_t got = (uint32_t)p[n - 4] | ((uint32_t)p[n - 3] << 8) | ((uint32_t)p[n - 2] << 16) | ((uint32_t)p[n - 1] << 24);
    if (want != got) return "bad-fcs";
    memcpy(f->dst, p, 6); memcpy(f->src, p + 6, 6);
    size_t o = 12;
    f->ntags = 0;
    for (;;) {
        unsigned t = (unsigned)((p[o] << 8) | p[o + 1]);
        if (t == 0x8100 || t == 0x88a8) {
            if (f->ntags == 2) return "too-many-tags";
            unsigned tci = (unsigned)((p[o + 2] << 8) | p[o + 3]);
            f->tags[f->ntags].tpid = t;
            f->tags[f->ntags].pcp = tci >> 13;
            f->tags[f->ntags].dei = (tci >> 12) & 1;
            f->tags[f->ntags].vid = tci & 0xfff;
            f->ntags++;
            o += 4;
        } else {
            f->ethertype = t;
            o += 2;
            break;
        }
    }
    *pay_off = o;
    f->payload = p + o;
    f->plen = n - 4 - o;
    return "ok";
}

static void mac(const unsigned char *m, char *s) {
    snprintf(s, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

typedef struct { unsigned oper; unsigned char sha[6], tha[6]; unsigned char spa[4], tpa[4]; } Arp;

static void arp_encode(const Arp *a, unsigned char *o) {
    o[0] = 0; o[1] = 1; o[2] = 8; o[3] = 0; o[4] = 6; o[5] = 4;
    o[6] = (unsigned char)(a->oper >> 8); o[7] = (unsigned char)a->oper;
    memcpy(o + 8, a->sha, 6); memcpy(o + 14, a->spa, 4); memcpy(o + 18, a->tha, 6); memcpy(o + 24, a->tpa, 4);
}

static const char *arp_decode(const unsigned char *p, size_t n, Arp *a) {
    if (n < 28) return "short";
    if (p[0] != 0 || p[1] != 1 || p[2] != 8 || p[3] != 0) return "unsupported-hw-or-proto";
    if (p[4] != 6 || p[5] != 4) return "bad-lengths";
    a->oper = (unsigned)((p[6] << 8) | p[7]);
    if (a->oper != 1 && a->oper != 2) return "bad-oper";
    memcpy(a->sha, p + 8, 6); memcpy(a->spa, p + 14, 4); memcpy(a->tha, p + 18, 6); memcpy(a->tpa, p + 24, 4);
    return "ok";
}

static const char *arp_kind(const Arp *a) {
    if (a->oper == 1 && memcmp(a->spa, a->tpa, 4) == 0) return "gratuitous-request";
    if (a->oper == 1) return "who-has";
    return "is-at";
}

int main(void) {
    static const unsigned char bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const unsigned char ma[6] = { 0x02, 0x00, 0x00, 0xaa, 0xbb, 0x01 }, mb[6] = { 0x02, 0x00, 0x00, 0xaa, 0xbb, 0x02 };
    unsigned char apay[28], fr[2048];
    Arp req = { 1, { 0 }, { 0 }, { 10, 0, 0, 1 }, { 10, 0, 0, 2 } };
    memcpy(req.sha, ma, 6);
    arp_encode(&req, apay);

    /* Untagged, single tag, QinQ. */
    Frame variants[3];
    memset(variants, 0, sizeof variants);
    for (int i = 0; i < 3; i++) {
        memcpy(variants[i].dst, bcast, 6); memcpy(variants[i].src, ma, 6);
        variants[i].ethertype = 0x0806; variants[i].payload = apay; variants[i].plen = 28;
        variants[i].ntags = i;
    }
    variants[1].tags[0] = (Tag){ 0x8100, 5, 0, 100 };
    variants[2].tags[0] = (Tag){ 0x88a8, 3, 1, 4094 };
    variants[2].tags[1] = (Tag){ 0x8100, 0, 0, 7 };
    for (int i = 0; i < 3; i++) {
        size_t n = encode(&variants[i], fr);
        Frame d;
        size_t off;
        const char *st = decode(fr, n, &d, &off);
        CHECK(strcmp(st, "ok") == 0);
        CHECK(d.ntags == i && d.ethertype == 0x0806 && memcmp(d.payload, apay, 28) == 0);
        printf("frame %d: %zu bytes on wire, payload offset %zu, tags=%d", i, n, off, d.ntags);
        for (int t = 0; t < d.ntags; t++) printf(" [tpid=%04x pcp=%u dei=%u vid=%u]", d.tags[t].tpid, d.tags[t].pcp, d.tags[t].dei, d.tags[t].vid);
        printf(" fcs=%02x%02x%02x%02x\n", fr[n - 1], fr[n - 2], fr[n - 3], fr[n - 4]);
        Arp a;
        CHECK(strcmp(arp_decode(d.payload, 28, &a), "ok") == 0);
    }

    /* Minimum size padding: a 3 byte payload becomes a 64 byte frame. */
    Frame small;
    memset(&small, 0, sizeof small);
    memcpy(small.dst, mb, 6); memcpy(small.src, ma, 6);
    small.ethertype = 0x88b5; small.payload = (const unsigned char *)"abc"; small.plen = 3;
    size_t sn = encode(&small, fr);
    Frame d;
    size_t off;
    CHECK(strcmp(decode(fr, sn, &d, &off), "ok") == 0);
    printf("tiny payload: wire=%zu, decoded payload length including pad=%zu, first bytes %02x%02x%02x\n", sn, d.plen, d.payload[0], d.payload[1], d.payload[2]);
    CHECK(sn == 64 && d.plen == 46);

    /* Corruption: flip a bit in each region. */
    size_t n = encode(&variants[1], fr);
    unsigned char t[2048];
    memcpy(t, fr, n); t[20] ^= 1;
    printf("corrupt payload: %s\n", decode(t, n, &d, &off));
    printf("runt: %s\n", decode(fr, 40, &d, &off));

    /* ARP exchange with a tiny cache. */
    Arp reply = { 2, { 0 }, { 0 }, { 10, 0, 0, 2 }, { 10, 0, 0, 1 } };
    memcpy(reply.sha, mb, 6); memcpy(reply.tha, ma, 6);
    Arp grat = { 1, { 0 }, { 0 }, { 10, 0, 0, 9 }, { 10, 0, 0, 9 } };
    static const unsigned char mg[6] = { 0x02, 0, 0, 0xcc, 0xdd, 0x09 };
    memcpy(grat.sha, mg, 6);
    const Arp *seq[3] = { &req, &reply, &grat };
    struct { unsigned char ip[4]; unsigned char mac[6]; } cache[4];
    int nc = 0;
    for (int i = 0; i < 3; i++) {
        unsigned char b[28];
        arp_encode(seq[i], b);
        Arp a;
        CHECK(strcmp(arp_decode(b, 28, &a), "ok") == 0);
        char m1[18];
        mac(a.sha, m1);
        printf("arp %-18s %u.%u.%u.%u is-sender %s target %u.%u.%u.%u\n", arp_kind(&a), a.spa[0], a.spa[1], a.spa[2], a.spa[3], m1, a.tpa[0], a.tpa[1], a.tpa[2], a.tpa[3]);
        int found = -1;
        for (int k = 0; k < nc; k++) if (memcmp(cache[k].ip, a.spa, 4) == 0) found = k;
        if (found < 0) { found = nc++; memcpy(cache[found].ip, a.spa, 4); }
        memcpy(cache[found].mac, a.sha, 6);
    }
    printf("cache entries: %d\n", nc);
    CHECK(nc == 3);
    unsigned char bad[28];
    arp_encode(&req, bad);
    bad[4] = 8;
    printf("arp errors: %s", arp_decode(bad, 28, &(Arp){ 0 }));
    arp_encode(&req, bad); bad[7] = 9;
    printf(" %s", arp_decode(bad, 28, &(Arp){ 0 }));
    printf(" %s\n", arp_decode(bad, 20, &(Arp){ 0 }));
    return 0;
}
