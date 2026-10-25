/*
 * title: CSV writer and RFC 4180 reader round trip
 * topic: io_files
 * covers: csv, quoting, embedded newlines, CRLF, state machine parser
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static inline void fail(const char *w) {
    fprintf(stderr, "check failed: %s\n", w);
    exit(1);
}
#define CHECK(c) do { if (!(c)) fail(#c); } while (0)

static inline void wfile(const char *name, const void *buf, size_t len) {
    FILE *f = fopen(name, "wb");
    if (!f) fail("open for write");
    if (len && fwrite(buf, 1, len, f) != len) fail("write");
    if (fclose(f) != 0) fail("close");
}

static inline unsigned char *rfile(const char *name, size_t *len) {
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

/* growable byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } Buf;
static inline void bput(Buf *b, const void *s, size_t k) {
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
static inline void bbyte(Buf *b, unsigned v) { unsigned char c = (unsigned char)v; bput(b, &c, 1); }
static inline void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static inline void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static uint32_t rng_s = 0x2545F491u;
static inline uint32_t rnd(void) {
    rng_s ^= rng_s << 13;
    rng_s ^= rng_s >> 17;
    rng_s ^= rng_s << 5;
    return rng_s;
}

/* Write random fields that need quoting, read them back with a state-machine parser. */
#define MAXF 8
typedef struct {
    char *f[MAXF];
    int n;
} Row;

static const char *POOL[] = {
    "plain", "with,comma", "say \"hi\"", "line1\nline2", "cr\r\nlf", "", " padded ",
    "\"\"", ",", "a,b\nc,d", "42", "trail\n", "\"quoted\",and\nnewline"
};
#define NPOOL ((int)(sizeof POOL / sizeof POOL[0]))

static int needs_quote(const char *s) {
    for (; *s; s++)
        if (*s == ',' || *s == '"' || *s == '\n' || *s == '\r') return 1;
    return 0;
}

static void write_field(Buf *b, const char *s, int force_empty_quote) {
    if (!needs_quote(s) && !(force_empty_quote && !*s)) {
        bstr(b, s);
        return;
    }
    bbyte(b, '"');
    for (; *s; s++) {
        if (*s == '"') bbyte(b, '"');
        bbyte(b, (unsigned char)*s);
    }
    bbyte(b, '"');
}

static void row_free(Row *r) {
    for (int i = 0; i < r->n; i++) free(r->f[i]);
    r->n = 0;
}

static char *dupn(const unsigned char *s, size_t n) {
    char *d = malloc(n + 1);
    if (!d) fail("oom");
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* Returns number of rows parsed; -1 on malformed input. */
static int parse_csv(const unsigned char *s, size_t n, Row *rows, int maxrows) {
    enum { FIELD_START, UNQUOTED, QUOTED, QUOTE_IN_QUOTED } st = FIELD_START;
    Buf cur = {0};
    int nr = 0;
    Row row = {{0}, 0};
    size_t i = 0;
    int have_row = 0;
    while (i < n) {
        unsigned char c = s[i];
        switch (st) {
        case FIELD_START:
            have_row = 1;
            if (c == '"') { st = QUOTED; i++; }
            else if (c == ',' ) { row.f[row.n++] = dupn(cur.p, cur.n); cur.n = 0; i++; }
            else if (c == '\r' && i + 1 < n && s[i + 1] == '\n') {
                row.f[row.n++] = dupn(cur.p, cur.n); cur.n = 0;
                rows[nr++] = row; row = (Row){{0}, 0}; have_row = 0; i += 2;
            } else { st = UNQUOTED; }
            break;
        case UNQUOTED:
            if (c == ',') { row.f[row.n++] = dupn(cur.p, cur.n); cur.n = 0; st = FIELD_START; i++; }
            else if (c == '\r' && i + 1 < n && s[i + 1] == '\n') {
                row.f[row.n++] = dupn(cur.p, cur.n); cur.n = 0;
                rows[nr++] = row; row = (Row){{0}, 0}; have_row = 0; st = FIELD_START; i += 2;
            } else if (c == '"') { row_free(&row); bfree(&cur); return -1; }
            else { bbyte(&cur, c); i++; }
            break;
        case QUOTED:
            if (c == '"') st = QUOTE_IN_QUOTED; else bbyte(&cur, c);
            i++;
            break;
        case QUOTE_IN_QUOTED:
            if (c == '"') { bbyte(&cur, '"'); st = QUOTED; i++; }
            else if (c == ',') { row.f[row.n++] = dupn(cur.p, cur.n); cur.n = 0; st = FIELD_START; i++; }
            else if (c == '\r' && i + 1 < n && s[i + 1] == '\n') {
                row.f[row.n++] = dupn(cur.p, cur.n); cur.n = 0;
                rows[nr++] = row; row = (Row){{0}, 0}; have_row = 0; st = FIELD_START; i += 2;
            } else { row_free(&row); bfree(&cur); return -1; }
            break;
        }
        if (nr >= maxrows) break;
    }
    if (st == QUOTED) { row_free(&row); bfree(&cur); return -1; }
    if (have_row && nr < maxrows) {
        row.f[row.n++] = dupn(cur.p, cur.n);
        rows[nr++] = row;
    } else row_free(&row);
    bfree(&cur);
    return nr;
}

static void show(const char *s) {
    putchar('[');
    for (; *s; s++) {
        if (*s == '\n') fputs("\\n", stdout);
        else if (*s == '\r') fputs("\\r", stdout);
        else putchar(*s);
    }
    putchar(']');
}

int main(void) {
    enum { R = 12 };
    static Row src[R], got[R + 2];
    Buf out = {0};
    for (int r = 0; r < R; r++) {
        src[r].n = 1 + (int)(rnd() % 5);
        for (int c = 0; c < src[r].n; c++) {
            uint32_t k = rnd() % (uint32_t)NPOOL;
            src[r].f[c] = dupn((const unsigned char *)POOL[k], strlen(POOL[k]));
        }
        /* a lone empty field must be quoted or the row vanishes */
        for (int c = 0; c < src[r].n; c++) {
            write_field(&out, src[r].f[c], src[r].n == 1);
            if (c + 1 < src[r].n) bbyte(&out, ',');
        }
        bstr(&out, "\r\n");
    }
    wfile("data.csv", out.p, out.n);
    size_t len;
    unsigned char *in = rfile("data.csv", &len);
    CHECK(len == out.n);
    int nr = parse_csv(in, len, got, R + 2);
    CHECK(nr == R);
    int quoted_fields = 0, total = 0;
    for (int r = 0; r < R; r++) {
        CHECK(got[r].n == src[r].n);
        for (int c = 0; c < src[r].n; c++) {
            CHECK(strcmp(got[r].f[c], src[r].f[c]) == 0);
            total++;
            if (needs_quote(src[r].f[c])) quoted_fields++;
        }
    }
    printf("file bytes: %zu, rows: %d, fields: %d, quoted: %d\n", len, nr, total, quoted_fields);
    for (int r = 0; r < 4; r++) {
        printf("row %d (%d):", r, got[r].n);
        for (int c = 0; c < got[r].n; c++) { putchar(' '); show(got[r].f[c]); }
        putchar('\n');
    }
    /* malformed inputs */
    Row bad[4];
    const char *m1 = "a,\"unterminated\r\nb";
    const char *m2 = "ab\"cd,e\r\n";
    const char *m3 = "\"ok\"x,1\r\n";
    printf("unterminated: %d\n", parse_csv((const unsigned char *)m1, strlen(m1), bad, 4));
    printf("stray quote: %d\n", parse_csv((const unsigned char *)m2, strlen(m2), bad, 4));
    printf("junk after quote: %d\n", parse_csv((const unsigned char *)m3, strlen(m3), bad, 4));
    /* no trailing CRLF on last record */
    const char *m4 = "x,y\r\n\"p\r\nq\",z";
    int n4 = parse_csv((const unsigned char *)m4, strlen(m4), bad, 4);
    printf("no final crlf: rows=%d second=", n4);
    show(bad[1].f[0]);
    putchar('\n');
    for (int i = 0; i < n4; i++) row_free(&bad[i]);
    for (int r = 0; r < R; r++) { row_free(&src[r]); row_free(&got[r]); }
    free(in);
    bfree(&out);
    remove("data.csv");
    return 0;
}
