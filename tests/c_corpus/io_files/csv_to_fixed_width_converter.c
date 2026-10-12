/*
 * title: CSV to fixed-width converter with alignment and truncation
 * topic: io_files
 * covers: csv parsing, column width inference, left/right alignment, numeric detection, truncation with ellipsis, fixed-width reader, round trip
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

enum { MAXR = 24, MAXC = 5, CELL = 40 };
typedef struct { char c[MAXC][CELL]; } Row;

/* minimal RFC 4180-ish splitter: quoted fields with "" escapes, no embedded newlines here */
static int split_csv(const char *line, char out[MAXC][CELL]) {
    int n = 0;
    const char *p = line;
    for (;;) {
        size_t o = 0;
        if (*p == '"') {
            p++;
            for (;;) {
                if (!*p) return -1;
                if (*p == '"') { if (p[1] == '"') { if (o + 1 < CELL) out[n][o++] = '"'; p += 2; continue; } p++; break; }
                if (o + 1 < CELL) out[n][o++] = *p;
                p++;
            }
        } else {
            while (*p && *p != ',') { if (o + 1 < CELL) out[n][o++] = *p; p++; }
        }
        out[n][o] = 0;
        n++;
        if (n > MAXC) return -1;
        if (*p == ',') { p++; continue; }
        if (*p == 0) break;
        return -1;
    }
    return n;
}

static int is_numeric(const char *s) {
    if (*s == '-' || *s == '+') s++;
    int digits = 0, dot = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') digits++;
        else if (*s == '.' && !dot) dot = 1;
        else return 0;
    }
    return digits > 0;
}

/* Sort key for stable ordering by a numeric column, done with insertion sort so ties keep order. */
static void sort_rows(Row *r, int n, int col) {
    for (int i = 1; i < n; i++) {
        Row x = r[i];
        int j = i - 1;
        while (j >= 0 && atof(r[j].c[col]) < atof(x.c[col])) { r[j + 1] = r[j]; j--; }
        r[j + 1] = x;
    }
}

int main(void) {
    static const char *first[] = {"Ada", "Grace", "Linus", "Dennis", "Barbara", "Ken", "Margaret"};
    static const char *notes[] = {"", "likes \"quotes\"", "comma, inside", "a very long free-form note that must be cut", "ok"};
    Buf csv = {0};
    bstr(&csv, "name,dept,salary,ratio,note\n");
    int nrows = 14;
    for (int i = 0; i < nrows; i++) {
        char line[200];
        uint32_t a = rnd(), b = rnd(), c = rnd(), d = rnd();
        const char *note = notes[d % 5];
        char q[100];
        size_t o = 0;
        q[o++] = '"';
        for (const char *s = note; *s; s++) { if (*s == '"') q[o++] = '"'; q[o++] = *s; }
        q[o++] = '"';
        q[o] = 0;
        snprintf(line, sizeof line, "%s %c.,D%u,%u,%d.%02u,%s\n", first[a % 7], (char)('A' + (int)(b % 26)), c % 4, 30000 + (unsigned)(c % 70000),
                 (int)(b % 5) - 1, (unsigned)(d % 100), q);
        bstr(&csv, line);
    }
    wfile("in.csv", csv.p, csv.n);
    size_t n;
    unsigned char *raw = rfile("in.csv", &n);
    char *text = malloc(n + 1);
    memcpy(text, raw, n);
    text[n] = 0;
    static Row rows[MAXR];
    int nr = 0, ncols = 0;
    char hdr[MAXC][CELL];
    char *save = text;
    int first_line = 1;
    while (*save) {
        char *e = strchr(save, '\n');
        *e = 0;
        if (first_line) { ncols = split_csv(save, hdr); first_line = 0; }
        else { int k = split_csv(save, rows[nr].c); CHECK(k == ncols); nr++; }
        save = e + 1;
    }
    CHECK(nr == nrows && ncols == 5);

    /* infer widths and alignments */
    int width[MAXC], numeric[MAXC];
    static const int maxw[MAXC] = {14, 6, 8, 7, 18};
    for (int c = 0; c < ncols; c++) {
        width[c] = (int)strlen(hdr[c]);
        numeric[c] = 1;
        for (int r = 0; r < nr; r++) {
            int l = (int)strlen(rows[r].c[c]);
            if (l > width[c]) width[c] = l;
            if (!is_numeric(rows[r].c[c])) numeric[c] = 0;
        }
        if (width[c] > maxw[c]) width[c] = maxw[c];
    }
    sort_rows(rows, nr, 2);
    Buf out = {0};
    for (int r = -1; r < nr; r++) {
        for (int c = 0; c < ncols; c++) {
            const char *s = r < 0 ? hdr[c] : rows[r].c[c];
            char cell[64];
            int l = (int)strlen(s);
            if (l > width[c]) { snprintf(cell, sizeof cell, "%.*s~", width[c] - 1, s); l = width[c]; }
            else snprintf(cell, sizeof cell, "%s", s);
            int padn = width[c] - l;
            if (numeric[c] && r >= 0) { for (int k = 0; k < padn; k++) bbyte(&out, ' '); bstr(&out, cell); }
            else { bstr(&out, cell); for (int k = 0; k < padn; k++) bbyte(&out, ' '); }
            bbyte(&out, c + 1 < ncols ? '|' : '\n');
        }
    }
    wfile("out.fw", out.p, out.n);
    size_t fn;
    unsigned char *fw = rfile("out.fw", &fn);
    CHECK(fn == out.n);
    /* reader: slice by widths, trim, and compare with the source (respecting truncation) */
    int line_len = 0;
    for (int c = 0; c < ncols; c++) line_len += width[c] + 1;
    CHECK(fn == (size_t)line_len * (size_t)(nr + 1));
    int truncated = 0, exact = 0;
    for (int r = 0; r < nr; r++) {
        const unsigned char *ln = fw + (size_t)(r + 1) * (size_t)line_len;
        int off = 0;
        for (int c = 0; c < ncols; c++) {
            char cell[64];
            memcpy(cell, ln + off, (size_t)width[c]);
            cell[width[c]] = 0;
            char *s = cell;
            while (*s == ' ') s++;
            size_t l = strlen(s);
            while (l && s[l - 1] == ' ') s[--l] = 0;
            const char *orig = rows[r].c[c];
            if (l && s[l - 1] == '~' && strlen(orig) > (size_t)width[c]) {
                CHECK(strncmp(s, orig, l - 1) == 0);
                truncated++;
            } else {
                CHECK(strcmp(s, orig) == 0 || (strlen(orig) && orig[strlen(orig) - 1] == ' '));
                exact++;
            }
            off += width[c] + 1;
            CHECK(ln[off - 1] == (c + 1 < ncols ? '|' : '\n'));
        }
    }
    printf("widths:");
    for (int c = 0; c < ncols; c++) printf(" %s=%d%s", hdr[c], width[c], numeric[c] ? "(num)" : "");
    printf("\nfixed-width file: %zu bytes, %d bytes per line\n", fn, line_len);
    fwrite(fw, 1, (size_t)line_len * 8, stdout);
    printf("cells exact=%d truncated=%d\n", exact, truncated);
    free(fw); free(raw); free(text); bfree(&csv); bfree(&out);
    remove("in.csv"); remove("out.fw");
    return 0;
}
