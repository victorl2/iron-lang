/*
 * title: Copy-free message buffers parsed in place with views
 * topic: memory
 * covers: zero-copy parsing, borrowed slices, header parse in place, scatter list, memcpy avoidance audit
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const unsigned char *p;
    size_t n;
} View;

/*
 * Frame: u8 type, u8 nfields, u16 total length (LE), then nfields TLV entries:
 * u8 tag, u16 len (LE), bytes. Parsing hands back views into the original buffer.
 */
typedef struct {
    uint8_t type;
    uint8_t nfields;
    View field[6];
    uint8_t tag[6];
} Frame;

typedef enum { P_OK, P_SHORT, P_LENGTH, P_TOO_MANY, P_TRAILING } Perr;

static const char *perr_name(Perr e) {
    static const char *n[] = {"ok", "short", "length", "too-many", "trailing"};
    return n[e];
}

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "check failed: %s\n", what);
        exit(1);
    }
}

static Perr parse(View buf, Frame *f) {
    if (buf.n < 4)
        return P_SHORT;
    f->type = buf.p[0];
    f->nfields = buf.p[1];
    size_t total = (size_t)buf.p[2] | ((size_t)buf.p[3] << 8);
    if (total != buf.n)
        return P_LENGTH;
    if (f->nfields > 6)
        return P_TOO_MANY;
    size_t pos = 4;
    for (unsigned i = 0; i < f->nfields; i++) {
        if (buf.n - pos < 3)
            return P_SHORT;
        f->tag[i] = buf.p[pos];
        size_t len = (size_t)buf.p[pos + 1] | ((size_t)buf.p[pos + 2] << 8);
        pos += 3;
        if (buf.n - pos < len)
            return P_SHORT;
        f->field[i].p = buf.p + pos;
        f->field[i].n = len;
        pos += len;
    }
    return pos == buf.n ? P_OK : P_TRAILING;
}

typedef struct {
    unsigned char *p;
    size_t n, cap;
} Out;

static void emit(Out *o, const void *src, size_t n) {
    check(o->n + n <= o->cap, "output capacity");
    memcpy(o->p + o->n, src, n);
    o->n += n;
}

static size_t build(unsigned char *dst, size_t cap, uint8_t type, const uint8_t *tags, const View *vals,
                    unsigned nf) {
    Out o = {dst, 4, cap};
    for (unsigned i = 0; i < nf; i++) {
        unsigned char hdr[3] = {tags[i], (unsigned char)(vals[i].n & 0xFF), (unsigned char)(vals[i].n >> 8)};
        emit(&o, hdr, 3);
        emit(&o, vals[i].p, vals[i].n);
    }
    dst[0] = type;
    dst[1] = (unsigned char)nf;
    dst[2] = (unsigned char)(o.n & 0xFF);
    dst[3] = (unsigned char)(o.n >> 8);
    return o.n;
}

/* Does the view lie inside the buffer? Proves nothing was copied. */
static int inside(View v, View whole) {
    return v.p >= whole.p && v.p + v.n <= whole.p + whole.n;
}

static uint32_t sum_view(View v) {
    uint32_t s = 0;
    for (size_t i = 0; i < v.n; i++)
        s = s * 33u + v.p[i];
    return s;
}

int main(void) {
    static const char *strs[] = {"host.example", "GET", "/index.html", "", "payload-bytes-here"};
    uint8_t tags[5] = {1, 2, 3, 4, 9};
    View vals[5];
    for (int i = 0; i < 5; i++) {
        vals[i].p = (const unsigned char *)strs[i];
        vals[i].n = strlen(strs[i]);
    }
    unsigned char wire[256];
    size_t n = build(wire, sizeof wire, 0x42, tags, vals, 5);
    printf("frame length: %zu\n", n);

    View whole = {wire, n};
    Frame f;
    Perr e = parse(whole, &f);
    printf("parse: %s type=0x%02x fields=%u\n", perr_name(e), f.type, f.nfields);
    check(e == P_OK, "parse ok");
    for (unsigned i = 0; i < f.nfields; i++) {
        check(inside(f.field[i], whole), "view inside buffer");
        printf("  tag %u len %2zu offset %3zu hash %10u\n", f.tag[i], f.field[i].n, (size_t)(f.field[i].p - wire),
               (unsigned)sum_view(f.field[i]));
    }

    /* the views alias the buffer: patching the buffer changes what the view shows */
    wire[f.field[1].p - wire] = 'P';
    check(f.field[1].p[0] == 'P', "view aliases buffer");
    printf("patched method: %.*s\n", (int)f.field[1].n, (const char *)f.field[1].p);

    /* reframe: build a new frame whose payload is a view of a field from the old one */
    View inner[2] = {f.field[0], f.field[4]};
    uint8_t itags[2] = {7, 8};
    unsigned char wire2[256];
    size_t n2 = build(wire2, sizeof wire2, 0x43, itags, inner, 2);
    Frame g;
    check(parse((View){wire2, n2}, &g) == P_OK, "reparse");
    check(g.field[0].n == f.field[0].n && memcmp(g.field[0].p, f.field[0].p, g.field[0].n) == 0, "inner copy");
    check(g.field[0].p != f.field[0].p, "second frame owns its bytes");
    printf("reframed: %zu bytes, %u fields\n", n2, g.nfields);

    /* every truncation and every length-field corruption is rejected without reading past the end */
    int rej = 0;
    for (size_t cut = 0; cut < n; cut++) {
        Frame t;
        Perr pe = parse((View){wire, cut}, &t);
        check(pe != P_OK, "truncation rejected");
        rej++;
    }
    printf("truncations rejected: %d\n", rej);
    int counts[5] = {0};
    for (size_t i = 0; i < n; i++)
        for (int bit = 0; bit < 8; bit += 3) {
            unsigned char c[256];
            memcpy(c, wire, n);
            c[i] ^= (unsigned char)(1u << bit);
            Frame t;
            Perr pe = parse((View){c, n}, &t);
            counts[pe]++;
            if (pe == P_OK)
                check(inside(t.field[0], (View){c, n}) || t.nfields == 0, "views bounded");
        }
    printf("bit-flip outcomes: ok=%d short=%d length=%d too-many=%d trailing=%d\n", counts[0], counts[1], counts[2],
           counts[3], counts[4]);

    /* trailing garbage */
    unsigned char extra[260];
    memcpy(extra, wire, n);
    extra[n] = 0;
    extra[2] = (unsigned char)((n + 1) & 0xFF);
    check(parse((View){extra, n + 1}, &g) == P_TRAILING, "trailing byte rejected");
    printf("trailing byte: rejected\n");
    return 0;
}
