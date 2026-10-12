/*
 * title: Cookie and Set-Cookie parsing with a domain and path matching jar
 * topic: networking
 * covers: RFC 6265 cookie header parsing, Set-Cookie attributes, IMF-fixdate to epoch conversion, domain-match and path-match, expiry and Max-Age precedence, cookie name prefixes, header ordering
 * deps: libc
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct {
    char name[32], value[64], domain[64], path[64];
    int host_only, secure, httponly;
    long long expires; /* -1 session */
    int seq;
    int samesite; /* 0 none set, 1 lax, 2 strict, 3 none */
} Cookie;

static long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

/* "Wed, 09 Jun 2021 10:18:14 GMT" */
static int parse_date(const char *s, long long *out) {
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char wd[4], mn[4], tz[4];
    int d, y, h, mi, se;
    if (sscanf(s, "%3s, %d %3s %d %d:%d:%d %3s", wd, &d, mn, &y, &h, &mi, &se, tz) != 8) return 0;
    if (strcmp(tz, "GMT") != 0) return 0;
    int m = -1;
    for (int i = 0; i < 12; i++) if (strcmp(mn, mon[i]) == 0) m = i + 1;
    if (m < 0 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return 0;
    *out = days_from_civil(y, (unsigned)m, (unsigned)d) * 86400 + h * 3600 + mi * 60 + se;
    return 1;
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    *e = 0;
    return s;
}

static int valid_name(const char *n) {
    if (!*n) return 0;
    for (; *n; n++) if ((unsigned char)*n <= 32 || (unsigned char)*n >= 127 || strchr("()<>@,;:\\\"/[]?={}", *n)) return 0;
    return 1;
}

/* Parse a Set-Cookie header value received from request host `host` for request path `rpath`.
   Returns NULL on success or the reason it was ignored. */
static const char *parse_set_cookie(const char *hdr, const char *host, const char *rpath, int https, long long now, int seq, Cookie *c) {
    char buf[300];
    snprintf(buf, sizeof buf, "%s", hdr);
    memset(c, 0, sizeof *c);
    c->expires = -1;
    c->seq = seq;
    long long max_age = 0;
    int have_max_age = 0;
    long long expires = -1;
    char *save = NULL;
    char *first = strtok_r(buf, ";", &save);
    if (!first) return "empty";
    char *eq = strchr(first, '=');
    if (!eq) return "no-equals";
    *eq = 0;
    char *nm = trim(first), *vl = trim(eq + 1);
    if (!valid_name(nm)) return "bad-name";
    snprintf(c->name, sizeof c->name, "%s", nm);
    size_t vlen = strlen(vl);
    if (vlen >= 2 && vl[0] == '"' && vl[vlen - 1] == '"') { vl[vlen - 1] = 0; vl++; }
    snprintf(c->value, sizeof c->value, "%s", vl);
    c->host_only = 1;
    snprintf(c->domain, sizeof c->domain, "%s", host);
    /* default path: directory of request path */
    const char *ls = strrchr(rpath, '/');
    if (!ls || ls == rpath) snprintf(c->path, sizeof c->path, "/");
    else snprintf(c->path, sizeof c->path, "%.*s", (int)(ls - rpath), rpath);
    for (char *attr = strtok_r(NULL, ";", &save); attr; attr = strtok_r(NULL, ";", &save)) {
        attr = trim(attr);
        char *av = strchr(attr, '=');
        if (av) { *av++ = 0; av = trim(av); }
        attr = trim(attr);
        if (ieq(attr, "Domain") && av && *av) {
            if (*av == '.') av++;
            char d[64];
            size_t i = 0;
            for (; av[i] && i < 63; i++) d[i] = (char)tolower((unsigned char)av[i]);
            d[i] = 0;
            size_t hl = strlen(host), dl = strlen(d);
            int ok = (hl == dl && strcmp(host, d) == 0) || (hl > dl && strcmp(host + hl - dl, d) == 0 && host[hl - dl - 1] == '.');
            if (!ok) return "domain-mismatch";
            if (strchr(d, '.') == NULL) return "public-suffix-like-domain";
            snprintf(c->domain, sizeof c->domain, "%s", d);
            c->host_only = 0;
        } else if (ieq(attr, "Path") && av && av[0] == '/') {
            snprintf(c->path, sizeof c->path, "%s", av);
        } else if (ieq(attr, "Expires") && av) {
            long long t;
            if (parse_date(av, &t)) expires = t;
        } else if (ieq(attr, "Max-Age") && av) {
            char *end;
            long long v = strtoll(av, &end, 10);
            if (*end == 0 && *av) { max_age = v; have_max_age = 1; }
        } else if (ieq(attr, "Secure")) c->secure = 1;
        else if (ieq(attr, "HttpOnly")) c->httponly = 1;
        else if (ieq(attr, "SameSite") && av) {
            c->samesite = ieq(av, "Lax") ? 1 : ieq(av, "Strict") ? 2 : ieq(av, "None") ? 3 : 0;
        }
    }
    if (have_max_age) c->expires = max_age <= 0 ? 0 : now + max_age;
    else if (expires >= 0) c->expires = expires;
    if (strncmp(c->name, "__Secure-", 9) == 0 && (!c->secure || !https)) return "secure-prefix-violation";
    if (strncmp(c->name, "__Host-", 7) == 0 && (!c->secure || !https || !c->host_only || strcmp(c->path, "/") != 0)) return "host-prefix-violation";
    if (c->samesite == 3 && !c->secure) return "samesite-none-needs-secure";
    return NULL;
}

static int domain_match(const Cookie *c, const char *host) {
    if (c->host_only) return strcmp(c->domain, host) == 0;
    size_t hl = strlen(host), dl = strlen(c->domain);
    return (hl == dl && strcmp(host, c->domain) == 0) || (hl > dl && strcmp(host + hl - dl, c->domain) == 0 && host[hl - dl - 1] == '.');
}

static int path_match(const char *cp, const char *rp) {
    size_t cl = strlen(cp);
    if (strncmp(rp, cp, cl) != 0) return 0;
    return rp[cl] == 0 || cp[cl - 1] == '/' || rp[cl] == '/';
}

typedef struct { Cookie c[16]; int n; } Jar;

static void jar_store(Jar *j, const Cookie *c, long long now) {
    for (int i = 0; i < j->n; i++) {
        if (strcmp(j->c[i].name, c->name) == 0 && strcmp(j->c[i].domain, c->domain) == 0 && strcmp(j->c[i].path, c->path) == 0 && j->c[i].host_only == c->host_only) {
            if (c->expires >= 0 && c->expires <= now) { j->c[i] = j->c[--j->n]; return; }
            Cookie keep = j->c[i];
            j->c[i] = *c;
            j->c[i].seq = keep.seq; /* creation order preserved on replacement */
            return;
        }
    }
    if (c->expires >= 0 && c->expires <= now) return;
    CHECK(j->n < 16);
    j->c[j->n++] = *c;
}

static int by_order(const void *a, const void *b) {
    const Cookie *x = a, *y = b;
    size_t px = strlen(x->path), py = strlen(y->path);
    if (px != py) return px > py ? -1 : 1;
    return x->seq - y->seq;
}

static void cookie_header(const Jar *j, const char *host, const char *path, int https, long long now, char *out, size_t cap) {
    Cookie sel[16];
    int n = 0;
    for (int i = 0; i < j->n; i++) {
        const Cookie *c = &j->c[i];
        if (c->expires >= 0 && c->expires <= now) continue;
        if (!domain_match(c, host) || !path_match(c->path, path)) continue;
        if (c->secure && !https) continue;
        sel[n++] = *c;
    }
    qsort(sel, (size_t)n, sizeof sel[0], by_order);
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < n; i++) o += (size_t)snprintf(out + o, cap - o, "%s%s=%s", i ? "; " : "", sel[i].name, sel[i].value);
}

/* Parse a request Cookie header into pairs; tolerant of extra spaces, rejects pairs without '='. */
static int parse_cookie_header(const char *h, char names[][32], char vals[][64], int max) {
    char buf[300];
    snprintf(buf, sizeof buf, "%s", h);
    int n = 0;
    char *save = NULL;
    for (char *p = strtok_r(buf, ";", &save); p; p = strtok_r(NULL, ";", &save)) {
        p = trim(p);
        char *eq = strchr(p, '=');
        if (!eq || eq == p) continue;
        *eq = 0;
        if (n == max) break;
        snprintf(names[n], 32, "%s", trim(p));
        char *v = trim(eq + 1);
        size_t l = strlen(v);
        if (l >= 2 && v[0] == '"' && v[l - 1] == '"') { v[l - 1] = 0; v++; }
        snprintf(vals[n], 64, "%s", v);
        n++;
    }
    return n;
}

int main(void) {
    long long t;
    CHECK(parse_date("Thu, 01 Jan 1970 00:00:00 GMT", &t) && t == 0);
    CHECK(parse_date("Wed, 09 Jun 2021 10:18:14 GMT", &t) && t == 1623233894LL);
    CHECK(parse_date("Tue, 29 Feb 2000 23:59:59 GMT", &t) && t == 951868799LL);
    printf("date checks: epoch, 2021-06-09, leap day 2000 ok (t=%lld)\n", t);
    CHECK(!parse_date("Wed, 09 Jun 2021 10:18:14 PST", &t) && !parse_date("garbage", &t));

    const long long now = 1700000000LL; /* fixed clock */
    static const struct { const char *host, *path; int https; const char *hdr; } sets[] = {
        { "www.example.com", "/app/login", 1, "sid=abc123; Path=/; Secure; HttpOnly; SameSite=Lax" },
        { "www.example.com", "/app/login", 1, "theme=dark; Domain=example.com; Max-Age=3600" },
        { "www.example.com", "/app/login", 1, "pref=\"a b\"; Path=/app; Expires=Wed, 09 Jun 2031 10:18:14 GMT" },
        { "www.example.com", "/app/login", 1, "tmp=1; Expires=Wed, 09 Jun 2001 10:18:14 GMT" },
        { "www.example.com", "/app/login", 1, "evil=1; Domain=other.org" },
        { "www.example.com", "/app/login", 1, "wide=1; Domain=com" },
        { "www.example.com", "/app/login", 1, "__Host-tok=z; Secure; Path=/" },
        { "www.example.com", "/app/login", 0, "__Secure-x=1; Secure" },
        { "www.example.com", "/app/login", 1, "__Host-bad=1; Secure; Domain=example.com; Path=/" },
        { "www.example.com", "/app/login", 1, "noeq" },
        { "www.example.com", "/app/login", 1, "bad name=1" },
        { "www.example.com", "/app/login", 1, "s=1; SameSite=None" },
        { "www.example.com", "/app/login", 1, "deep=1" },
        { "www.example.com", "/app/login", 1, "theme=light; Domain=.EXAMPLE.com; Max-Age=7200" },
        { "www.example.com", "/app/login", 1, "gone=1; Max-Age=0" },
    };
    static Jar jar;
    for (size_t i = 0; i < sizeof sets / sizeof sets[0]; i++) {
        Cookie c;
        const char *e = parse_set_cookie(sets[i].hdr, sets[i].host, sets[i].path, sets[i].https, now, (int)i, &c);
        if (e) { printf("%-70s ignored: %s\n", sets[i].hdr, e); continue; }
        printf("%-70s %s: %s domain=%s%s path=%s exp=%lld\n", sets[i].hdr, (c.expires >= 0 && c.expires <= now) ? "expired on arrival" : "stored", c.name, c.domain, c.host_only ? "(host-only)" : "", c.path, c.expires);
        jar_store(&jar, &c, now);
    }
    printf("jar holds %d cookies\n", jar.n);
    static const struct { const char *host, *path; int https; } reqs[] = {
        { "www.example.com", "/app/login", 1 }, { "www.example.com", "/app/login", 0 }, { "www.example.com", "/", 1 }, { "api.example.com", "/app/x", 1 },
        { "example.com", "/app", 1 }, { "www.example.com", "/application", 1 }, { "notexample.com", "/", 1 },
    };
    for (size_t i = 0; i < sizeof reqs / sizeof reqs[0]; i++) {
        char h[300];
        cookie_header(&jar, reqs[i].host, reqs[i].path, reqs[i].https, now, h, sizeof h);
        printf("GET %s%s %s -> Cookie: %s\n", reqs[i].host, reqs[i].path, reqs[i].https ? "https" : "http", h[0] ? h : "(none)");
    }
    char later[300];
    cookie_header(&jar, "www.example.com", "/app/login", 1, now + 8000, later, sizeof later);
    printf("two hours and 13 minutes later: %s\n", later);

    char nm[8][32], vl[8][64];
    int n = parse_cookie_header("a=1;  b = two ; c=\"quoted;no\"; =x; junk; d=", nm, vl, 8);
    printf("request header parsed into %d pairs:", n);
    for (int i = 0; i < n; i++) printf(" [%s|%s]", nm[i], vl[i]);
    printf("\n");
    return 0;
}
