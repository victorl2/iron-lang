/*
 * title: TSV with backslash escapes and column stats
 * topic: io_files
 * covers: tsv, escapes, numeric columns, header lookup, column statistics
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

/* TSV dialect: \t \n \r \\ escapes inside fields, header row, typed column stats. */
typedef struct {
    char name[16];
    char note[48];
    int qty;
    int cents;
} Item;

static void esc_field(Buf *b, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '\t': bstr(b, "\\t"); break;
        case '\n': bstr(b, "\\n"); break;
        case '\r': bstr(b, "\\r"); break;
        case '\\': bstr(b, "\\\\"); break;
        default: bbyte(b, (unsigned char)*s);
        }
    }
}

/* Unescape s[0..n) into dst (cap bytes). Returns length or -1 on bad escape. */
static int unesc_field(const char *s, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\') {
            if (++i >= n) return -1;
            switch (s[i]) {
            case 't': c = '\t'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case '\\': c = '\\'; break;
            default: return -1;
            }
        }
        if (o + 1 >= cap) return -1;
        dst[o++] = c;
    }
    dst[o] = 0;
    return (int)o;
}

static int split_tabs(const char *line, size_t n, const char **st, size_t *ln, int max) {
    int k = 0;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || line[i] == '\t') {
            if (k >= max) return -1;
            st[k] = line + start;
            ln[k] = i - start;
            k++;
            start = i + 1;
        }
    }
    return k;
}

int main(void) {
    static const char *names[] = {"widget", "gad\tget", "thing\\y", "bolt", "nut", "wash\ner"};
    static const char *notes[] = {"", "tab\there", "back\\slash", "two\nlines", "cr\rhere", "plain note"};
    Item items[20];
    int n = 20;
    for (int i = 0; i < n; i++) {
        uint32_t a = rnd(), b = rnd(), c = rnd(), d = rnd();
        snprintf(items[i].name, sizeof items[i].name, "%s", names[a % 6]);
        snprintf(items[i].note, sizeof items[i].note, "%s", notes[b % 6]);
        items[i].qty = (int)(c % 50);
        items[i].cents = (int)(d % 100000);
    }
    Buf b = {0};
    bstr(&b, "name\tnote\tqty\tcents\n");
    for (int i = 0; i < n; i++) {
        char num[32];
        esc_field(&b, items[i].name); bbyte(&b, '\t');
        esc_field(&b, items[i].note); bbyte(&b, '\t');
        snprintf(num, sizeof num, "%d\t%d\n", items[i].qty, items[i].cents);
        bstr(&b, num);
    }
    wfile("items.tsv", b.p, b.n);
    size_t len;
    unsigned char *in = rfile("items.tsv", &len);
    CHECK(len == b.n);
    int lines = 0, tabs_total = 0;
    for (size_t i = 0; i < len; i++) {
        if (in[i] == '\n') lines++;
        if (in[i] == '\t') tabs_total++;
    }
    /* escaped file has exactly 3 tabs and one newline per line */
    CHECK(lines == n + 1);
    CHECK(tabs_total == 3 * (n + 1));
    const char *text = (const char *)in;
    size_t pos = 0;
    int row = -1;
    int col_qty = -1, col_cents = -1;
    long qty_sum = 0, cents_sum = 0;
    int qty_max = -1;
    while (pos < len) {
        size_t e = pos;
        while (text[e] != '\n') e++;
        const char *st[8];
        size_t ln[8];
        int k = split_tabs(text + pos, e - pos, st, ln, 8);
        CHECK(k == 4);
        if (row < 0) {
            for (int c = 0; c < k; c++) {
                if (ln[c] == 3 && strncmp(st[c], "qty", 3) == 0) col_qty = c;
                if (ln[c] == 5 && strncmp(st[c], "cents", 5) == 0) col_cents = c;
            }
            CHECK(col_qty == 2 && col_cents == 3);
        } else {
            char nm[16], nt[48];
            CHECK(unesc_field(st[0], ln[0], nm, sizeof nm) >= 0);
            CHECK(unesc_field(st[1], ln[1], nt, sizeof nt) >= 0);
            CHECK(strcmp(nm, items[row].name) == 0);
            CHECK(strcmp(nt, items[row].note) == 0);
            char tmp[16];
            memcpy(tmp, st[col_qty], ln[col_qty]); tmp[ln[col_qty]] = 0;
            int q = atoi(tmp);
            memcpy(tmp, st[col_cents], ln[col_cents]); tmp[ln[col_cents]] = 0;
            int c = atoi(tmp);
            CHECK(q == items[row].qty && c == items[row].cents);
            qty_sum += q; cents_sum += c;
            if (q > qty_max) qty_max = q;
        }
        row++;
        pos = e + 1;
    }
    CHECK(row == n);
    printf("bytes=%zu rows=%d\n", len, row);
    printf("qty sum=%ld max=%d\n", qty_sum, qty_max);
    printf("total value cents=%ld avg=%ld.%02ld\n", cents_sum, cents_sum / n, (cents_sum * 100 / n) % 100);
    printf("first data line escaped: ");
    size_t p2 = 0;
    while (in[p2] != '\n') p2++;
    p2++;
    while (in[p2] != '\n') { putchar(in[p2] == '\t' ? '|' : in[p2]); p2++; }
    putchar('\n');
    char out[8];
    printf("bad escape: %d, dangling: %d, overflow: %d\n",
           unesc_field("a\\qb", 4, out, sizeof out), unesc_field("ab\\", 3, out, sizeof out),
           unesc_field("0123456789", 10, out, sizeof out));
    free(in);
    bfree(&b);
    remove("items.tsv");
    return 0;
}
