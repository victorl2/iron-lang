/*
 * title: RFC 3986 relative reference resolution with the RFC examples
 * topic: networking
 * covers: reference resolution algorithm, merge paths, remove_dot_segments, normal and abnormal examples, query and fragment inheritance, component recomposition
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { char scheme[16], auth[64], path[256], query[64], frag[64]; int has_scheme, has_auth, has_query, has_frag; } Ref;

/* RFC 3986 appendix B in hand written form. */
static void split(const char *s, Ref *r) {
    memset(r, 0, sizeof *r);
    const char *p = s;
    size_t n = strcspn(p, ":/?#");
    if (p[n] == ':' && n > 0) {
        snprintf(r->scheme, sizeof r->scheme, "%.*s", (int)n, p);
        r->has_scheme = 1;
        p += n + 1;
    }
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        n = strcspn(p, "/?#");
        snprintf(r->auth, sizeof r->auth, "%.*s", (int)n, p);
        r->has_auth = 1;
        p += n;
    }
    n = strcspn(p, "?#");
    snprintf(r->path, sizeof r->path, "%.*s", (int)n, p);
    p += n;
    if (*p == '?') {
        p++;
        n = strcspn(p, "#");
        snprintf(r->query, sizeof r->query, "%.*s", (int)n, p);
        r->has_query = 1;
        p += n;
    }
    if (*p == '#') { snprintf(r->frag, sizeof r->frag, "%s", p + 1); r->has_frag = 1; }
}

/* Section 5.2.4 as written: input buffer moves to output buffer. */
static void remove_dot_segments(const char *in_s, char *out) {
    char in[256];
    snprintf(in, sizeof in, "%s", in_s);
    char *i = in;
    out[0] = 0;
    size_t o = 0;
    while (*i) {
        if (strncmp(i, "../", 3) == 0) { i += 3; }
        else if (strncmp(i, "./", 2) == 0) { i += 2; }
        else if (strncmp(i, "/./", 3) == 0) { i += 2; }
        else if (strcmp(i, "/.") == 0) { memcpy(i, "/", 2); }
        else if (strncmp(i, "/../", 4) == 0 || strcmp(i, "/..") == 0) {
            if (i[3]) i += 3; else memcpy(i, "/", 2);
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            out[o] = 0;
        }
        else if (strcmp(i, ".") == 0 || strcmp(i, "..") == 0) { *i = 0; }
        else {
            size_t l = strcspn(i + (*i == '/' ? 1 : 0), "/") + (*i == '/' ? 1 : 0);
            memcpy(out + o, i, l);
            o += l;
            out[o] = 0;
            i += l;
        }
    }
}

static void merge(const Ref *base, const char *rpath, char *out) {
    if (base->has_auth && base->path[0] == 0) {
        snprintf(out, 256, "/%s", rpath);
    } else {
        const char *slash = strrchr(base->path, '/');
        size_t keep = slash ? (size_t)(slash - base->path) + 1 : 0;
        snprintf(out, 256, "%.*s%s", (int)keep, base->path, rpath);
    }
}

static void recompose(const Ref *t, char *out, size_t cap) {
    size_t n = 0;
    if (t->has_scheme) n += (size_t)snprintf(out + n, cap - n, "%s:", t->scheme);
    if (t->has_auth) n += (size_t)snprintf(out + n, cap - n, "//%s", t->auth);
    n += (size_t)snprintf(out + n, cap - n, "%s", t->path);
    if (t->has_query) n += (size_t)snprintf(out + n, cap - n, "?%s", t->query);
    if (t->has_frag) snprintf(out + n, cap - n, "#%s", t->frag);
}

/* strict resolution: a reference with a scheme is never treated as relative */
static void resolve(const Ref *base, const Ref *r, char *out, size_t cap) {
    Ref t;
    memset(&t, 0, sizeof t);
    char tmp[256];
    if (r->has_scheme) {
        t = *r;
        remove_dot_segments(r->path, tmp);
        snprintf(t.path, sizeof t.path, "%s", tmp);
    } else {
        if (r->has_auth) {
            t = *r;
            remove_dot_segments(r->path, tmp);
            snprintf(t.path, sizeof t.path, "%s", tmp);
        } else {
            if (r->path[0] == 0) {
                snprintf(t.path, sizeof t.path, "%s", base->path);
                if (r->has_query) { snprintf(t.query, sizeof t.query, "%s", r->query); t.has_query = 1; }
                else { snprintf(t.query, sizeof t.query, "%s", base->query); t.has_query = base->has_query; }
            } else {
                if (r->path[0] == '/') {
                    remove_dot_segments(r->path, tmp);
                } else {
                    char m[256];
                    merge(base, r->path, m);
                    remove_dot_segments(m, tmp);
                }
                snprintf(t.path, sizeof t.path, "%s", tmp);
                snprintf(t.query, sizeof t.query, "%s", r->query);
                t.has_query = r->has_query;
            }
            snprintf(t.auth, sizeof t.auth, "%s", base->auth);
            t.has_auth = base->has_auth;
        }
        snprintf(t.scheme, sizeof t.scheme, "%s", base->scheme);
        t.has_scheme = base->has_scheme;
    }
    snprintf(t.frag, sizeof t.frag, "%s", r->frag);
    t.has_frag = r->has_frag;
    recompose(&t, out, cap);
}

int main(void) {
    static const struct { const char *ref, *want; } cases[] = {
        /* 5.4.1 normal examples */
        { "g:h", "g:h" }, { "g", "http://a/b/c/g" }, { "./g", "http://a/b/c/g" }, { "g/", "http://a/b/c/g/" },
        { "/g", "http://a/g" }, { "//g", "http://g" }, { "?y", "http://a/b/c/d;p?y" }, { "g?y", "http://a/b/c/g?y" },
        { "#s", "http://a/b/c/d;p?q#s" }, { "g#s", "http://a/b/c/g#s" }, { "g?y#s", "http://a/b/c/g?y#s" },
        { ";x", "http://a/b/c/;x" }, { "g;x", "http://a/b/c/g;x" }, { "g;x?y#s", "http://a/b/c/g;x?y#s" },
        { "", "http://a/b/c/d;p?q" }, { ".", "http://a/b/c/" }, { "./", "http://a/b/c/" }, { "..", "http://a/b/" },
        { "../", "http://a/b/" }, { "../g", "http://a/b/g" }, { "../..", "http://a/" }, { "../../", "http://a/" },
        { "../../g", "http://a/g" },
        /* 5.4.2 abnormal examples */
        { "../../../g", "http://a/g" }, { "../../../../g", "http://a/g" }, { "/./g", "http://a/g" }, { "/../g", "http://a/g" },
        { "g.", "http://a/b/c/g." }, { ".g", "http://a/b/c/.g" }, { "g..", "http://a/b/c/g.." }, { "..g", "http://a/b/c/..g" },
        { "./../g", "http://a/b/g" }, { "./g/.", "http://a/b/c/g/" }, { "g/./h", "http://a/b/c/g/h" }, { "g/../h", "http://a/b/c/h" },
        { "g;x=1/./y", "http://a/b/c/g;x=1/y" }, { "g;x=1/../y", "http://a/b/c/y" },
        { "g?y/./x", "http://a/b/c/g?y/./x" }, { "g?y/../x", "http://a/b/c/g?y/../x" },
        { "g#s/./x", "http://a/b/c/g#s/./x" }, { "g#s/../x", "http://a/b/c/g#s/../x" },
        { "http:g", "http:g" },
    };
    Ref base;
    split("http://a/b/c/d;p?q", &base);
    printf("base: http://a/b/c/d;p?q\n");
    int n = (int)(sizeof cases / sizeof cases[0]);
    for (int i = 0; i < n; i++) {
        Ref r;
        char out[300];
        split(cases[i].ref, &r);
        resolve(&base, &r, out, sizeof out);
        printf("%-14s = %s%s\n", cases[i].ref[0] ? cases[i].ref : "(empty)", out, i == 22 ? "    [end of normal examples]" : "");
        CHECK(strcmp(out, cases[i].want) == 0);
    }
    printf("%d RFC examples resolved as specified\n", n);

    /* remove_dot_segments examples from 5.2.4 */
    static const struct { const char *in, *want; } rds[] = { { "/a/b/c/./../../g", "/a/g" }, { "mid/content=5/../6", "mid/6" }, { "/a/b/..", "/a/" }, { "/.", "/" }, { "a/..", "/" }, { "/../../x", "/x" }, { "/a//b/../c", "/a//c" } };
    for (size_t i = 0; i < sizeof rds / sizeof rds[0]; i++) {
        char o[256];
        remove_dot_segments(rds[i].in, o);
        printf("remove_dot_segments(%s) = \"%s\"\n", rds[i].in, o);
        CHECK(strcmp(o, rds[i].want) == 0);
    }
    /* merge with an authority and empty base path. */
    Ref b2, r2;
    char out[300];
    split("http://example.org", &b2);
    split("x/y", &r2);
    resolve(&b2, &r2, out, sizeof out);
    printf("empty base path: %s\n", out);
    CHECK(strcmp(out, "http://example.org/x/y") == 0);
    return 0;
}
