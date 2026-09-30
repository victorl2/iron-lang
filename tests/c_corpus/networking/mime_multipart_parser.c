/*
 * title: MIME multipart/form-data and nested multipart parser
 * topic: networking
 * covers: boundary extraction from Content-Type, delimiter scanning on binary data, preamble and epilogue, part headers, Content-Disposition parameters, nested multipart, boundary-like content, malformed inputs
 * deps: libc
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    char name[32], filename[48], ctype[48];
    const unsigned char *data;
    size_t len;
} Part;

/* Extract parameter `key` from a header value like: form-data; name="a"; filename=b.txt */
static int header_param(const char *hv, const char *key, char *out, size_t cap) {
    const char *p = hv;
    size_t kl = strlen(key);
    while ((p = strchr(p, ';')) != NULL) {
        p++;
        while (*p == ' ') p++;
        if (strncasecmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            size_t n = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && n + 1 < cap) {
                    if (*p == '\\' && p[1]) p++;
                    out[n++] = *p++;
                }
            } else {
                while (*p && *p != ';' && *p != ' ' && n + 1 < cap) out[n++] = *p++;
            }
            out[n] = 0;
            return 1;
        }
    }
    return 0;
}

static const unsigned char *find(const unsigned char *h, size_t hl, const char *needle, size_t nl) {
    if (nl > hl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++) if (memcmp(h + i, needle, nl) == 0) return h + i;
    return NULL;
}

/* Parses body according to boundary; returns NULL on success or a reason. */
static const char *parse_multipart(const unsigned char *body, size_t n, const char *boundary, Part *parts, int max, int *np, size_t *preamble, size_t *epilogue) {
    if (!boundary[0] || strlen(boundary) > 70) return "bad-boundary";
    char delim[80], first[80];
    snprintf(first, sizeof first, "--%s", boundary);
    snprintf(delim, sizeof delim, "\r\n--%s", boundary);
    size_t fl = strlen(first), dl = strlen(delim);
    /* first delimiter may be at offset 0 or after a preamble ending in CRLF */
    const unsigned char *p;
    if (n >= fl && memcmp(body, first, fl) == 0) { p = body; *preamble = 0; }
    else {
        p = find(body, n, delim, dl);
        if (!p) return "no-opening-boundary";
        *preamble = (size_t)(p - body);
        p += 2;
    }
    p += fl;
    *np = 0;
    const unsigned char *end = body + n;
    for (;;) {
        if (end - p >= 2 && p[0] == '-' && p[1] == '-') { p += 2; break; }
        /* transport padding then CRLF */
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (end - p < 2 || p[0] != '\r' || p[1] != '\n') return "bad-delimiter-line";
        p += 2;
        if (*np == max) return "too-many-parts";
        Part *pt = &parts[*np];
        memset(pt, 0, sizeof *pt);
        /* headers */
        for (;;) {
            const unsigned char *eol = find(p, (size_t)(end - p), "\r\n", 2);
            if (!eol) return "truncated-headers";
            if (eol == p) { p += 2; break; }
            char line[200];
            size_t ll = (size_t)(eol - p);
            if (ll >= sizeof line) return "header-too-long";
            memcpy(line, p, ll);
            line[ll] = 0;
            char *colon = strchr(line, ':');
            if (!colon) return "bad-part-header";
            *colon = 0;
            char *v = colon + 1;
            while (*v == ' ') v++;
            if (strcasecmp(line, "Content-Disposition") == 0) {
                header_param(v, "name", pt->name, sizeof pt->name);
                header_param(v, "filename", pt->filename, sizeof pt->filename);
            } else if (strcasecmp(line, "Content-Type") == 0) {
                snprintf(pt->ctype, sizeof pt->ctype, "%s", v);
            }
            p = eol + 2;
        }
        const unsigned char *next = find(p, (size_t)(end - p), delim, dl);
        if (!next) return "missing-closing-boundary";
        pt->data = p;
        pt->len = (size_t)(next - p);
        (*np)++;
        p = next + dl;
    }
    /* after close-delimiter: optional CRLF then epilogue */
    if (end - p >= 2 && p[0] == '\r' && p[1] == '\n') p += 2;
    *epilogue = (size_t)(end - p);
    return NULL;
}

typedef struct { unsigned char *b; size_t n; } W;
static void wr(W *w, const void *d, size_t n) { memcpy(w->b + w->n, d, n); w->n += n; }
static void ws(W *w, const char *s) { wr(w, s, strlen(s)); }

static uint32_t rs = 0x3141592bu;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    static unsigned char msg[8192];
    W w = { msg, 0 };
    ws(&w, "This is the preamble.  It is to be ignored.\r\n");
    ws(&w, "--XyZ-boundary\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nHello multipart\r\n");
    ws(&w, "--XyZ-boundary\r\nContent-Disposition: form-data; name=\"tags\"\r\n\r\nc\r\n");
    ws(&w, "--XyZ-boundary\r\nContent-Disposition: form-data; name=\"tags\"\r\n\r\nnet\r\n");
    ws(&w, "--XyZ-boundary  \r\nContent-Disposition: form-data; name=\"note\"; filename=\"a \\\"quoted\\\" name.txt\"\r\ncontent-type: text/plain\r\n\r\n");
    ws(&w, "line one\r\n--XyZ-boundar\r\n--XyZ-bound\r\n-- XyZ-boundary\r\nend");
    ws(&w, "\r\n--XyZ-boundary\r\nContent-Disposition: form-data; name=\"empty\"\r\n\r\n\r\n");
    static unsigned char bin[300];
    for (size_t i = 0; i < sizeof bin; i++) bin[i] = (unsigned char)(rnd() >> 7);
    ws(&w, "--XyZ-boundary\r\nContent-Disposition: form-data; name=\"blob\"; filename=data.bin\r\nContent-Type: application/octet-stream\r\n\r\n");
    wr(&w, bin, sizeof bin);
    ws(&w, "\r\n--XyZ-boundary--\r\nThis is the epilogue.\r\n");
    Part parts[16];
    int np;
    size_t pre, epi;
    const char *e = parse_multipart(msg, w.n, "XyZ-boundary", parts, 16, &np, &pre, &epi);
    CHECK(e == NULL);
    printf("parsed %d parts, preamble %zu bytes, epilogue %zu bytes\n", np, pre, epi);
    for (int i = 0; i < np; i++) {
        printf("  part %d name=%-6s filename=%-22s type=%-26s len=%zu\n", i, parts[i].name, parts[i].filename[0] ? parts[i].filename : "-", parts[i].ctype[0] ? parts[i].ctype : "-", parts[i].len);
    }
    CHECK(np == 6 && parts[0].len == 15 && memcmp(parts[0].data, "Hello multipart", 15) == 0);
    CHECK(parts[4].len == 0 && parts[5].len == sizeof bin && memcmp(parts[5].data, bin, sizeof bin) == 0);
    CHECK(strcmp(parts[3].filename, "a \"quoted\" name.txt") == 0);
    printf("  decoy part is %zu bytes, first line [%.8s]\n", parts[3].len, (const char *)parts[3].data);

    /* Nested multipart/mixed inside the second-level part. */
    static unsigned char outer[2048];
    W o = { outer, 0 };
    ws(&o, "--OUT\r\nContent-Disposition: form-data; name=\"files\"\r\nContent-Type: multipart/mixed; boundary=IN\r\n\r\n");
    ws(&o, "--IN\r\nContent-Disposition: file; filename=\"one.txt\"\r\nContent-Type: text/plain\r\n\r\nfirst\r\n");
    ws(&o, "--IN\r\nContent-Disposition: file; filename=\"two.txt\"\r\nContent-Type: text/plain\r\n\r\nsecond\r\n--IN--");
    ws(&o, "\r\n--OUT--\r\n");
    Part op[4];
    CHECK(parse_multipart(outer, o.n, "OUT", op, 4, &np, &pre, &epi) == NULL && np == 1);
    char inner_b[80];
    CHECK(header_param(op[0].ctype, "boundary", inner_b, sizeof inner_b));
    Part ip[4];
    int ni;
    CHECK(parse_multipart(op[0].data, op[0].len, inner_b, ip, 4, &ni, &pre, &epi) == NULL);
    printf("nested: outer part %s holds %d inner parts:", op[0].name, ni);
    for (int i = 0; i < ni; i++) printf(" %s(%.*s)", ip[i].filename, (int)ip[i].len, (const char *)ip[i].data);
    printf("\n");
    CHECK(ni == 2);

    /* Random binary payloads that contain partial boundary text must round trip. */
    int ok = 0;
    for (int t = 0; t < 200; t++) {
        static unsigned char pl[3][400];
        size_t lens[3];
        W m = { msg, 0 };
        for (int k = 0; k < 3; k++) {
            lens[k] = rnd() % 400;
            for (size_t i = 0; i < lens[k]; i++) {
                uint32_t r = rnd();
                pl[k][i] = (r % 9 == 0) ? (unsigned char)"\r\n--bnd"[r % 7] : (unsigned char)(r >> 9);
            }
            char h[100];
            snprintf(h, sizeof h, "--bnd\r\nContent-Disposition: form-data; name=\"f%d\"\r\n\r\n", k);
            ws(&m, h);
            wr(&m, pl[k], lens[k]);
            ws(&m, "\r\n");
        }
        ws(&m, "--bnd--");
        /* payloads containing the full delimiter would be ambiguous; skip those (a writer must pick another boundary) */
        int ambiguous = 0;
        for (int k = 0; k < 3; k++) if (find(pl[k], lens[k], "\r\n--bnd", 7)) ambiguous = 1;
        if (ambiguous) continue;
        Part pp[4];
        CHECK(parse_multipart(msg, m.n, "bnd", pp, 4, &np, &pre, &epi) == NULL && np == 3);
        for (int k = 0; k < 3; k++) CHECK(pp[k].len == lens[k] && memcmp(pp[k].data, pl[k], lens[k]) == 0);
        ok++;
    }
    printf("random binary round trips: %d messages parsed exactly\n", ok);

    /* Malformed. */
    static const char *bad[] = {
        "--b\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nno end",
        "no boundary at all",
        "--b\r\nContent-Disposition form-data\r\n\r\nx\r\n--b--",
        "--b\r\nContent-Disposition: form-data; name=\"a\"",
        "--bXYZ\r\n\r\nx\r\n--b--",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char *r = parse_multipart((const unsigned char *)bad[i], strlen(bad[i]), "b", parts, 16, &np, &pre, &epi);
        printf("bad %zu: %s\n", i + 1, r ? r : "accepted");
        CHECK(r != NULL);
    }
    return 0;
}
