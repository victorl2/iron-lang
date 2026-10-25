/*
 * title: DHCP message and option TLV codec
 * topic: networking
 * covers: BOOTP fixed header, magic cookie, option TLVs, pad and end options, long option concatenation, option overload into file and sname, lease time arithmetic, validation
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

enum { O_PAD = 0, O_SUBNET = 1, O_ROUTER = 3, O_DNS = 6, O_HOSTNAME = 12, O_REQIP = 50, O_LEASE = 51, O_OVERLOAD = 52, O_MSGTYPE = 53,
       O_SERVERID = 54, O_PARAMREQ = 55, O_MAXMSG = 57, O_CLIENTID = 61, O_END = 255 };

#define FIXED 236
#define SNAME_OFF 44
#define SNAME_LEN 64
#define FILE_OFF 108
#define FILE_LEN 128

typedef struct { unsigned char *b; size_t n; size_t cap; } Buf;

static void put(Buf *b, const void *d, size_t n) { CHECK(b->n + n <= b->cap); memcpy(b->b + b->n, d, n); b->n += n; }
static void opt(Buf *b, unsigned code, const void *d, size_t n) {
    /* options longer than 255 are split into consecutive instances of the same code */
    const unsigned char *p = d;
    do {
        size_t c = n > 255 ? 255 : n;
        unsigned char h[2] = { (unsigned char)code, (unsigned char)c };
        put(b, h, 2);
        put(b, p, c);
        p += c; n -= c;
    } while (n > 0);
}
static void opt_u32(Buf *b, unsigned code, uint32_t v) {
    unsigned char t[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
    opt(b, code, t, 4);
}
static void opt_u8(Buf *b, unsigned code, unsigned v) { unsigned char t = (unsigned char)v; opt(b, code, &t, 1); }

static void header(Buf *b, unsigned op, uint32_t xid, const unsigned char mac[6], uint32_t yiaddr) {
    unsigned char h[FIXED];
    memset(h, 0, sizeof h);
    h[0] = (unsigned char)op; h[1] = 1; h[2] = 6; h[3] = 0;
    for (int i = 0; i < 4; i++) { h[4 + i] = (unsigned char)(xid >> (24 - 8 * i)); h[16 + i] = (unsigned char)(yiaddr >> (24 - 8 * i)); }
    h[10] = 0x80; /* broadcast flag */
    memcpy(h + 28, mac, 6);
    put(b, h, sizeof h);
    static const unsigned char cookie[4] = { 99, 130, 83, 99 };
    put(b, cookie, 4);
}

typedef struct { const unsigned char *val; size_t len; } Opt;
typedef struct { Opt o[256]; int present[256]; } Opts;

/* returns 0 ok, else error text; walks options in [p, p+n) and appends to the table */
static const char *walk(const unsigned char *p, size_t n, Opts *t, unsigned char *scratch, size_t *scratch_n, int *saw_end) {
    size_t i = 0;
    *saw_end = 0;
    while (i < n) {
        unsigned c = p[i++];
        if (c == O_PAD) continue;
        if (c == O_END) { *saw_end = 1; return NULL; }
        if (i >= n) return "missing-length";
        size_t l = p[i++];
        if (i + l > n) return "option-overruns";
        if (t->present[c]) {
            /* concatenate continuation: copy old + new into scratch */
            size_t old = t->o[c].len;
            CHECK(*scratch_n + old + l <= 4096);
            unsigned char *dst = scratch + *scratch_n;
            memcpy(dst, t->o[c].val, old);
            memcpy(dst + old, p + i, l);
            *scratch_n += old + l;
            t->o[c].val = dst;
            t->o[c].len = old + l;
        } else {
            t->present[c] = 1;
            t->o[c].val = p + i;
            t->o[c].len = l;
        }
        i += l;
    }
    return NULL;
}

static const char *parse(const unsigned char *m, size_t n, Opts *t, unsigned char *scratch) {
    memset(t, 0, sizeof *t);
    size_t sn = 0;
    if (n < FIXED + 4 + 1) return "too-short";
    if (m[FIXED] != 99 || m[FIXED + 1] != 130 || m[FIXED + 2] != 83 || m[FIXED + 3] != 99) return "bad-cookie";
    int end;
    const char *e = walk(m + FIXED + 4, n - FIXED - 4, t, scratch, &sn, &end);
    if (e) return e;
    if (!end) return "no-end-option";
    if (t->present[O_OVERLOAD] && t->o[O_OVERLOAD].len == 1) {
        unsigned ov = t->o[O_OVERLOAD].val[0];
        if (ov & 1) { e = walk(m + FILE_OFF, FILE_LEN, t, scratch, &sn, &end); if (e) return e; }
        if (ov & 2) { e = walk(m + SNAME_OFF, SNAME_LEN, t, scratch, &sn, &end); if (e) return e; }
    }
    if (!t->present[O_MSGTYPE] || t->o[O_MSGTYPE].len != 1) return "no-message-type";
    return NULL;
}

static const char *mtname(unsigned t) {
    static const char *n[] = { "?", "DISCOVER", "OFFER", "REQUEST", "DECLINE", "ACK", "NAK", "RELEASE", "INFORM" };
    return t < 9 ? n[t] : "?";
}

static uint32_t u32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static void ipstr(uint32_t a, char *s) { snprintf(s, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255)); }

static void show(const unsigned char *m, size_t n) {
    static unsigned char scratch[4096];
    Opts t;
    const char *e = parse(m, n, &t, scratch);
    if (e) { printf("  parse error: %s\n", e); return; }
    char a[16], b[16];
    printf("  op=%u xid=%08x type=%s", m[0], (unsigned)u32(m + 4), mtname(t.o[O_MSGTYPE].val[0]));
    ipstr(u32(m + 16), a);
    printf(" yiaddr=%s\n", a);
    if (t.present[O_SUBNET]) { ipstr(u32(t.o[O_SUBNET].val), a); printf("  subnet mask %s\n", a); }
    if (t.present[O_ROUTER]) { ipstr(u32(t.o[O_ROUTER].val), a); printf("  router %s\n", a); }
    if (t.present[O_DNS]) { printf("  dns:"); for (size_t i = 0; i + 4 <= t.o[O_DNS].len; i += 4) { ipstr(u32(t.o[O_DNS].val + i), a); printf(" %s", a); } printf("\n"); }
    if (t.present[O_LEASE]) {
        uint32_t l = u32(t.o[O_LEASE].val);
        printf("  lease %u s (%uh), T1=%u T2=%u\n", (unsigned)l, (unsigned)(l / 3600), (unsigned)(l / 2), (unsigned)(l / 8 * 7));
    }
    if (t.present[O_HOSTNAME]) printf("  hostname \"%.*s\"\n", (int)t.o[O_HOSTNAME].len, (const char *)t.o[O_HOSTNAME].val);
    if (t.present[O_SERVERID]) { ipstr(u32(t.o[O_SERVERID].val), a); printf("  server id %s\n", a); }
    if (t.present[O_REQIP]) { ipstr(u32(t.o[O_REQIP].val), b); printf("  requested ip %s\n", b); }
    if (t.present[O_PARAMREQ]) { printf("  param request list:"); for (size_t i = 0; i < t.o[O_PARAMREQ].len; i++) printf(" %u", t.o[O_PARAMREQ].val[i]); printf("\n"); }
    if (t.present[O_CLIENTID]) { printf("  client id:"); for (size_t i = 0; i < t.o[O_CLIENTID].len; i++) printf("%s%02x", i ? ":" : " ", t.o[O_CLIENTID].val[i]); printf("\n"); }
    int cnt = 0;
    for (int i = 0; i < 256; i++) cnt += t.present[i];
    printf("  %d distinct options\n", cnt);
}

int main(void) {
    static unsigned char pkt[1024];
    static const unsigned char mac[6] = { 0x02, 0x00, 0x00, 0x12, 0x34, 0x56 };
    Buf b = { pkt, 0, sizeof pkt };
    header(&b, 1, 0x3903f326u, mac, 0);
    opt_u8(&b, O_MSGTYPE, 1);
    unsigned char cid[7] = { 1 };
    memcpy(cid + 1, mac, 6);
    opt(&b, O_CLIENTID, cid, 7);
    opt(&b, O_HOSTNAME, "laptop-7", 8);
    static const unsigned char pl[] = { 1, 3, 6, 15, 28, 51 };
    opt(&b, O_PARAMREQ, pl, sizeof pl);
    unsigned char e = O_END;
    put(&b, &e, 1);
    printf("DISCOVER (%zu bytes)\n", b.n);
    show(pkt, b.n);

    Buf o = { pkt, 0, sizeof pkt };
    header(&o, 2, 0x3903f326u, mac, 0xc0a8010au);
    opt_u8(&o, O_MSGTYPE, 2);
    opt_u32(&o, O_SERVERID, 0xc0a80101u);
    opt_u32(&o, O_LEASE, 86400);
    opt_u32(&o, O_SUBNET, 0xffffff00u);
    opt_u32(&o, O_ROUTER, 0xc0a80101u);
    unsigned char dns[8] = { 1, 1, 1, 1, 9, 9, 9, 9 };
    opt(&o, O_DNS, dns, 8);
    unsigned char pad = 0;
    put(&o, &pad, 1); put(&o, &pad, 1);
    put(&o, &e, 1);
    printf("OFFER (%zu bytes)\n", o.n);
    show(pkt, o.n);

    /* Option overload: put lease and hostname in the file field, message type stays in options. */
    Buf v = { pkt, 0, sizeof pkt };
    header(&v, 2, 0x11223344u, mac, 0x0a000005u);
    opt_u8(&v, O_MSGTYPE, 5);
    opt_u8(&v, O_OVERLOAD, 1);
    put(&v, &e, 1);
    Buf f = { pkt + FILE_OFF, 0, FILE_LEN };
    opt_u32(&f, O_LEASE, 7200);
    opt(&f, O_HOSTNAME, "overloaded", 10);
    put(&f, &e, 1);
    printf("ACK with overload (%zu bytes)\n", v.n);
    show(pkt, v.n);

    /* A 300 byte option is split into 255 + 45 and re-joined. */
    Buf l = { pkt, 0, sizeof pkt };
    header(&l, 1, 1, mac, 0);
    opt_u8(&l, O_MSGTYPE, 3);
    unsigned char big[300];
    for (int i = 0; i < 300; i++) big[i] = (unsigned char)(i * 7);
    opt(&l, 43, big, 300);
    put(&l, &e, 1);
    Opts t;
    static unsigned char scratch[4096];
    CHECK(parse(pkt, l.n, &t, scratch) == NULL);
    CHECK(t.o[43].len == 300 && memcmp(t.o[43].val, big, 300) == 0);
    printf("vendor option 43 reassembled from %d chunks: %zu bytes\n", 2, t.o[43].len);

    /* Errors. */
    Buf x = { pkt, 0, sizeof pkt };
    header(&x, 1, 1, mac, 0);
    opt_u8(&x, O_MSGTYPE, 1);
    printf("errors: %s", parse(pkt, x.n, &t, scratch));
    put(&x, &e, 1);
    pkt[FIXED] = 0;
    printf(" %s", parse(pkt, x.n, &t, scratch));
    pkt[FIXED] = 99;
    pkt[FIXED + 5] = 40;
    printf(" %s", parse(pkt, x.n, &t, scratch));
    Buf y = { pkt, 0, sizeof pkt };
    header(&y, 1, 1, mac, 0);
    put(&y, &e, 1);
    printf(" %s", parse(pkt, y.n, &t, scratch));
    printf(" %s\n", parse(pkt, 100, &t, scratch));
    return 0;
}
