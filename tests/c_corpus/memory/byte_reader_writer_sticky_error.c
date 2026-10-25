/*
 * title: Bounds-checked byte reader and writer with sticky error state
 * topic: memory
 * covers: cursor API, sticky error, endianness helpers, length-prefixed strings, truncated input, round trip
 * deps: libc
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *p; size_t len, pos; int err; } Reader;
typedef struct { uint8_t *p; size_t cap, pos; int err; } Writer;

static void rd_init(Reader *r, const void *p, size_t n) { r->p = p; r->len = n; r->pos = 0; r->err = 0; }
static int rd_need(Reader *r, size_t n) {
    if (r->err) return 0;
    if (r->len - r->pos < n) { r->err = 1; return 0; }
    return 1;
}
static uint8_t rd_u8(Reader *r) { if (!rd_need(r, 1)) return 0; return r->p[r->pos++]; }
static uint16_t rd_u16(Reader *r) { if (!rd_need(r, 2)) return 0; uint16_t v = (uint16_t)(r->p[r->pos] << 8 | r->p[r->pos + 1]); r->pos += 2; return v; }
static uint32_t rd_u32(Reader *r) {
    if (!rd_need(r, 4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = v << 8 | r->p[r->pos + (size_t)i];
    r->pos += 4;
    return v;
}
/* copies at most cap-1 bytes and terminates; strings longer than the buffer are an error, not a truncation */
static size_t rd_str(Reader *r, char *out, size_t cap) {
    size_t n = rd_u8(r);
    if (r->err) return 0;
    if (n >= cap || !rd_need(r, n)) { r->err = 1; return 0; }
    memcpy(out, r->p + r->pos, n);
    out[n] = '\0';
    r->pos += n;
    return n;
}

static void wr_init(Writer *w, void *p, size_t cap) { w->p = p; w->cap = cap; w->pos = 0; w->err = 0; }
static int wr_room(Writer *w, size_t n) { if (w->err) return 0; if (w->cap - w->pos < n) { w->err = 1; return 0; } return 1; }
static void wr_u8(Writer *w, uint8_t v) { if (wr_room(w, 1)) w->p[w->pos++] = v; }
static void wr_u16(Writer *w, uint16_t v) { if (wr_room(w, 2)) { w->p[w->pos++] = (uint8_t)(v >> 8); w->p[w->pos++] = (uint8_t)v; } }
static void wr_u32(Writer *w, uint32_t v) { if (wr_room(w, 4)) for (int i = 3; i >= 0; i--) w->p[w->pos++] = (uint8_t)(v >> (8 * i)); }
static void wr_str(Writer *w, const char *s) {
    size_t n = strlen(s);
    if (n > 255) { w->err = 1; return; }
    wr_u8(w, (uint8_t)n);
    if (wr_room(w, n)) { memcpy(w->p + w->pos, s, n); w->pos += n; }
}

typedef struct { uint32_t id; uint16_t flags; char name[16]; uint8_t nscores; uint16_t scores[6]; } Rec;

static void put_rec(Writer *w, const Rec *r) {
    wr_u32(w, r->id); wr_u16(w, r->flags); wr_str(w, r->name); wr_u8(w, r->nscores);
    for (int i = 0; i < r->nscores; i++) wr_u16(w, r->scores[i]);
}
static int get_rec(Reader *r, Rec *o) {
    memset(o, 0, sizeof *o);
    o->id = rd_u32(r); o->flags = rd_u16(r);
    rd_str(r, o->name, 12); /* the reader's policy limit is 11 characters */
    o->nscores = rd_u8(r);
    if (o->nscores > 6) { r->err = 1; return 0; }
    for (int i = 0; i < o->nscores; i++) o->scores[i] = rd_u16(r);
    return !r->err;
}

int main(void) {
    Rec in[3] = {
        {0xDEADBEEF, 0x0102, "alice", 3, {10, 20, 30}},
        {7, 0, "bob", 0, {0}},
        {0x01020304, 0xFFFF, "carolinejones", 6, {1, 2, 3, 4, 5, 65535}}, /* 13 chars: over the reader's limit */
    };
    uint8_t buf[128];
    Writer w;
    wr_init(&w, buf, sizeof buf);
    for (int i = 0; i < 2; i++) put_rec(&w, &in[i]);
    printf("wrote %zu bytes, err=%d\n", w.pos, w.err);
    size_t good = w.pos;

    Reader r;
    rd_init(&r, buf, good);
    Rec out;
    for (int i = 0; i < 2; i++) {
        int ok = get_rec(&r, &out);
        printf("rec %d ok=%d id=%08x flags=%04x name=%s n=%d last=%d\n", i, ok, (unsigned)out.id, out.flags, out.name, out.nscores, out.nscores ? out.scores[out.nscores - 1] : -1);
        if (!ok || out.id != in[i].id || strcmp(out.name, in[i].name) != 0) return 1;
    }
    printf("consumed exactly: %d\n", r.pos == good && !r.err);

    /* the long name is written fine (13 < 256) but refused by the reader's small field */
    wr_init(&w, buf, sizeof buf);
    put_rec(&w, &in[2]);
    rd_init(&r, buf, w.pos);
    int ok = get_rec(&r, &out);
    printf("oversized name: written=%zu read ok=%d err=%d\n", w.pos, ok, r.err);

    /* truncation sweep: every prefix of a valid 2-record stream either fully parses or errors, never overreads */
    wr_init(&w, buf, sizeof buf);
    for (int i = 0; i < 2; i++) put_rec(&w, &in[i]);
    size_t total = w.pos;
    int parsed_full = 0, errs = 0;
    for (size_t n = 0; n <= total; n++) {
        uint8_t *exact = malloc(n ? n : 1);
        if (!exact) return 1;
        memcpy(exact, buf, n);
        rd_init(&r, exact, n);
        int k = 0;
        while (r.pos < n && get_rec(&r, &out)) k++;
        if (!r.err && k == 2 && r.pos == n) parsed_full++; else errs += r.err;
        free(exact);
    }
    printf("prefix sweep over %zu lengths: full parse=%d, errors=%d\n", total + 1, parsed_full, errs);

    /* writer refuses to overflow */
    uint8_t tiny[6];
    wr_init(&w, tiny, sizeof tiny);
    wr_u32(&w, 1); wr_u16(&w, 2); wr_u8(&w, 3);
    printf("tiny writer: pos=%zu err=%d\n", w.pos, w.err);
    return 0;
}
