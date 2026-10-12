/*
 * title: HTTP Accept, Accept-Language and Accept-Encoding negotiation
 * topic: networking
 * covers: media range parsing with q-values in thousandths, specificity precedence, parameter matching, language prefix matching, identity coding defaults, server preference tie breaks, 406 outcomes, malformed header rejection
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { char type[32], sub[48], param[32]; int q; /* thousandths */ } Range;

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    *e = 0;
    return s;
}

/* q = 0 | 0.[0-9]{0,3} | 1 | 1.0{0,3} ; returns -1 on error */
static int parse_q(const char *s) {
    if (s[0] == '0') {
        int v = 0, d = 0;
        if (s[1] == 0) return 0;
        if (s[1] != '.') return -1;
        for (const char *p = s + 2; *p; p++) { if (!isdigit((unsigned char)*p) || ++d > 3) return -1; v = v * 10 + (*p - '0'); }
        while (d++ < 3) v *= 10;
        return v;
    }
    if (s[0] == '1') {
        if (s[1] == 0) return 1000;
        if (s[1] != '.') return -1;
        int d = 0;
        for (const char *p = s + 2; *p; p++) { if (*p != '0' || ++d > 3) return -1; }
        return 1000;
    }
    return -1;
}

static int token_ok(const char *s) {
    if (!*s) return 0;
    for (; *s; s++) if (!isalnum((unsigned char)*s) && !strchr("!#$%&'*+-.^_`|~", *s)) return 0;
    return 1;
}

static const char *parse_accept(const char *hdr, Range *r, int *n, int max) {
    char buf[300];
    if (strlen(hdr) >= sizeof buf) return "header-too-long";
    snprintf(buf, sizeof buf, "%s", hdr);
    *n = 0;
    char *save = NULL;
    for (char *item = strtok_r(buf, ",", &save); item; item = strtok_r(NULL, ",", &save)) {
        item = trim(item);
        if (!*item) continue;
        if (*n == max) return "too-many-ranges";
        char *semi = strchr(item, ';');
        char *params = NULL;
        if (semi) { *semi = 0; params = semi + 1; }
        char *slash = strchr(trim(item), '/');
        if (!slash) return "missing-slash";
        *slash = 0;
        char *ty = trim(item), *sub = trim(slash + 1);
        if (!token_ok(ty) || !token_ok(sub)) return "bad-media-token";
        if (!strcmp(ty, "*") && strcmp(sub, "*") != 0) return "wildcard-type-needs-wildcard-subtype";
        Range *x = &r[*n];
        memset(x, 0, sizeof *x);
        snprintf(x->type, sizeof x->type, "%s", ty);
        snprintf(x->sub, sizeof x->sub, "%s", sub);
        x->q = 1000;
        int seen_q = 0;
        char *ps = NULL;
        for (char *p = params ? strtok_r(params, ";", &ps) : NULL; p; p = strtok_r(NULL, ";", &ps)) {
            p = trim(p);
            char *eq = strchr(p, '=');
            if (!eq) return "bad-parameter";
            *eq = 0;
            char *k = trim(p), *v = trim(eq + 1);
            if (strcasecmp(k, "q") == 0) {
                int q = parse_q(v);
                if (q < 0) return "bad-qvalue";
                x->q = q;
                seen_q = 1;
            } else if (!seen_q) {
                snprintf(x->param, sizeof x->param, "%s=%s", k, v);
            } /* parameters after q are accept-ext and ignored */
        }
        (*n)++;
    }
    return NULL;
}

typedef struct { const char *type, *sub, *param; } Offer;

/* specificity: 3 exact with parameter, 2 exact, 1 type wildcard, 0 full wildcard, -1 no match */
static int specificity(const Range *r, const Offer *o) {
    if (!strcmp(r->type, "*")) return 0;
    if (strcasecmp(r->type, o->type) != 0) return -1;
    if (!strcmp(r->sub, "*")) return 1;
    if (strcasecmp(r->sub, o->sub) != 0) return -1;
    if (r->param[0]) return (o->param && strcasecmp(r->param, o->param) == 0) ? 3 : -1;
    return 2;
}

static int offer_q(const Range *r, int n, const Offer *o) {
    int best_spec = -1, q = 0;
    for (int i = 0; i < n; i++) {
        int s = specificity(&r[i], o);
        if (s > best_spec) { best_spec = s; q = r[i].q; }
    }
    return best_spec < 0 ? 0 : q;
}

static int negotiate(const char *hdr, const Offer *offers, int no, const char **why) {
    Range r[16];
    int n;
    const char *e = parse_accept(hdr, r, &n, 16);
    if (e) { *why = e; return -2; }
    int best = -1, bq = 0;
    for (int i = 0; i < no; i++) {
        int q = offer_q(r, n, &offers[i]);
        if (q > bq) { bq = q; best = i; } /* strictly greater keeps the earlier (server preferred) offer on ties */
    }
    *why = best < 0 ? "no-acceptable-offer" : "ok";
    return best;
}

/* Language: ranges like en-US, en, *; lookup with prefix matching on subtag boundaries. */
static int lang_match(const char *range, const char *tag) {
    if (!strcmp(range, "*")) return 1;
    size_t rl = strlen(range);
    if (strncasecmp(range, tag, rl) != 0) return 0;
    return tag[rl] == 0 || tag[rl] == '-';
}

static int negotiate_lang(const char *hdr, const char **avail, int na, char *why) {
    char buf[300];
    snprintf(buf, sizeof buf, "%s", hdr);
    struct { char tag[16]; int q; } rg[12];
    int nr = 0;
    char *save = NULL;
    for (char *it = strtok_r(buf, ",", &save); it; it = strtok_r(NULL, ",", &save)) {
        it = trim(it);
        char *semi = strchr(it, ';');
        int q = 1000;
        if (semi) {
            *semi = 0;
            char *p = trim(semi + 1);
            if (strncasecmp(p, "q=", 2) != 0 || (q = parse_q(trim(p + 2))) < 0) { snprintf(why, 40, "bad-qvalue"); return -2; }
        }
        it = trim(it);
        if (!*it) continue;
        for (char *c = it; *c; c++) if (!isalnum((unsigned char)*c) && *c != '-' && *c != '*') { snprintf(why, 40, "bad-language-range"); return -2; }
        if (nr == 12) break;
        snprintf(rg[nr].tag, sizeof rg[nr].tag, "%s", it);
        rg[nr++].q = q;
    }
    int best = -1, bq = 0, blen = -1;
    for (int i = 0; i < na; i++) {
        int q = 0, len = -1;
        for (int k = 0; k < nr; k++) {
            if (!lang_match(rg[k].tag, avail[i])) continue;
            int l = (int)strlen(rg[k].tag);
            if (!strcmp(rg[k].tag, "*")) l = 0;
            if (l > len) { len = l; q = rg[k].q; }
        }
        if (q > bq || (q == bq && q > 0 && len > blen)) { bq = q; best = i; blen = len; }
    }
    snprintf(why, 40, best < 0 ? "no-acceptable-language" : "ok");
    return best;
}

/* Encoding: identity is acceptable unless explicitly excluded. */
static int negotiate_enc(const char *hdr, const char **avail, int na) {
    char buf[200];
    snprintf(buf, sizeof buf, "%s", hdr);
    int q[8], star = -1, ident = -1;
    const char *names[8];
    int nn = 0;
    char *save = NULL;
    for (char *it = strtok_r(buf, ",", &save); it; it = strtok_r(NULL, ",", &save)) {
        it = trim(it);
        int qq = 1000;
        char *semi = strchr(it, ';');
        if (semi) { *semi = 0; char *p = trim(semi + 1); if (strncasecmp(p, "q=", 2) == 0) qq = parse_q(trim(p + 2)); if (qq < 0) return -2; }
        it = trim(it);
        if (!strcmp(it, "*")) star = qq;
        else if (!strcasecmp(it, "identity")) ident = qq;
        else if (nn < 8) { names[nn] = strdup(it); q[nn++] = qq; }
    }
    int best = -1, bq = 0;
    for (int i = 0; i < na; i++) {
        int qq;
        if (!strcasecmp(avail[i], "identity")) qq = ident >= 0 ? ident : (star >= 0 ? star : 1000);
        else {
            qq = star >= 0 ? star : 0;
            for (int k = 0; k < nn; k++) if (!strcasecmp(names[k], avail[i])) qq = q[k];
        }
        if (qq > bq) { bq = qq; best = i; }
    }
    for (int k = 0; k < nn; k++) free((void *)names[k]);
    return best;
}

int main(void) {
    Offer offers[] = { { "text", "html", NULL }, { "application", "json", NULL }, { "text", "plain", NULL }, { "image", "png", NULL }, { "text", "html", "level=1" } };
    const char *names[] = { "text/html", "application/json", "text/plain", "image/png", "text/html;level=1" };
    static const char *hdrs[] = {
        "text/html, application/json;q=0.9, */*;q=0.1", "application/json", "text/*;q=0.5, image/png;q=0.8", "*/*", "text/html;q=0", "image/*;q=0.2, text/plain;q=0.2",
        "text/html;level=1, text/html;q=0.7", "text/plain;q=0.3, text/html;q=0.3", "text/html;q=1.000, application/json;q=1", "audio/mpeg", "", "text/html;q=1.5", "text/html;q=0.1234",
        "text/, image/png", "*/html", "text/html;foo", "text/html;charset=utf-8;q=0.4, application/json;q=0.4;ext=1", "text/html; q=0.5 , image/png ; q=0.5",
    };
    for (size_t i = 0; i < sizeof hdrs / sizeof hdrs[0]; i++) {
        const char *why;
        int r = negotiate(hdrs[i], offers, 5, &why);
        printf("Accept: %-52s -> %s\n", hdrs[i][0] ? hdrs[i] : "(empty)", r >= 0 ? names[r] : (r == -2 ? why : "406 Not Acceptable"));
    }
    /* q parsing table. */
    static const struct { const char *s; int v; } qs[] = { { "0", 0 }, { "0.5", 500 }, { "0.05", 50 }, { "0.001", 1 }, { "1", 1000 }, { "1.0", 1000 }, { "1.000", 1000 }, { "0.", 0 }, { "1.5", -1 }, { "0.1234", -1 }, { "2", -1 }, { "-1", -1 }, { ".5", -1 }, { "0.5a", -1 } };
    for (size_t i = 0; i < sizeof qs / sizeof qs[0]; i++) CHECK(parse_q(qs[i].s) == qs[i].v);
    printf("q-value table of %zu cases verified\n", sizeof qs / sizeof qs[0]);

    const char *langs[] = { "en-US", "en-GB", "fr", "pt-BR", "de" };
    static const char *lh[] = { "en-GB, en;q=0.8", "fr;q=0.9, en;q=0.5", "pt, en;q=0.1", "*;q=0.1, de;q=0.9", "it, es", "en;q=0", "en-US;q=0.4, en;q=0.4, *;q=0.1", "EN-gb;q=0.7,x!;q=1", "es;q=0.5, pt-PT;q=0.5, *;q=0.01" };
    for (size_t i = 0; i < sizeof lh / sizeof lh[0]; i++) {
        char why[40];
        int r = negotiate_lang(lh[i], langs, 5, why);
        printf("Accept-Language: %-42s -> %s\n", lh[i], r >= 0 ? langs[r] : why);
    }
    const char *encs[] = { "br", "gzip", "identity" };
    static const char *eh[] = { "gzip, br;q=0.5", "br, gzip", "identity;q=0, gzip;q=0", "*;q=0", "gzip;q=0.8, *;q=0.3", "deflate", "", "gzip;q=0, identity;q=0.001", "br;q=0.2, gzip;q=0.2" };
    for (size_t i = 0; i < sizeof eh / sizeof eh[0]; i++) {
        int r = negotiate_enc(eh[i], encs, 3);
        printf("Accept-Encoding: %-30s -> %s\n", eh[i][0] ? eh[i] : "(empty)", r >= 0 ? encs[r] : "406/none");
    }
    return 0;
}
