/*
 * title: Strict HTTP/1.1 request parser with smuggling defences
 * topic: networking
 * covers: request line grammar, header field syntax, obs-fold rejection, whitespace before colon, conflicting Content-Length, Transfer-Encoding with Content-Length, bare LF rejection, size limits, case-insensitive lookup
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { char name[48], value[128]; } Hdr;
typedef struct {
    char method[16], target[128];
    int minor;
    Hdr h[24];
    int nh;
    long content_length; /* -1 none */
    int chunked;
} Req;

static int tchar(int c) { return isalnum(c) || (c && strchr("!#$%&'*+-.^_`|~", c)); }

static const char *hget(const Req *r, const char *name) {
    for (int i = 0; i < r->nh; i++) {
        const char *a = r->h[i].name, *b = name;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*a && !*b) return r->h[i].value;
    }
    return NULL;
}

/* Parse one message head from buf[0..n). Returns NULL on success (and sets *used) or the rejection reason. */
static const char *parse_request(const char *buf, size_t n, Req *r, size_t *used) {
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    size_t pos = 0;
    int line = 0;
    int seen_cl = 0;
    for (;;) {
        size_t start = pos;
        while (pos < n && buf[pos] != '\n') {
            if (buf[pos] == '\r' && !(pos + 1 < n && buf[pos + 1] == '\n')) return "bare-cr";
            if (buf[pos] == '\0') return "nul-byte";
            pos++;
        }
        if (pos >= n) return "incomplete";
        if (pos == start || buf[pos - 1] != '\r') return "bare-lf";
        size_t len = pos - 1 - start;
        const char *ln = buf + start;
        pos++;
        if (len > 200) return "line-too-long";
        if (line == 0) {
            if (len == 0) return "empty-request-line";
            const char *sp1 = memchr(ln, ' ', len);
            if (!sp1) return "bad-request-line";
            const char *sp2 = memchr(sp1 + 1, ' ', len - (size_t)(sp1 + 1 - ln));
            if (!sp2) return "bad-request-line";
            size_t ml = (size_t)(sp1 - ln), tl = (size_t)(sp2 - sp1 - 1), vl = len - (size_t)(sp2 + 1 - ln);
            if (ml == 0 || ml >= sizeof r->method) return "bad-method";
            for (size_t i = 0; i < ml; i++) if (!tchar((unsigned char)ln[i])) return "bad-method";
            if (tl == 0 || tl >= sizeof r->target) return "bad-target";
            for (size_t i = 0; i < tl; i++) if ((unsigned char)sp1[1 + i] <= 32 || (unsigned char)sp1[1 + i] == 127) return "bad-target";
            if (vl != 8 || memcmp(sp2 + 1, "HTTP/1.", 7) != 0 || (sp2[8] != '0' && sp2[8] != '1')) return "bad-version";
            memcpy(r->method, ln, ml);
            memcpy(r->target, sp1 + 1, tl);
            r->minor = sp2[8] - '0';
            line = 1;
            continue;
        }
        if (len == 0) break;
        if (ln[0] == ' ' || ln[0] == '\t') return "obs-fold";
        const char *colon = memchr(ln, ':', len);
        if (!colon || colon == ln) return "bad-header-line";
        size_t nl = (size_t)(colon - ln);
        for (size_t i = 0; i < nl; i++) {
            if (ln[i] == ' ' || ln[i] == '\t') return "space-before-colon";
            if (!tchar((unsigned char)ln[i])) return "bad-header-name";
        }
        if (nl >= sizeof r->h[0].name) return "header-name-too-long";
        const char *v = colon + 1, *ve = ln + len;
        while (v < ve && (*v == ' ' || *v == '\t')) v++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        for (const char *t = v; t < ve; t++) if ((unsigned char)*t < 32 && *t != '\t') return "control-char-in-value";
        if (r->nh == 24) return "too-many-headers";
        if ((size_t)(ve - v) >= sizeof r->h[0].value) return "header-value-too-long";
        memcpy(r->h[r->nh].name, ln, nl);
        memcpy(r->h[r->nh].value, v, (size_t)(ve - v));
        Hdr *h = &r->h[r->nh++];
        if (strcasecmp(h->name, "Content-Length") == 0) {
            if (!h->value[0]) return "bad-content-length";
            long cl = 0;
            for (const char *t = h->value; *t; t++) {
                if (!isdigit((unsigned char)*t)) return "bad-content-length";
                cl = cl * 10 + (*t - '0');
                if (cl > 100000000L) return "content-length-too-large";
            }
            if (seen_cl && cl != r->content_length) return "conflicting-content-length";
            seen_cl = 1;
            r->content_length = cl;
        }
        if (strcasecmp(h->name, "Transfer-Encoding") == 0) {
            if (strcasecmp(h->value, "chunked") == 0) r->chunked = 1;
            else return "unsupported-transfer-encoding";
        }
    }
    if (r->minor == 1 && !hget(r, "Host")) return "missing-host";
    if (r->chunked && seen_cl) return "te-and-content-length";
    *used = pos;
    return NULL;
}

int main(void) {
    static const struct { const char *label; const char *msg; } cases[] = {
        { "simple GET", "GET /index.html HTTP/1.1\r\nHost: example.com\r\nAccept: */*\r\n\r\n" },
        { "POST with body length", "POST /submit HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\nContent-Type: text/plain\r\n\r\nhello world" },
        { "HTTP/1.0 without Host", "GET / HTTP/1.0\r\n\r\n" },
        { "OWS trimmed, mixed case", "PUT /x HTTP/1.1\r\nHOST:   spaced.example  \t\r\nX-Note:\ttabbed value\t \r\n\r\n" },
        { "chunked request", "POST /up HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n" },
        { "duplicate equal CL", "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n" },
        { "obs-fold", "GET / HTTP/1.1\r\nHost: h\r\nX-Long: part one\r\n  part two\r\n\r\n" },
        { "space before colon", "GET / HTTP/1.1\r\nHost : h\r\n\r\n" },
        { "bare LF", "GET / HTTP/1.1\nHost: h\n\n" },
        { "bare CR in value", "GET / HTTP/1.1\r\nHost: h\rX: y\r\n\r\n" },
        { "conflicting CL", "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n" },
        { "TE plus CL smuggling", "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n" },
        { "signed CL", "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: +5\r\n\r\n" },
        { "hex CL", "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 0x10\r\n\r\n" },
        { "missing Host 1.1", "GET / HTTP/1.1\r\nAccept: */*\r\n\r\n" },
        { "bad version", "GET / HTTP/2.0\r\nHost: h\r\n\r\n" },
        { "lowercase method ok as token", "get / HTTP/1.1\r\nHost: h\r\n\r\n" },
        { "space in target", "GET /a b HTTP/1.1\r\nHost: h\r\n\r\n" },
        { "double space", "GET  / HTTP/1.1\r\nHost: h\r\n\r\n" },
        { "NUL in header", "GET / HTTP/1.1\r\nHost: h\0x\r\n\r\n" },
        { "header name with space char", "GET / HTTP/1.1\r\nHost: h\r\nBad Name: v\r\n\r\n" },
        { "unknown transfer coding", "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n\r\n" },
        { "truncated head", "GET / HTTP/1.1\r\nHost: h\r\n" },
        { "empty header name", "GET / HTTP/1.1\r\nHost: h\r\n: v\r\n\r\n" },
    };
    int accepted = 0, rejected = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Req r;
        size_t used = 0;
        size_t len = strlen(cases[i].msg);
        if (strcmp(cases[i].label, "NUL in header") == 0) len = 39;
        const char *e = parse_request(cases[i].msg, len, &r, &used);
        if (e) {
            printf("%-28s REJECT %s\n", cases[i].label, e);
            rejected++;
        } else {
            const char *host = hget(&r, "host");
            printf("%-28s ok: %s %s HTTP/1.%d headers=%d host=%s body=%s\n", cases[i].label, r.method, r.target, r.minor, r.nh, host ? host : "-",
                   r.chunked ? "chunked" : (r.content_length >= 0 ? "length" : "none"));
            if (r.content_length >= 0) printf("%-28s   content-length=%ld, body bytes present=%zu\n", "", r.content_length, len - used);
            accepted++;
        }
    }
    printf("accepted %d, rejected %d\n", accepted, rejected);
    CHECK(accepted == 7 && rejected == 17);
    /* Header trimming detail. */
    Req r;
    size_t used;
    const char *m = "PUT /x HTTP/1.1\r\nHOST:   spaced.example  \t\r\nX-Note:\ttabbed value\t \r\n\r\n";
    CHECK(parse_request(m, strlen(m), &r, &used) == NULL);
    CHECK(strcmp(hget(&r, "host"), "spaced.example") == 0 && strcmp(hget(&r, "x-note"), "tabbed value") == 0);
    CHECK(used == strlen(m));
    printf("trimmed values: [%s] [%s]\n", hget(&r, "host"), hget(&r, "X-NOTE"));
    return 0;
}
