/*
 * title: DNS message wire codec with name compression and loop defence
 * topic: networking
 * covers: DNS header flags, label encoding, suffix compression pointers, RDATA for A AAAA CNAME MX TXT, pointer loop detection, forward pointer rejection, malformed input classification
 * deps: libc
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { unsigned char *b; size_t n, cap; struct { size_t off; char name[256]; } tab[64]; int nt; int compress; } W;

static void wb(W *w, unsigned v) { CHECK(w->n < w->cap); w->b[w->n++] = (unsigned char)v; }
static void w16(W *w, unsigned v) { wb(w, v >> 8); wb(w, v & 255); }
static void w32(W *w, uint32_t v) { w16(w, v >> 16); w16(w, v & 0xffff); }

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static void wname(W *w, const char *name) {
    const char *p = name;
    while (*p) {
        if (w->compress) {
            for (int i = 0; i < w->nt; i++)
                if (ieq(w->tab[i].name, p)) { w16(w, 0xc000u | (unsigned)w->tab[i].off); return; }
        }
        if (w->nt < 64 && w->n < 0x4000) {
            w->tab[w->nt].off = w->n;
            snprintf(w->tab[w->nt].name, 256, "%s", p);
            w->nt++;
        }
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        CHECK(l > 0 && l <= 63);
        wb(w, (unsigned)l);
        for (size_t i = 0; i < l; i++) wb(w, (unsigned char)p[i]);
        p += l;
        if (*p == '.') p++;
    }
    wb(w, 0);
}

typedef enum { D_OK, D_TRUNC, D_LOOP, D_FORWARD, D_BADLABEL, D_TOOLONG } DErr;
static const char *derr[] = { "ok", "truncated", "pointer-loop", "forward-pointer", "bad-label-type", "name-too-long" };

static DErr rname(const unsigned char *m, size_t n, size_t *off, char *out, int backward_only) {
    size_t pos = *off, olen = 0;
    int hops = 0, jumped = 0;
    size_t resume = 0;
    out[0] = 0;
    for (;;) {
        if (pos >= n) return D_TRUNC;
        unsigned c = m[pos];
        if (c == 0) { pos++; break; }
        if ((c & 0xc0) == 0xc0) {
            if (pos + 1 >= n) return D_TRUNC;
            size_t tgt = (size_t)(((c & 0x3f) << 8) | m[pos + 1]);
            if (!jumped) resume = pos + 2;
            jumped = 1;
            if (backward_only && tgt >= pos) return D_FORWARD;
            if (++hops > 32) return D_LOOP;
            pos = tgt;
            continue;
        }
        if (c & 0xc0) return D_BADLABEL;
        if (pos + 1 + c > n) return D_TRUNC;
        if (olen + c + 1 > 254) return D_TOOLONG;
        if (olen) out[olen++] = '.';
        memcpy(out + olen, m + pos + 1, c);
        olen += c;
        out[olen] = 0;
        pos += 1 + c;
    }
    *off = jumped ? resume : pos;
    if (!olen) { out[0] = '.'; out[1] = 0; }
    return D_OK;
}

typedef struct { const char *name; unsigned type; uint32_t ttl; const char *target; unsigned pref; uint32_t ip; unsigned char v6[16]; const char *txt; } RR;

static void wrr(W *w, const RR *r) {
    wname(w, r->name);
    w16(w, r->type); w16(w, 1); w32(w, r->ttl);
    size_t lenpos = w->n;
    w16(w, 0);
    size_t start = w->n;
    switch (r->type) {
    case 1: w32(w, r->ip); break;
    case 28: for (int i = 0; i < 16; i++) wb(w, r->v6[i]); break;
    case 2: case 5: wname(w, r->target); break;
    case 15: w16(w, r->pref); wname(w, r->target); break;
    case 16: { size_t l = strlen(r->txt); wb(w, (unsigned)l); for (size_t i = 0; i < l; i++) wb(w, (unsigned char)r->txt[i]); break; }
    default: CHECK(0);
    }
    size_t rl = w->n - start;
    w->b[lenpos] = (unsigned char)(rl >> 8);
    w->b[lenpos + 1] = (unsigned char)rl;
}

static const char *tname(unsigned t) {
    switch (t) { case 1: return "A"; case 2: return "NS"; case 5: return "CNAME"; case 15: return "MX"; case 16: return "TXT"; case 28: return "AAAA"; default: return "?"; }
}

static DErr dump(const unsigned char *m, size_t n, int backward, int quiet, int *nrr) {
    if (n < 12) return D_TRUNC;
    unsigned flags = (unsigned)((m[2] << 8) | m[3]);
    unsigned qd = (unsigned)((m[4] << 8) | m[5]), an = (unsigned)((m[6] << 8) | m[7]);
    if (!quiet) printf("  id=%04x %s opcode=%u aa=%u tc=%u rd=%u ra=%u rcode=%u qd=%u an=%u\n", (unsigned)((m[0] << 8) | m[1]), (flags & 0x8000) ? "response" : "query",
                       (flags >> 11) & 15, (flags >> 10) & 1, (flags >> 9) & 1, (flags >> 8) & 1, (flags >> 7) & 1, flags & 15, qd, an);
    size_t off = 12;
    char nm[300], tg[300];
    *nrr = 0;
    for (unsigned i = 0; i < qd; i++) {
        DErr e = rname(m, n, &off, nm, backward);
        if (e) return e;
        if (off + 4 > n) return D_TRUNC;
        if (!quiet) printf("  Q %s %s\n", nm, tname((unsigned)((m[off] << 8) | m[off + 1])));
        off += 4;
    }
    for (unsigned i = 0; i < an; i++) {
        DErr e = rname(m, n, &off, nm, backward);
        if (e) return e;
        if (off + 10 > n) return D_TRUNC;
        unsigned t = (unsigned)((m[off] << 8) | m[off + 1]);
        uint32_t ttl = ((uint32_t)m[off + 4] << 24) | ((uint32_t)m[off + 5] << 16) | ((uint32_t)m[off + 6] << 8) | m[off + 7];
        unsigned rl = (unsigned)((m[off + 8] << 8) | m[off + 9]);
        off += 10;
        if (off + rl > n) return D_TRUNC;
        size_t end = off + rl;
        if (!quiet) printf("  %-20s %5u %-5s ", nm, (unsigned)ttl, tname(t));
        if (t == 1) { if (!quiet) printf("%u.%u.%u.%u\n", m[off], m[off + 1], m[off + 2], m[off + 3]); }
        else if (t == 28) { if (!quiet) { for (int k = 0; k < 8; k++) printf("%x%s", (unsigned)((m[off + 2 * k] << 8) | m[off + 2 * k + 1]), k < 7 ? ":" : "\n"); } }
        else if (t == 2 || t == 5) { size_t o2 = off; e = rname(m, n, &o2, tg, backward); if (e) return e; if (!quiet) printf("%s\n", tg); }
        else if (t == 15) { size_t o2 = off + 2; e = rname(m, n, &o2, tg, backward); if (e) return e; if (!quiet) printf("%u %s\n", (unsigned)((m[off] << 8) | m[off + 1]), tg); }
        else if (t == 16) { if (!quiet) printf("\"%.*s\"\n", (int)m[off], (const char *)m + off + 1); }
        off = end;
        (*nrr)++;
    }
    return D_OK;
}

int main(void) {
    static unsigned char buf[2048];
    RR rrs[] = {
        { "www.example.com", 5, 300, "web.example.com", 0, 0, { 0 }, NULL },
        { "web.example.com", 1, 60, NULL, 0, 0x5db8d822u, { 0 }, NULL },
        { "web.example.com", 28, 60, NULL, 0, 0, { 0x26, 0x06, 0x28, 0, 0x02, 0x20, 0, 1, 0x02, 0x48, 0x18, 0x93, 0x25, 0xc8, 0x19, 0x46 }, NULL },
        { "example.com", 15, 3600, "mail.Example.COM", 10, 0, { 0 }, NULL },
        { "example.com", 2, 86400, "ns1.example.com", 0, 0, { 0 }, NULL },
        { "example.com", 16, 120, NULL, 0, 0, { 0 }, "v=spf1 -all" },
    };
    int nr = (int)(sizeof rrs / sizeof rrs[0]);
    size_t sizes[2];
    for (int comp = 1; comp >= 0; comp--) {
        W w = { buf, 0, sizeof buf, { { 0, { 0 } } }, 0, comp };
        w16(&w, 0xabcd); w16(&w, 0x8180); w16(&w, 1); w16(&w, (unsigned)nr); w16(&w, 0); w16(&w, 0);
        wname(&w, "www.example.com"); w16(&w, 1); w16(&w, 1);
        for (int i = 0; i < nr; i++) wrr(&w, &rrs[i]);
        sizes[comp] = w.n;
        int cnt;
        printf("%s message: %zu bytes\n", comp ? "compressed" : "uncompressed", w.n);
        DErr e = dump(buf, w.n, 1, comp == 0, &cnt);
        CHECK(e == D_OK && cnt == nr);
    }
    printf("compression saved %zu bytes\n", sizes[0] - sizes[1]);
    CHECK(sizes[1] < sizes[0]);

    /* Malformed inputs. */
    struct { const char *label; unsigned char m[64]; size_t n; } bad[] = {
        { "self pointer", { 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xc0, 12, 0, 1, 0, 1 }, 18 },
        { "two-node loop", { 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 0xc0, 12 + 4, 1, 'b', 0xc0, 12, 0, 1, 0, 1 }, 24 },
        { "forward pointer", { 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xc0, 18, 0, 1, 0, 1, 3, 'f', 'o', 'o', 0 }, 23 },
        { "reserved label bits", { 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0x80, 1, 0, 0, 1, 0, 1 }, 19 },
        { "label past end", { 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 9, 'a', 'b' }, 15 },
        { "header only", { 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 }, 12 },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int c;
        DErr e1 = dump(bad[i].m, bad[i].n, 0, 1, &c);
        DErr e2 = dump(bad[i].m, bad[i].n, 1, 1, &c);
        printf("%-20s hop-limit policy: %-15s backward-only policy: %s\n", bad[i].label, derr[e1], derr[e2]);
        CHECK(e2 != D_OK && (e1 != D_OK || i == 2));
    }
    /* A 255 byte name is rejected. */
    unsigned char big[400];
    memset(big, 0, sizeof big);
    big[5] = 1;
    size_t o = 12;
    for (int i = 0; i < 5; i++) { big[o++] = 60; memset(big + o, 'x', 60); o += 60; }
    big[o++] = 0;
    int c;
    printf("over-long name: %s\n", derr[dump(big, o + 4, 1, 1, &c)]);

    /* Random compression round trips: decode(encode(names)) equals the names. */
    static const char *pool[] = { "a", "bb", "ccc", "example", "org", "mail", "x-y" };
    uint32_t rs = 0x91e10da5u;
    long saved = 0;
    for (int t = 0; t < 300; t++) {
        W w = { buf, 0, sizeof buf, { { 0, { 0 } } }, 0, 1 };
        char names[10][80];
        size_t offs[10];
        int k = 3 + t % 6;
        for (int i = 0; i < k; i++) {
            int labels = 1 + (int)(rs % 4);
            rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
            names[i][0] = 0;
            for (int j = 0; j < labels; j++) {
                uint32_t r = rs;
                rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
                if (j) strcat(names[i], ".");
                strcat(names[i], pool[r % 7]);
            }
            offs[i] = w.n;
            wname(&w, names[i]);
            saved += (long)strlen(names[i]) + 2 - (long)(w.n - offs[i]);
        }
        for (int i = 0; i < k; i++) {
            size_t off = offs[i];
            char out[300];
            CHECK(rname(buf, w.n, &off, out, 1) == D_OK);
            CHECK(strcmp(out, names[i]) == 0);
        }
    }
    printf("300 random name sets round-trip; bytes saved by suffix pointers: %ld\n", saved);
    return 0;
}
