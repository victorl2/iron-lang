/*
 * title: ASN.1 BER TLV codec for SNMPv2c messages with a GetNext walk
 * topic: networking
 * covers: BER tag length value, short and long length forms, minimal two's complement integers, OID base-128 encoding, SNMP application types, PDU context tags, lexicographic OID ordering, MIB walk agent
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

enum { T_INT = 0x02, T_OCTETS = 0x04, T_NULL = 0x05, T_OID = 0x06, T_SEQ = 0x30, T_IPADDR = 0x40, T_COUNTER32 = 0x41, T_GAUGE32 = 0x42, T_TICKS = 0x43,
       T_NOSUCHOBJ = 0x80, T_ENDOFMIB = 0x82, P_GET = 0xa0, P_GETNEXT = 0xa1, P_RESPONSE = 0xa2, P_SET = 0xa3 };

typedef struct { unsigned char b[1024]; size_t n; } W;

static void put_len(W *w, size_t n) {
    if (n < 128) { w->b[w->n++] = (unsigned char)n; return; }
    int k = n > 0xffff ? 3 : n > 0xff ? 2 : 1;
    w->b[w->n++] = (unsigned char)(0x80 | k);
    for (int i = k - 1; i >= 0; i--) w->b[w->n++] = (unsigned char)(n >> (8 * i));
}

/* Build helpers write into a temp buffer then wrap. */
static void tlv(W *w, unsigned tag, const void *v, size_t n) {
    w->b[w->n++] = (unsigned char)tag;
    put_len(w, n);
    memcpy(w->b + w->n, v, n);
    w->n += n;
}

static void t_int(W *w, unsigned tag, int64_t v) {
    unsigned char t[9];
    int n = 0;
    uint64_t u = (uint64_t)v;
    for (int i = 7; i >= 0; i--) t[n++] = (unsigned char)(u >> (8 * i));
    int s = 0;
    while (s < 7 && ((t[s] == 0x00 && !(t[s + 1] & 0x80)) || (t[s] == 0xff && (t[s + 1] & 0x80)))) s++;
    tlv(w, tag, t + s, (size_t)(8 - s));
}
static void t_uint(W *w, unsigned tag, uint32_t v) { /* unsigned 32 bit: prefix 0 if high bit set */
    unsigned char t[5];
    int n = 0;
    if (v & 0x80000000u) t[n++] = 0;
    int started = n;
    for (int i = 3; i >= 0; i--) {
        unsigned char b = (unsigned char)(v >> (8 * i));
        if (!started && b == 0 && i > 0) continue;
        if (!started && (b & 0x80) && n == 0) t[n++] = 0;
        t[n++] = b; started = 1;
    }
    tlv(w, tag, t, (size_t)n);
}
static void t_octets(W *w, const char *s) { tlv(w, T_OCTETS, s, strlen(s)); }
static void t_null(W *w, unsigned tag) { tlv(w, tag, "", 0); }

static void t_oid(W *w, const uint32_t *a, size_t n) {
    unsigned char t[64];
    size_t k = 0;
    CHECK(n >= 2 && a[0] <= 2);
    for (size_t i = 1; i < n; i++) {
        uint32_t v = i == 1 ? a[0] * 40 + a[1] : a[i];
        if (i == 1 && n == 1) break;
        unsigned char tmp[5];
        int m = 0;
        do { tmp[m++] = (unsigned char)(v & 127); v >>= 7; } while (v);
        while (m--) t[k++] = (unsigned char)(tmp[m] | (m ? 128 : 0));
    }
    tlv(w, T_OID, t, k);
}

/* wrap the bytes written since `mark` in a constructed TLV */
static void wrap_from(W *w, size_t mark, unsigned tag) {
    unsigned char tmp[1024];
    size_t inner = w->n - mark;
    memcpy(tmp, w->b + mark, inner);
    w->n = mark;
    tlv(w, tag, tmp, inner);
}

typedef struct { unsigned tag; const unsigned char *v; size_t len; size_t total; } Tlv;

static const char *read_tlv(const unsigned char *p, size_t n, Tlv *t) {
    if (n < 2) return "truncated";
    t->tag = p[0];
    if ((p[0] & 31) == 31) return "high-tag-number-unsupported";
    size_t len, hdr = 2;
    if (p[1] < 128) len = p[1];
    else if (p[1] == 0x80) return "indefinite-length";
    else {
        size_t k = p[1] & 127;
        if (k > 4) return "length-too-large";
        if (n < 2 + k) return "truncated";
        len = 0;
        for (size_t i = 0; i < k; i++) len = (len << 8) | p[2 + i];
        if (len < 128 || (k > 1 && p[2] == 0)) return "non-minimal-length";
        hdr = 2 + k;
    }
    if (n - hdr < len) return "truncated";
    t->v = p + hdr; t->len = len; t->total = hdr + len;
    return NULL;
}

static const char *dec_int(const Tlv *t, int64_t *out) {
    if (t->len == 0 || t->len > 8) return "bad-integer-length";
    if (t->len > 1 && ((t->v[0] == 0 && !(t->v[1] & 0x80)) || (t->v[0] == 0xff && (t->v[1] & 0x80)))) return "non-minimal-integer";
    int64_t v = (t->v[0] & 0x80) ? -1 : 0;
    for (size_t i = 0; i < t->len; i++) v = (int64_t)(((uint64_t)v << 8) | t->v[i]);
    *out = v;
    return NULL;
}

static const char *dec_oid(const Tlv *t, uint32_t *a, size_t *n) {
    if (t->len == 0) return "empty-oid";
    size_t k = 0;
    uint64_t v = 0;
    for (size_t i = 0; i < t->len; i++) {
        if (v == 0 && t->v[i] == 0x80) return "non-minimal-subid";
        v = (v << 7) | (t->v[i] & 127);
        if (v > 0xffffffffull) return "subid-overflow";
        if (!(t->v[i] & 128)) {
            if (k == 0) { a[k++] = v < 80 ? (uint32_t)(v / 40) : 2; a[k++] = v < 80 ? (uint32_t)(v % 40) : (uint32_t)(v - 80); }
            else a[k++] = (uint32_t)v;
            v = 0;
        }
    }
    if (v) return "truncated-subid";
    *n = k;
    return NULL;
}

static int oid_cmp(const uint32_t *a, size_t an, const uint32_t *b, size_t bn) {
    for (size_t i = 0; i < an && i < bn; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return an == bn ? 0 : (an < bn ? -1 : 1);
}

static void oid_str(const uint32_t *a, size_t n, char *o) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) k += (size_t)snprintf(o + k, 96 - k, "%s%u", i ? "." : "", (unsigned)a[i]);
}

typedef struct { uint32_t oid[12]; size_t n; unsigned type; int64_t num; const char *str; const char *name; } MibEnt;

#define O(...) { __VA_ARGS__ }
static const MibEnt mib[] = {
    { O(1, 3, 6, 1, 2, 1, 1, 1, 0), 9, T_OCTETS, 0, "Iron test agent", "sysDescr.0" },
    { O(1, 3, 6, 1, 2, 1, 1, 3, 0), 9, T_TICKS, 123456, NULL, "sysUpTime.0" },
    { O(1, 3, 6, 1, 2, 1, 1, 5, 0), 9, T_OCTETS, 0, "router-7", "sysName.0" },
    { O(1, 3, 6, 1, 2, 1, 2, 1, 0), 9, T_INT, 2, NULL, "ifNumber.0" },
    { O(1, 3, 6, 1, 2, 1, 2, 2, 1, 10, 1), 11, T_COUNTER32, 4294967295LL, NULL, "ifInOctets.1" },
    { O(1, 3, 6, 1, 2, 1, 2, 2, 1, 10, 2), 11, T_COUNTER32, 1000, NULL, "ifInOctets.2" },
    { O(1, 3, 6, 1, 2, 1, 2, 2, 1, 5, 2), 11, T_GAUGE32, 100000000, NULL, "ifSpeed.2" },
};
#undef O
static const size_t nmib = sizeof mib / sizeof mib[0];

static int mib_cmp_sort(const void *a, const void *b) { const MibEnt *x = a, *y = b; return oid_cmp(x->oid, x->n, y->oid, y->n); }

static void tlv_describe(const Tlv *t, char *o, size_t cap) {
    int64_t v;
    uint32_t a[24];
    size_t an;
    switch (t->tag) {
    case T_INT: dec_int(t, &v); snprintf(o, cap, "INTEGER %lld", (long long)v); break;
    case T_OCTETS: snprintf(o, cap, "STRING \"%.*s\"", (int)t->len, (const char *)t->v); break;
    case T_NULL: snprintf(o, cap, "NULL"); break;
    case T_OID: dec_oid(t, a, &an); { char s[96]; oid_str(a, an, s); snprintf(o, cap, "OID %s", s); } break;
    case T_COUNTER32: case T_GAUGE32: case T_TICKS: { uint64_t u = 0; for (size_t i = 0; i < t->len; i++) u = (u << 8) | t->v[i]; snprintf(o, cap, "%s %llu", t->tag == T_COUNTER32 ? "Counter32" : t->tag == T_GAUGE32 ? "Gauge32" : "TimeTicks", (unsigned long long)u); break; }
    case T_NOSUCHOBJ: snprintf(o, cap, "noSuchObject"); break;
    case T_ENDOFMIB: snprintf(o, cap, "endOfMibView"); break;
    default: snprintf(o, cap, "tag 0x%02x", t->tag);
    }
}

static void build_msg(W *w, unsigned pdu, int32_t reqid, uint32_t (*oids)[12], const size_t *lens, size_t nv, const MibEnt *vals) {
    size_t m0 = w->n;
    t_int(w, T_INT, 1); /* version 2c */
    t_octets(w, "public");
    size_t m1 = w->n;
    t_int(w, T_INT, reqid); t_int(w, T_INT, 0); t_int(w, T_INT, 0);
    size_t m2 = w->n;
    for (size_t i = 0; i < nv; i++) {
        size_t m3 = w->n;
        t_oid(w, oids[i], lens[i]);
        if (!vals) t_null(w, T_NULL);
        else if (vals[i].type == 0) t_null(w, T_ENDOFMIB);
        else if (vals[i].type == T_OCTETS) t_octets(w, vals[i].str);
        else if (vals[i].type == T_INT) t_int(w, T_INT, vals[i].num);
        else t_uint(w, vals[i].type, (uint32_t)vals[i].num);
        wrap_from(w, m3, T_SEQ);
    }
    wrap_from(w, m2, T_SEQ);
    wrap_from(w, m1, pdu);
    wrap_from(w, m0, T_SEQ);
}

static const char *parse_msg(const unsigned char *p, size_t n, unsigned *pdu, int64_t *reqid, uint32_t (*oids)[12], size_t *lens, Tlv *vals, size_t *nv, char *comm) {
    Tlv t, ver, cm, pd, id, es, vb, item, o, v;
    const char *e;
    if ((e = read_tlv(p, n, &t))) return e;
    if (t.tag != T_SEQ) return "message-not-sequence";
    if (t.total != n) return "trailing-bytes";
    const unsigned char *q = t.v; size_t left = t.len;
    if ((e = read_tlv(q, left, &ver))) return e;
    int64_t vn;
    if (ver.tag != T_INT || (e = dec_int(&ver, &vn))) return "bad-version";
    if (vn != 0 && vn != 1) return "unsupported-version";
    q += ver.total; left -= ver.total;
    if ((e = read_tlv(q, left, &cm))) return e;
    if (cm.tag != T_OCTETS || cm.len >= 32) return "bad-community";
    memcpy(comm, cm.v, cm.len); comm[cm.len] = 0;
    q += cm.total; left -= cm.total;
    if ((e = read_tlv(q, left, &pd))) return e;
    if (pd.tag < 0xa0 || pd.tag > 0xa3) return "unsupported-pdu";
    *pdu = pd.tag;
    q = pd.v; left = pd.len;
    if ((e = read_tlv(q, left, &id)) || id.tag != T_INT || (e = dec_int(&id, reqid))) return e ? e : "bad-request-id";
    q += id.total; left -= id.total;
    for (int k = 0; k < 2; k++) { if ((e = read_tlv(q, left, &es)) || es.tag != T_INT) return e ? e : "bad-error-field"; q += es.total; left -= es.total; }
    if ((e = read_tlv(q, left, &vb))) return e;
    if (vb.tag != T_SEQ) return "bad-varbind-list";
    q = vb.v; left = vb.len;
    *nv = 0;
    while (left) {
        if ((e = read_tlv(q, left, &item))) return e;
        if (item.tag != T_SEQ) return "bad-varbind";
        if ((e = read_tlv(item.v, item.len, &o))) return e;
        if (o.tag != T_OID) return "varbind-name-not-oid";
        if ((e = read_tlv(item.v + o.total, item.len - o.total, &v))) return e;
        if (o.total + v.total != item.len) return "varbind-trailing";
        if (*nv >= 8) return "too-many-varbinds";
        if ((e = dec_oid(&o, oids[*nv], &lens[*nv]))) return e;
        vals[*nv] = v;
        (*nv)++;
        q += item.total; left -= item.total;
    }
    return NULL;
}

static void hex(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x%s", p[i], (i + 1) % 32 == 0 && i + 1 < n ? "\n      " : "");
    printf("\n");
}

int main(void) {
    static MibEnt sorted[16];
    memcpy(sorted, mib, sizeof mib);
    qsort(sorted, nmib, sizeof sorted[0], mib_cmp_sort);
    CHECK(memcmp(sorted, mib, sizeof mib) != 0); /* the source table is deliberately unsorted */

    /* Integer encodings. */
    static const struct { int64_t v; const char *hex; } ints[] = {
        { 0, "020100" }, { 127, "02017f" }, { 128, "02020080" }, { 255, "020200ff" }, { 256, "02020100" }, { -1, "0201ff" },
        { -128, "020180" }, { -129, "0202ff7f" }, { 32768, "0203008000" }, { -32768, "02028000" }, { 2147483647, "02047fffffff" }, { -2147483648LL, "020480000000" },
    };
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) {
        W w = { { 0 }, 0 };
        t_int(&w, T_INT, ints[i].v);
        char h[40];
        for (size_t k = 0; k < w.n; k++) snprintf(h + 2 * k, 3, "%02x", w.b[k]);
        CHECK(strcmp(h, ints[i].hex) == 0);
        Tlv t; int64_t back;
        CHECK(read_tlv(w.b, w.n, &t) == NULL && dec_int(&t, &back) == NULL && back == ints[i].v);
    }
    printf("%zu integer encodings match minimal two's complement\n", sizeof ints / sizeof ints[0]);
    /* OID encodings. */
    static const uint32_t o1[] = { 1, 3, 6, 1, 2, 1, 1, 1, 0 }, o2[] = { 1, 3, 6, 1, 4, 1, 311, 1 }, o3[] = { 2, 999, 3 }, o4[] = { 1, 3, 6, 1, 4, 1, 2097152, 4294967295u };
    const uint32_t *os[] = { o1, o2, o3, o4 };
    const size_t on[] = { 9, 8, 3, 8 };
    for (int i = 0; i < 4; i++) {
        W w = { { 0 }, 0 };
        t_oid(&w, os[i], on[i]);
        printf("OID ");
        char s[96]; oid_str(os[i], on[i], s);
        printf("%-28s ->", s);
        for (size_t k = 2; k < w.n; k++) printf(" %02x", w.b[k]);
        printf("\n");
        Tlv t; uint32_t back[24]; size_t bn;
        CHECK(read_tlv(w.b, w.n, &t) == NULL && dec_oid(&t, back, &bn) == NULL && bn == on[i] && memcmp(back, os[i], bn * 4) == 0);
    }
    /* Length forms. */
    for (size_t n = 126; n <= 300; n += (n < 130 ? 1 : 100)) {
        W w = { { 0 }, 0 };
        unsigned char big[400];
        memset(big, 'z', n);
        tlv(&w, T_OCTETS, big, n);
        Tlv t;
        CHECK(read_tlv(w.b, w.n, &t) == NULL && t.len == n);
        printf("length %zu uses %zu length byte(s)\n", n, t.total - n - 1);
    }

    /* Walk the MIB with GetNext requests as a manager would. */
    uint32_t cur[12] = { 1, 3, 6, 1, 2, 1 };
    size_t curn = 6;
    int steps = 0;
    for (;;) {
        W req = { { 0 }, 0 };
        uint32_t oids[1][12];
        memcpy(oids[0], cur, sizeof cur);
        size_t lens[1] = { curn };
        build_msg(&req, P_GETNEXT, 1000 + steps, oids, lens, 1, NULL);
        if (steps == 0) { printf("GetNext request (%zu bytes):\n      ", req.n); hex(req.b, req.n); }
        /* agent side */
        unsigned pdu; int64_t rid; uint32_t qo[8][12]; size_t ql[8]; Tlv qv[8]; size_t nv; char comm[32];
        const char *e = parse_msg(req.b, req.n, &pdu, &rid, qo, ql, qv, &nv, comm);
        CHECK(e == NULL && pdu == P_GETNEXT && nv == 1 && strcmp(comm, "public") == 0);
        const MibEnt *hit = NULL;
        for (size_t i = 0; i < nmib; i++) if (oid_cmp(sorted[i].oid, sorted[i].n, qo[0], ql[0]) > 0) { hit = &sorted[i]; break; }
        W rsp = { { 0 }, 0 };
        uint32_t ro[1][12];
        size_t rl[1];
        MibEnt endv = { { 0 }, 0, 0, 0, NULL, NULL };
        if (hit) { memcpy(ro[0], hit->oid, sizeof hit->oid); rl[0] = hit->n; }
        else { memcpy(ro[0], qo[0], sizeof ro[0]); rl[0] = ql[0]; }
        build_msg(&rsp, P_RESPONSE, (int32_t)rid, ro, rl, 1, hit ? hit : &endv);
        /* manager side */
        uint32_t mo[8][12]; size_t ml[8]; Tlv mv[8]; size_t mn; char c2[32];
        e = parse_msg(rsp.b, rsp.n, &pdu, &rid, mo, ml, mv, &mn, c2);
        CHECK(e == NULL && pdu == P_RESPONSE && rid == 1000 + steps);
        char d[96], os2[96];
        tlv_describe(&mv[0], d, sizeof d);
        oid_str(mo[0], ml[0], os2);
        if (mv[0].tag == T_ENDOFMIB) { printf("  %-24s %s\n", os2, d); break; }
        printf("  %-24s %-16s %s\n", os2, hit->name, d);
        memcpy(cur, mo[0], sizeof cur);
        curn = ml[0];
        steps++;
        CHECK(steps < 20);
    }
    CHECK(steps == (int)nmib);
    printf("walk visited %d objects\n", steps);

    /* Malformed messages. */
    W good = { { 0 }, 0 };
    uint32_t oids[1][12];
    memcpy(oids[0], o1, sizeof o1);
    size_t lens[1] = { 9 };
    build_msg(&good, P_GET, 7, oids, lens, 1, NULL);
    struct { const char *label; size_t at; unsigned char val; size_t trunc; } muts[] = {
        { "truncated message", 0, 0, 20 }, { "indefinite length", 1, 0x80, 0 }, { "wrong outer tag", 0, 0x31, 0 }, { "unsupported version", 4, 5, 0 },
        { "non-minimal length", 1, 0x81, 0 }, { "PDU tag 0xa4", 13, 0xa4, 0 },
    };
    for (size_t i = 0; i < sizeof muts / sizeof muts[0]; i++) {
        unsigned char m[256];
        memcpy(m, good.b, good.n);
        size_t n = good.n;
        if (muts[i].trunc) n = muts[i].trunc; else m[muts[i].at] = muts[i].val;
        unsigned pdu; int64_t rid; uint32_t qo[8][12]; size_t ql[8]; Tlv qv[8]; size_t nv; char comm[32];
        const char *e = parse_msg(m, n, &pdu, &rid, qo, ql, qv, &nv, comm);
        printf("%-20s -> %s\n", muts[i].label, e ? e : "accepted");
    }
    return 0;
}
