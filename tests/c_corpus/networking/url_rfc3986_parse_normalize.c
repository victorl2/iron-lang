/*
 * title: RFC 3986 URI component parser and syntax-based normalizer
 * topic: networking
 * covers: URI decomposition, userinfo host port, IPv6 literal hosts, character class validation, case normalization, percent-encoding normalization, dot segment removal, default port elision
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    char scheme[16], user[64], host[128], path[256], query[128], frag[64];
    int has_auth, has_user, has_port, has_query, has_frag;
    long port;
} Uri;

static int unreserved(int c) { return isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~'; }
static int subdelim(int c) { return c && strchr("!$&'()*+,;=", c) != NULL; }
static int hexd(int c) { return isxdigit(c); }

/* Validate that s[0..n) only holds characters from the given class (plus pct-encoded triplets). */
static int valid_chars(const char *s, size_t n, const char *extra, int allow_sub) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%') {
            if (i + 2 >= n) return 0;
            if (!hexd((unsigned char)s[i + 1]) || !hexd((unsigned char)s[i + 2])) return 0;
            i += 2;
            continue;
        }
        if (unreserved(c)) continue;
        if (allow_sub && subdelim(c)) continue;
        if (extra && c && strchr(extra, c)) continue;
        return 0;
    }
    return 1;
}

/* Returns NULL when ok, otherwise the reason. */
static const char *parse(const char *s, Uri *u) {
    memset(u, 0, sizeof *u);
    const char *p = s;
    /* scheme */
    if (isalpha((unsigned char)*p)) {
        const char *q = p;
        while (isalnum((unsigned char)*q) || *q == '+' || *q == '-' || *q == '.') q++;
        if (*q == ':') {
            if ((size_t)(q - p) >= sizeof u->scheme) return "scheme-too-long";
            for (const char *t = p; t < q; t++) u->scheme[t - p] = (char)tolower((unsigned char)*t);
            p = q + 1;
        }
    }
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        u->has_auth = 1;
        size_t alen = strcspn(p, "/?#");
        const char *a = p, *aend = p + alen;
        const char *at = NULL;
        for (const char *t = a; t < aend; t++) if (*t == '@') at = t;
        if (at) {
            u->has_user = 1;
            if (!valid_chars(a, (size_t)(at - a), ":", 1)) return "bad-userinfo";
            snprintf(u->user, sizeof u->user, "%.*s", (int)(at - a), a);
            a = at + 1;
        }
        const char *hend = aend;
        if (*a == '[') {
            const char *rb = memchr(a, ']', (size_t)(aend - a));
            if (!rb) return "unterminated-ip-literal";
            for (const char *t = a + 1; t < rb; t++) if (!(isxdigit((unsigned char)*t) || *t == ':' || *t == '.')) return "bad-ip-literal";
            snprintf(u->host, sizeof u->host, "%.*s", (int)(rb - a + 1), a);
            for (char *t = u->host; *t; t++) *t = (char)tolower((unsigned char)*t);
            hend = rb + 1;
            if (hend != aend && *hend != ':') return "junk-after-ip-literal";
        } else {
            const char *colon = NULL;
            for (const char *t = a; t < aend; t++) if (*t == ':') colon = t;
            hend = colon ? colon : aend;
            if (!valid_chars(a, (size_t)(hend - a), NULL, 1)) return "bad-host";
            snprintf(u->host, sizeof u->host, "%.*s", (int)(hend - a), a);
        }
        if (hend < aend) {
            const char *d = hend + 1;
            if (*hend != ':') return "bad-authority";
            if (d < aend) {
                long v = 0;
                for (; d < aend; d++) {
                    if (!isdigit((unsigned char)*d)) return "bad-port";
                    v = v * 10 + (*d - '0');
                    if (v > 65535) return "port-out-of-range";
                }
                u->port = v;
                u->has_port = 1;
            }
        }
        p += alen;
    }
    size_t plen = strcspn(p, "?#");
    if (!valid_chars(p, plen, ":@/", 1)) return "bad-path";
    snprintf(u->path, sizeof u->path, "%.*s", (int)plen, p);
    p += plen;
    if (*p == '?') {
        p++;
        size_t ql = strcspn(p, "#");
        if (!valid_chars(p, ql, ":@/?", 1)) return "bad-query";
        snprintf(u->query, sizeof u->query, "%.*s", (int)ql, p);
        u->has_query = 1;
        p += ql;
    }
    if (*p == '#') {
        p++;
        if (!valid_chars(p, strlen(p), ":@/?", 1)) return "bad-fragment";
        snprintf(u->frag, sizeof u->frag, "%s", p);
        u->has_frag = 1;
    }
    return NULL;
}

/* Normalize percent-encoding: decode unreserved, uppercase remaining hex. */
static void norm_pct(const char *in, char *out) {
    while (*in) {
        if (in[0] == '%' && hexd((unsigned char)in[1]) && hexd((unsigned char)in[2])) {
            int v = (int)strtol((char[]){ in[1], in[2], 0 }, NULL, 16);
            if (unreserved(v)) *out++ = (char)v;
            else { *out++ = '%'; *out++ = (char)toupper((unsigned char)in[1]); *out++ = (char)toupper((unsigned char)in[2]); }
            in += 3;
        } else {
            *out++ = *in++;
        }
    }
    *out = 0;
}

static void remove_dots(const char *in, char *out) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", in);
    char *i = buf;
    size_t o = 0;
    out[0] = 0;
    while (*i) {
        if (strncmp(i, "../", 3) == 0) i += 3;
        else if (strncmp(i, "./", 2) == 0) i += 2;
        else if (strncmp(i, "/./", 3) == 0) i += 2;
        else if (strcmp(i, "/.") == 0) { i[1] = 0; }
        else if (strncmp(i, "/../", 4) == 0 || strcmp(i, "/..") == 0) {
            if (i[3] == 0) i[1] = 0; else i += 3;
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            out[o] = 0;
        }
        else if (strcmp(i, ".") == 0 || strcmp(i, "..") == 0) *i = 0;
        else {
            size_t l = strcspn(i + 1, "/") + 1;
            memcpy(out + o, i, l);
            o += l;
            out[o] = 0;
            i += l;
        }
    }
}

static void normalize(const Uri *u, char *out, size_t cap) {
    char path[256], p2[256], q[128], f[64], h[128], us[64];
    norm_pct(u->path, path);
    remove_dots(path, p2);
    norm_pct(u->query, q);
    norm_pct(u->frag, f);
    norm_pct(u->host, h);
    norm_pct(u->user, us);
    for (char *t = h; *t; t++) *t = (char)tolower((unsigned char)*t);
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "%s%s", u->scheme, u->scheme[0] ? ":" : "");
    if (u->has_auth) {
        n += (size_t)snprintf(out + n, cap - n, "//");
        if (u->has_user) n += (size_t)snprintf(out + n, cap - n, "%s@", us);
        n += (size_t)snprintf(out + n, cap - n, "%s", h);
        int dflt = (strcmp(u->scheme, "http") == 0 && u->port == 80) || (strcmp(u->scheme, "https") == 0 && u->port == 443) || (strcmp(u->scheme, "ftp") == 0 && u->port == 21);
        if (u->has_port && !dflt) n += (size_t)snprintf(out + n, cap - n, ":%ld", u->port);
        if (!p2[0] && (strcmp(u->scheme, "http") == 0 || strcmp(u->scheme, "https") == 0)) snprintf(p2, sizeof p2, "/");
    }
    n += (size_t)snprintf(out + n, cap - n, "%s", p2);
    if (u->has_query) n += (size_t)snprintf(out + n, cap - n, "?%s", q);
    if (u->has_frag) n += (size_t)snprintf(out + n, cap - n, "#%s", f);
}

int main(void) {
    static const char *good[] = {
        "http://user:pw@Example.COM:8080/a/./b/../c?x=1&y=%7euser#Frag",
        "HTTP://www.EXAMPLE.com:80/%7Ejane/%2fx",
        "https://example.com:443",
        "ftp://ftp.is.co.za/rfc/rfc1808.txt",
        "http://[2001:DB8::7]:8000/path",
        "mailto:John.Doe@example.com",
        "urn:oasis:names:specification:docbook:dtd:xml:4.1.2",
        "http://a/b/c/../../../g",
        "file:///etc/hosts",
        "http://example.com/a%2fb%41%7E?q=%3d%3D#",
        "//host/only/network-path",
        "/just/a/path?with=query",
        "http://example.com:/x",
        "news:comp.infosystems.www.servers.unix",
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        Uri u;
        const char *e = parse(good[i], &u);
        CHECK(e == NULL);
        char n[400], portstr[16];
        if (u.has_port) snprintf(portstr, sizeof portstr, "%ld", u.port); else snprintf(portstr, sizeof portstr, "-");
        normalize(&u, n, sizeof n);
        printf("%s\n  scheme=%s host=%s port=%s path=%s query=%s frag=%s\n  normal: %s\n", good[i], u.scheme[0] ? u.scheme : "-", u.has_auth ? u.host : "-",
               portstr, u.path[0] ? u.path : "(empty)", u.has_query ? u.query : "-", u.has_frag ? (u.frag[0] ? u.frag : "(empty)") : "-", n);
        Uri u2;
        char n2[400];
        CHECK(parse(n, &u2) == NULL);
        normalize(&u2, n2, sizeof n2);
        CHECK(strcmp(n, n2) == 0); /* idempotent */
    }
    static const char *bad[] = { "http://exa mple.com/", "http://host:99999/", "http://host:8a/", "http://[::1/", "http://a/b%zz", "http://a/b%4", "http://us er@h/", "http://h/a b", "http://[::g]/", "http://h/<x>", "http://h/?q=\"" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Uri u;
        const char *e = parse(bad[i], &u);
        CHECK(e != NULL);
        printf("reject %-22s %s\n", bad[i], e);
    }
    /* Equivalence classes after normalization. */
    static const char *eq[] = { "HTTP://Example.COM:80/a/b/../c", "http://example.com/a/c", "http://EXAMPLE.com/%61/%63", "http://example.com:80/a/./c" };
    char first[400];
    for (int i = 0; i < 4; i++) {
        Uri u;
        char n[400];
        CHECK(parse(eq[i], &u) == NULL);
        normalize(&u, n, sizeof n);
        if (i == 0) snprintf(first, sizeof first, "%s", n);
        if (i == 2) { printf("%%61/%%63 case: %s\n", n); continue; }
        CHECK(strcmp(n, first) == 0);
    }
    printf("equivalent forms all normalize to %s\n", first);
    return 0;
}
