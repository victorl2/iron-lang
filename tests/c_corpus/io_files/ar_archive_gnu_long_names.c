/*
 * title: ar archive with GNU long-name table and symbol-free members
 * topic: io_files
 * covers: ar format, !<arch> magic, 60-byte headers, even alignment padding, // long names table, /offset references, update and delete
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

typedef struct { char name[80]; size_t len; unsigned char data[300]; } Member;

static void field(Buf *b, const char *s, size_t width) {
    size_t l = strlen(s);
    CHECK(l <= width);
    bstr(b, s);
    for (; l < width; l++) bbyte(b, ' ');
}

static void num_field(Buf *b, unsigned long v, size_t width) {
    char t[32];
    snprintf(t, sizeof t, "%lu", v);
    field(b, t, width);
}

static void write_ar(Buf *out, const Member *m, int n) {
    /* names >= 16 chars go to the "//" table, referenced as /offset */
    Buf tbl = {0};
    size_t off[16];
    for (int i = 0; i < n; i++) {
        off[i] = tbl.n;
        if (strlen(m[i].name) >= 16) { bstr(&tbl, m[i].name); bstr(&tbl, "/\n"); }
    }
    bstr(out, "!<arch>\n");
    if (tbl.n) {
        field(out, "//", 16); field(out, "0", 12); field(out, "0", 6); field(out, "0", 6); field(out, "0", 8);
        num_field(out, tbl.n, 10);
        bstr(out, "`\n");
        bput(out, tbl.p, tbl.n);
        if (tbl.n & 1) bbyte(out, '\n');
    }
    for (int i = 0; i < n; i++) {
        char nm[32];
        if (strlen(m[i].name) >= 16) snprintf(nm, sizeof nm, "/%zu", off[i]);
        else snprintf(nm, sizeof nm, "%s/", m[i].name);
        field(out, nm, 16);
        field(out, "1700000000", 12); field(out, "0", 6); field(out, "0", 6); field(out, "100644", 8);
        num_field(out, m[i].len, 10);
        bstr(out, "`\n");
        bput(out, m[i].data, m[i].len);
        if (m[i].len & 1) bbyte(out, '\n');
    }
    bfree(&tbl);
}

static long parse_dec(const unsigned char *p, size_t w) {
    long v = 0;
    size_t i = 0;
    while (i < w && p[i] == ' ') i++;
    if (i == w) return -1;
    for (; i < w && p[i] != ' '; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

static int read_ar(const unsigned char *d, size_t n, Member *m, int max, const char **err) {
    if (n < 8 || memcmp(d, "!<arch>\n", 8)) { *err = "bad magic"; return -1; }
    size_t pos = 8;
    const unsigned char *tbl = NULL;
    size_t tbl_len = 0;
    int cnt = 0;
    while (pos < n) {
        if (pos + 60 > n) { *err = "truncated member header"; return -1; }
        const unsigned char *h = d + pos;
        if (h[58] != '`' || h[59] != '\n') { *err = "bad header terminator"; return -1; }
        long sz = parse_dec(h + 48, 10);
        if (sz < 0) { *err = "bad size field"; return -1; }
        pos += 60;
        if (pos + (size_t)sz > n) { *err = "member overruns archive"; return -1; }
        if (h[0] == '/' && h[1] == '/' && h[2] == ' ') {
            tbl = d + pos; tbl_len = (size_t)sz;
        } else {
            if (cnt >= max) { *err = "too many members"; return -1; }
            Member *o = &m[cnt];
            if (h[0] == '/' && h[1] >= '0' && h[1] <= '9') {
                long off = parse_dec(h + 1, 15);
                if (!tbl || off < 0 || (size_t)off >= tbl_len) { *err = "bad long name reference"; return -1; }
                size_t e = (size_t)off;
                while (e < tbl_len && tbl[e] != '/') e++;
                snprintf(o->name, sizeof o->name, "%.*s", (int)(e - (size_t)off), tbl + off);
            } else {
                size_t e = 0;
                while (e < 16 && h[e] != '/') e++;
                if (e == 16) { *err = "unterminated short name"; return -1; }
                snprintf(o->name, sizeof o->name, "%.*s", (int)e, h);
            }
            if ((size_t)sz > sizeof o->data) { *err = "member too big"; return -1; }
            o->len = (size_t)sz;
            memcpy(o->data, d + pos, (size_t)sz);
            cnt++;
        }
        pos += (size_t)sz + ((size_t)sz & 1);
    }
    return cnt;
}

int main(void) {
    static Member m[8], got[8];
    static const char *names[] = {"a.o", "libcore_utilities_impl.o", "x", "another_quite_long_name.c", "mid_size_name.h", "z.txt"};
    static const size_t lens[] = {17, 100, 0, 255, 1, 300};
    int n = 6;
    for (int i = 0; i < n; i++) {
        snprintf(m[i].name, sizeof m[i].name, "%s", names[i]);
        m[i].len = lens[i];
        for (size_t k = 0; k < lens[i]; k++) m[i].data[k] = (unsigned char)(rnd() >> 10);
    }
    Buf a = {0};
    write_ar(&a, m, n);
    wfile("lib.a", a.p, a.n);
    size_t sz;
    unsigned char *d = rfile("lib.a", &sz);
    const char *err = "";
    int c = read_ar(d, sz, got, 8, &err);
    CHECK(c == n);
    printf("archive: %zu bytes, %d members\n", sz, c);
    for (int i = 0; i < c; i++) {
        CHECK(!strcmp(got[i].name, m[i].name) && got[i].len == m[i].len && !memcmp(got[i].data, m[i].data, m[i].len));
        printf("  %-28s %3zu bytes%s\n", got[i].name, got[i].len, strlen(got[i].name) >= 16 ? "  (long name)" : "");
    }
    /* delete member 2 and 3, replace member 0 with new contents, append one, re-serialize */
    Member m2[8];
    int k = 0;
    for (int i = 0; i < c; i++) if (i != 2 && i != 3) m2[k++] = got[i];
    m2[0].len = 5;
    memcpy(m2[0].data, "NEW!!", 5);
    snprintf(m2[k].name, sizeof m2[k].name, "%s", "appended_member_with_long_name.bin");
    m2[k].len = 9;
    memcpy(m2[k].data, "123456789", 9);
    k++;
    Buf b = {0};
    write_ar(&b, m2, k);
    wfile("lib.a", b.p, b.n);
    unsigned char *d2 = rfile("lib.a", &sz);
    Member got2[8];
    int c2 = read_ar(d2, sz, got2, 8, &err);
    CHECK(c2 == k);
    printf("after edit: %zu bytes, members:", sz);
    for (int i = 0; i < c2; i++) printf(" %s(%zu)", got2[i].name, got2[i].len);
    putchar('\n');
    CHECK(!memcmp(got2[0].data, "NEW!!", 5));
    /* damage tests */
    unsigned char *bad = malloc(sz);
    memcpy(bad, d2, sz);
    bad[8 + 58] = 'X';
    CHECK(read_ar(bad, sz, got2, 8, &err) < 0);
    printf("bad terminator: %s\n", err);
    memcpy(bad, d2, sz);
    bad[3] = '?';
    CHECK(read_ar(bad, sz, got2, 8, &err) < 0);
    printf("bad magic: %s\n", err);
    CHECK(read_ar(d2, sz - 3, got2, 8, &err) < 0);
    printf("truncated: %s\n", err);
    /* long-name reference beyond the table */
    Member one[1];
    snprintf(one[0].name, sizeof one[0].name, "%s", "a_name_that_is_long_enough.o");
    one[0].len = 2;
    memcpy(one[0].data, "hi", 2);
    Buf c1 = {0};
    write_ar(&c1, one, 1);
    unsigned char *e = c1.p;
    size_t mh = 8 + 60 + 30;   /* magic + table header + table (29 bytes + pad) */
    CHECK(e[mh] == '/' && e[mh + 1] == '0');
    e[mh + 1] = '5'; e[mh + 2] = '0';
    CHECK(read_ar(e, c1.n, got2, 8, &err) < 0);
    printf("dangling name ref: %s\n", err);
    free(bad); free(d); free(d2); bfree(&a); bfree(&b); bfree(&c1);
    remove("lib.a");
    return 0;
}
