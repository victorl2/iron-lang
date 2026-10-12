/*
 * title: Practical email address validation (RFC 5321 subset)
 * topic: networking
 * covers: local part dot-atom and quoted-string forms, length limits 64 254 63, domain label rules, address literals, plus-tag and case normalization, structured rejection reasons
 * deps: libc
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef enum {
    E_OK, E_EMPTY, E_NO_AT, E_MULTI_AT, E_LOCAL_EMPTY, E_LOCAL_LONG, E_TOTAL_LONG, E_LOCAL_CHAR, E_LOCAL_DOT, E_QUOTE, E_DOMAIN_EMPTY,
    E_LABEL_EMPTY, E_LABEL_LONG, E_LABEL_HYPHEN, E_DOMAIN_CHAR, E_NO_TLD_DOT, E_TLD_NUMERIC, E_LITERAL, E_NONASCII
} Err;
static const char *ename[] = { "ok", "empty", "missing-@", "multiple-@", "empty-local", "local-too-long", "address-too-long", "bad-local-char", "bad-dot-placement",
                               "bad-quoted-string", "empty-domain", "empty-label", "label-too-long", "label-hyphen", "bad-domain-char", "single-label-domain", "numeric-tld",
                               "bad-address-literal", "non-ascii" };

typedef struct { char local[80], domain[260]; int quoted, literal; } Addr;

static int atext(int c) { return isalnum(c) || (c && strchr("!#$%&'*+-/=?^_`{|}~", c)); }

static Err check_v4(const char *s) {
    int parts = 0;
    while (1) {
        int nd = 0;
        unsigned v = 0;
        while (isdigit((unsigned char)*s)) { v = v * 10 + (unsigned)(*s - '0'); s++; if (++nd > 3) return E_LITERAL; }
        if (!nd || v > 255) return E_LITERAL;
        parts++;
        if (*s == '.') { s++; continue; }
        break;
    }
    return (parts == 4 && *s == 0) ? E_OK : E_LITERAL;
}

static Err validate(const char *s, Addr *a, int require_dotted) {
    memset(a, 0, sizeof *a);
    size_t len = strlen(s);
    if (!len) return E_EMPTY;
    for (size_t i = 0; i < len; i++) if ((unsigned char)s[i] >= 128) return E_NONASCII;
    if (len > 254) return E_TOTAL_LONG;
    /* find the split '@': the last one outside a quoted local part */
    size_t at = (size_t)-1;
    int inq = 0, ats = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && inq) { i++; continue; }
        if (s[i] == '"') inq = !inq;
        else if (s[i] == '@' && !inq) { at = i; ats++; }
    }
    if (inq) return E_QUOTE;
    if (ats == 0) return E_NO_AT;
    if (ats > 1) return E_MULTI_AT;
    if (at == 0) return E_LOCAL_EMPTY;
    if (at > 64) return E_LOCAL_LONG;
    const char *loc = s;
    size_t ll = at;
    if (loc[0] == '"') {
        if (ll < 2 || loc[ll - 1] != '"') return E_QUOTE;
        for (size_t i = 1; i + 1 < ll; i++) {
            unsigned char c = (unsigned char)loc[i];
            if (c == '\\') { if (i + 2 >= ll) return E_QUOTE; i++; if ((unsigned char)loc[i] < 32) return E_QUOTE; continue; }
            if (c == '"') return E_QUOTE;
            if (c < 32 || c == 127) return E_LOCAL_CHAR;
        }
        a->quoted = 1;
    } else {
        if (loc[0] == '.' || loc[ll - 1] == '.') return E_LOCAL_DOT;
        for (size_t i = 0; i < ll; i++) {
            if (loc[i] == '.') { if (i + 1 < ll && loc[i + 1] == '.') return E_LOCAL_DOT; continue; }
            if (!atext((unsigned char)loc[i])) return E_LOCAL_CHAR;
        }
    }
    snprintf(a->local, sizeof a->local, "%.*s", (int)ll, loc);
    const char *d = s + at + 1;
    size_t dl = len - at - 1;
    if (!dl) return E_DOMAIN_EMPTY;
    if (d[0] == '[') {
        if (d[dl - 1] != ']') return E_LITERAL;
        char inner[64];
        if (dl - 2 >= sizeof inner) return E_LITERAL;
        snprintf(inner, sizeof inner, "%.*s", (int)dl - 2, d + 1);
        if (strncasecmp(inner, "IPv6:", 5) == 0) {
            /* structural check only: hex groups and colons, at least two colons */
            int colons = 0;
            for (const char *p = inner + 5; *p; p++) { if (*p == ':') colons++; else if (!isxdigit((unsigned char)*p) && *p != '.') return E_LITERAL; }
            if (colons < 2) return E_LITERAL;
        } else {
            Err e = check_v4(inner);
            if (e) return e;
        }
        a->literal = 1;
        snprintf(a->domain, sizeof a->domain, "%s", d);
        return E_OK;
    }
    if (dl > 253) return E_TOTAL_LONG;
    int labels = 0;
    size_t ls = 0;
    char lastlabel[64] = "";
    for (size_t i = 0; i <= dl; i++) {
        if (i == dl || d[i] == '.') {
            size_t l = i - ls;
            if (l == 0) return E_LABEL_EMPTY;
            if (l > 63) return E_LABEL_LONG;
            if (d[ls] == '-' || d[i - 1] == '-') return E_LABEL_HYPHEN;
            snprintf(lastlabel, sizeof lastlabel, "%.*s", (int)l, d + ls);
            labels++;
            ls = i + 1;
        } else if (!isalnum((unsigned char)d[i]) && d[i] != '-') {
            return E_DOMAIN_CHAR;
        }
    }
    if (require_dotted && labels < 2) return E_NO_TLD_DOT;
    int allnum = 1;
    for (const char *p = lastlabel; *p; p++) if (!isdigit((unsigned char)*p)) allnum = 0;
    if (allnum && labels > 1) return E_TLD_NUMERIC;
    for (size_t i = 0; i < dl; i++) a->domain[i] = (char)tolower((unsigned char)d[i]);
    a->domain[dl] = 0;
    return E_OK;
}

/* Canonical form: domain lowercase; local part kept as is except gmail-style plus removal when requested. */
static void canonical(const Addr *a, int strip_plus, char *out, size_t cap) {
    char loc[80];
    snprintf(loc, sizeof loc, "%s", a->local);
    if (strip_plus && !a->quoted) { char *p = strchr(loc, '+'); if (p) *p = 0; }
    snprintf(out, cap, "%s@%s", loc, a->domain);
}

static unsigned rs = 0xe3a11e3au;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    static const char *tab[] = {
        "simple@example.com", "very.common@example.com", "disposable.style.email.with+symbol@example.com", "other.email-with-hyphen@example.com",
        "fully-qualified-domain@example.com", "user.name+tag+sorting@example.com", "x@example.com", "example-indeed@strange-example.com",
        "test/test@test.com", "admin@mailserver1", "\"john..doe\"@example.org", "\"very.(),:;<>[]\\\".VERY.\\\"very@\\\\ \\\"very\\\".unusual\"@strange.example.com",
        "user@[192.168.2.1]", "user@[IPv6:2001:db8::1]", "user@sub.Example.COM", "_______@example.com", "\"a b\"@example.com",
        "Abc.example.com", "A@b@c@example.com", "a\"b(c)d,e:f;g<h>i[j\\k]l@example.com", "just\"not\"right@example.com", "this is\"not\\allowed@example.com",
        ".leading@example.com", "trailing.@example.com", "double..dot@example.com", "@example.com", "user@", "user@.example.com", "user@example..com",
        "user@-example.com", "user@example-.com", "user@exa_mple.com", "user@example.123", "user@[300.1.1.1]", "user@[1.2.3]", "user@[IPv6:zz::1]",
        "us er@example.com", "user@exa mple.com", "\"unterminated@example.com", "caf\xc3\xa9@example.com", "", "user@example.com.",
    };
    int ok = 0, n = (int)(sizeof tab / sizeof tab[0]);
    for (int i = 0; i < n; i++) {
        Addr a;
        Err e = validate(tab[i], &a, 0);
        if (e == E_OK) {
            char c[300];
            canonical(&a, 0, c, sizeof c);
            printf("%-58.58s valid   %s%s\n", tab[i][0] ? tab[i] : "(empty)", c, a.quoted ? " [quoted]" : a.literal ? " [literal]" : "");
            ok++;
        } else {
            printf("%-58.58s INVALID %s\n", tab[i][0] ? tab[i] : "(empty)", ename[e]);
        }
    }
    printf("%d of %d valid\n", ok, n);
    CHECK(ok == 17);
    Addr a;
    CHECK(validate("admin@mailserver1", &a, 1) == E_NO_TLD_DOT);
    CHECK(validate("user@[10.0.0.1]", &a, 1) == E_OK);
    /* Length limits. */
    char big[400];
    memset(big, 'a', 64); big[64] = 0; strcat(big, "@example.com");
    CHECK(validate(big, &a, 0) == E_OK);
    memset(big, 'a', 65); big[65] = 0; strcat(big, "@example.com");
    CHECK(validate(big, &a, 0) == E_LOCAL_LONG);
    char lab[100];
    memset(lab, 'b', 63); lab[63] = 0;
    snprintf(big, sizeof big, "u@%s.com", lab);
    CHECK(validate(big, &a, 0) == E_OK);
    snprintf(big, sizeof big, "u@%sb.com", lab);
    CHECK(validate(big, &a, 0) == E_LABEL_LONG);
    snprintf(big, sizeof big, "u@%s.%s.%s.%s.com", lab, lab, lab, lab);
    printf("4x63 char labels: %s (address length %zu)\n", ename[validate(big, &a, 0)], strlen(big));
    char c[300];
    validate("First.Last+news@Mail.Example.ORG", &a, 0);
    canonical(&a, 0, c, sizeof c);
    printf("canonical: %s\n", c);
    canonical(&a, 1, c, sizeof c);
    printf("plus-stripped: %s\n", c);
    /* Fuzz: random strings over the address alphabet; validator must never overrun and the accepted ones must re-validate after canonicalization. */
    static const char alpha[] = "ab.-_+@\"[]:1 \\";
    int accepted = 0, tested = 0;
    for (int t = 0; t < 30000; t++) {
        char s[40];
        int l = 1 + (int)(rnd() % 30);
        for (int i = 0; i < l; i++) s[i] = alpha[rnd() % (sizeof alpha - 1)];
        s[l] = 0;
        Addr x;
        Err e = validate(s, &x, 0);
        tested++;
        if (e == E_OK) {
            char cn[300];
            Addr y;
            canonical(&x, 0, cn, sizeof cn);
            CHECK(validate(cn, &y, 0) == E_OK && strcmp(y.domain, x.domain) == 0);
            accepted++;
        }
    }
    printf("fuzz: %d strings, %d accepted, all canonical forms re-validate\n", tested, accepted);
    return 0;
}
