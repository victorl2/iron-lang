/*
 * title: vCard content lines with 75-octet folding, UTF-8 safe splits and value escaping
 * topic: io_files
 * covers: rfc 6350 style content lines, folding at 75 octets, continuation lines, utf-8 boundary aware splitting, backslash escapes, parameters, case-insensitive names, unfolding
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

void fail(const char *w) {
    fprintf(stderr, "check failed: %s\n", w);
    exit(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

void wfile(const char *name, const void *buf, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open for write");
    if (len && fwrite(buf, 1, len, f) != len) fail("write");
    if (fclose(f) != 0) fail("close");
}

unsigned char *rfile(const char *name, size_t *len) {
    FILE *f = fopen(name, "rb");
    if (!f) fail("open for read");
    size_t cap = 256, n = 0;
    unsigned char *b = malloc(cap);
    if (!b) fail("oom");
    for (;;) {
        if (n == cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) fail("oom");
        }
        size_t r = fread(b + n, 1, cap - n, f);
        if (r == 0) break;
        n += r;
    }
    fclose(f);
    *len = n;
    return b;
}


static uint32_t rng_s = 0x2545F491u;
uint32_t rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}
/* growable byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } Buf;
void bput(Buf *b, const void *s, size_t k) {
    if (b->n + k > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->n + k) nc *= 2;
        b->p = realloc(b->p, nc);
        if (!b->p) fail("oom");
        b->cap = nc;
    }
    if (k) memcpy(b->p + b->n, s, k);
    b->n += k;
}
void bbyte(Buf *b, unsigned v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

typedef struct { char name[24]; char params[40]; char *value; } Line;
typedef struct { Line l[24]; int n; } Card;

static void esc_value(Buf *o, const char *v) {
    for (; *v; v++) {
        if (*v == '\\' || *v == ';' || *v == ',') { bbyte(o, '\\'); bbyte(o, (unsigned char)*v); }
        else if (*v == '\n') bstr(o, "\\n");
        else bbyte(o, (unsigned char)*v);
    }
}

static void unesc_value(Buf *o, const char *v) {
    for (; *v; v++) {
        if (*v == '\\' && v[1]) {
            v++;
            bbyte(o, *v == 'n' || *v == 'N' ? '\n' : (unsigned char)*v);
        } else bbyte(o, (unsigned char)*v);
    }
    bbyte(o, 0);
}

/* fold one logical line: at most 75 octets per physical line (continuations start with a space and count it),
 * never splitting inside a UTF-8 sequence. Returns number of physical lines. */
static int fold(Buf *o, const unsigned char *s, size_t n) {
    size_t pos = 0;
    int lines = 0;
    int first = 1;
    while (pos < n || first) {
        size_t limit = first ? 75 : 74;
        size_t take = n - pos < limit ? n - pos : limit;
        if (pos + take < n) {
            while (take > 0 && (s[pos + take] & 0xC0) == 0x80) take--;   /* back up to a character start */
            CHECK(take > 0);
        }
        if (!first) bbyte(o, ' ');
        bput(o, s + pos, take);
        bstr(o, "\r\n");
        pos += take;
        first = 0;
        lines++;
        if (pos >= n) break;
    }
    return lines;
}

static void add_prop(Buf *o, const char *name, const char *params, const char *value, int *phys) {
    Buf ln = {0};
    bstr(&ln, name);
    if (params && *params) { bbyte(&ln, ';'); bstr(&ln, params); }
    bbyte(&ln, ':');
    esc_value(&ln, value);
    *phys += fold(o, ln.p, ln.n);
    bfree(&ln);
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if ((*a | 32) != (*b | 32)) return 0;
    return !*a && !*b;
}

static int parse_card(const char *text, size_t n, Card *c, int *longest) {
    /* unfold: CRLF followed by space or tab is removed */
    Buf un = {0};
    for (size_t i = 0; i < n; i++) {
        if (text[i] == '\r' && i + 2 < n && text[i + 1] == '\n' && (text[i + 2] == ' ' || text[i + 2] == '\t')) { i += 2; continue; }
        bbyte(&un, (unsigned char)text[i]);
    }
    bbyte(&un, 0);
    c->n = 0;
    *longest = 0;
    char *p = (char *)un.p;
    int ok = 1;
    while (*p && ok) {
        char *e = strstr(p, "\r\n");
        if (!e) { ok = 0; break; }
        *e = 0;
        if ((int)strlen(p) > *longest) *longest = (int)strlen(p);
        char *colon = strchr(p, ':');
        if (!colon || c->n >= 24) { ok = 0; break; }
        *colon = 0;
        char *semi = strchr(p, ';');
        Line *l = &c->l[c->n];
        memset(l, 0, sizeof *l);
        if (semi) { *semi = 0; snprintf(l->params, sizeof l->params, "%s", semi + 1); }
        snprintf(l->name, sizeof l->name, "%s", p);
        Buf v = {0};
        unesc_value(&v, colon + 1);
        l->value = (char *)v.p;
        c->n++;
        p = e + 2;
    }
    bfree(&un);
    return ok;
}

static const Line *find(const Card *c, const char *name) {
    for (int i = 0; i < c->n; i++) if (ieq(c->l[i].name, name)) return &c->l[i];
    return NULL;
}

int main(void) {
    static const char *notes[] = {
        "Short note.",
        "Line one\nLine two; with semicolon, comma, and back\\slash",
        "A long note without any special characters that just keeps going and going, well past the seventy-five octet limit for a single content line.",
        "Ünïcödé façade: 東京都渋谷区 — the quick brown fox jumps over the lazy dog 🦊 and keeps talking about multibyte text near fold points",
    };
    Buf card = {0};
    int phys = 0;
    bstr(&card, "BEGIN:VCARD\r\n"); phys++;
    bstr(&card, "VERSION:4.0\r\n"); phys++;
    add_prop(&card, "FN", NULL, "Ada Lovelace", &phys);
    add_prop(&card, "N", NULL, "Lovelace;Ada;Augusta;Countess;", &phys);   /* structured: escaped by esc_value on purpose */
    add_prop(&card, "EMAIL", "TYPE=work", "ada@example.org", &phys);
    for (int i = 0; i < 4; i++) add_prop(&card, "NOTE", i == 3 ? "LANGUAGE=mul" : NULL, notes[i], &phys);
    add_prop(&card, "ADR", "TYPE=home", "12 St James's Square, London", &phys);
    bstr(&card, "END:VCARD\r\n"); phys++;
    wfile("ada.vcf", card.p, card.n);
    size_t n;
    unsigned char *raw = rfile("ada.vcf", &n);
    /* physical line length audit */
    int maxlen = 0, physical = 0, bad_split = 0;
    for (size_t i = 0, st = 0; i + 1 < n; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n') {
            int l = (int)(i - st);
            if (l > maxlen) maxlen = l;
            physical++;
            if (st > 0 && raw[st] != ' ' && (raw[st] & 0xC0) == 0x80) bad_split++;
            if (st > 0 && raw[st] == ' ' && (raw[st + 1] & 0xC0) == 0x80) bad_split++;
            st = i + 2;
        }
    }
    CHECK(maxlen <= 75 && physical == phys && bad_split == 0);
    printf("file %zu bytes, %d physical lines, longest %d octets, utf-8 splits broken: %d\n", n, physical, maxlen, bad_split);
    /* show the folded text with visible line ends */
    for (size_t i = 0; i < n; i++) {
        if (raw[i] == '\r') continue;
        if (raw[i] == '\n') { fputs("$\n", stdout); continue; }
        putchar(raw[i]);
    }
    Card c;
    int longest;
    CHECK(parse_card((const char *)raw, n, &c, &longest));
    printf("parsed %d properties, longest logical line %d octets\n", c.n, longest);
    CHECK(!strcmp(find(&c, "fn")->value, "Ada Lovelace"));
    CHECK(!strcmp(find(&c, "n")->value, "Lovelace;Ada;Augusta;Countess;"));
    int ni = 0;
    for (int i = 0; i < c.n; i++) {
        if (!ieq(c.l[i].name, "NOTE")) continue;
        CHECK(!strcmp(c.l[i].value, notes[ni]));
        printf("note %d: %zu octets restored%s%s\n", ni, strlen(c.l[i].value), c.l[i].params[0] ? ", params " : "", c.l[i].params);
        ni++;
    }
    CHECK(ni == 4);
    /* random property values through fold/unfold: exhaustive fold-boundary shift test */
    int lines_total = 0;
    for (int shift = 0; shift < 8; shift++) {
        for (int t = 0; t < 20; t++) {
            Buf v = {0};
            for (int i = 0; i < shift; i++) bbyte(&v, 'x');
            int chars = 20 + (int)(rnd() % 60);
            for (int i = 0; i < chars; i++) {
                uint32_t r = rnd() % 4;
                if (r == 0) { bbyte(&v, 0xC3); bbyte(&v, 0xA9); }                          /* é, 2 octets */
                else if (r == 1) { bbyte(&v, 0xE6); bbyte(&v, 0x9D); bbyte(&v, 0xB1); }   /* 東, 3 octets */
                else if (r == 2) { bbyte(&v, 0xF0); bbyte(&v, 0x9F); bbyte(&v, 0xA6); bbyte(&v, 0x8A); } /* 4 octets */
                else bbyte(&v, (unsigned char)('a' + rnd() % 26));
            }
            bbyte(&v, 0);
            Buf o = {0}, ln = {0};
            bstr(&ln, "X-DATA:");
            esc_value(&ln, (char *)v.p);
            lines_total += fold(&o, ln.p, ln.n);
            /* every physical line valid UTF-8 boundary and <= 75 */
            size_t st = 0;
            for (size_t i = 0; i + 1 < o.n; i++)
                if (o.p[i] == '\r' && o.p[i + 1] == '\n') {
                    CHECK(i - st <= 75);
                    if (st > 0) CHECK(o.p[st] == ' ' && (o.p[st + 1] & 0xC0) != 0x80);
                    st = i + 2;
                }
            Buf all = {0};
            bstr(&all, "BEGIN:VCARD\r\n");
            bput(&all, o.p, o.n);
            bstr(&all, "END:VCARD\r\n");
            Card cc;
            int lg;
            CHECK(parse_card((char *)all.p, all.n, &cc, &lg));
            CHECK(!strcmp(find(&cc, "x-data")->value, (char *)v.p));
            for (int i = 0; i < cc.n; i++) free(cc.l[i].value);
            bfree(&v); bfree(&o); bfree(&ln); bfree(&all);
        }
    }
    printf("160 fold/unfold round trips ok, %d physical lines\n", lines_total);
    for (int i = 0; i < c.n; i++) free(c.l[i].value);
    free(raw); bfree(&card);
    remove("ada.vcf");
    return 0;
}
