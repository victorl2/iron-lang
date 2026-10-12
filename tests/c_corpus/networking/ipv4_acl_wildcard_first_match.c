/*
 * title: IPv4 access list with wildcard masks, port operators and shadow detection
 * topic: networking
 * covers: ACL text parsing, wildcard masks including non-contiguous ones, host and any shortcuts, port eq gt lt neq range operators, first-match evaluation with implicit deny, hit counters, rule shadowing analysis, two-implementation cross-check
 * deps: libc
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

typedef struct { uint32_t base, wild; unsigned plo, phi; int has_port; } Spec;
typedef struct { int permit; int proto; /* 0 ip, 1 icmp, 6 tcp, 17 udp */ Spec src, dst; char text[100]; unsigned hits; int noncontig; } Rule;
typedef struct { uint32_t src, dst; unsigned sport, dport; int proto; } Pkt;

static int parse_ip(const char *s, uint32_t *out) {
    unsigned p[4];
    char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &p[0], &p[1], &p[2], &p[3], &extra) != 4) return 0;
    for (int i = 0; i < 4; i++) if (p[i] > 255) return 0;
    *out = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
    return 1;
}

static int contiguous_wild(uint32_t w) { return ((w + 1) & w) == 0; }

/* tokens */
typedef struct { char t[12][24]; int n, i; } Toks;

static const char *parse_spec(Toks *k, Spec *s, int ports_ok) {
    if (k->i >= k->n) return "missing-address";
    const char *t = k->t[k->i++];
    s->plo = 0; s->phi = 65535; s->has_port = 0;
    if (strcmp(t, "any") == 0) { s->base = 0; s->wild = 0xffffffffu; }
    else if (strcmp(t, "host") == 0) {
        if (k->i >= k->n || !parse_ip(k->t[k->i++], &s->base)) return "bad-host-address";
        s->wild = 0;
    } else {
        if (!parse_ip(t, &s->base)) return "bad-address";
        if (k->i >= k->n || !parse_ip(k->t[k->i++], &s->wild)) return "bad-wildcard";
        s->base &= ~s->wild;
    }
    if (k->i < k->n) {
        const char *op = k->t[k->i];
        int isop = !strcmp(op, "eq") || !strcmp(op, "gt") || !strcmp(op, "lt") || !strcmp(op, "neq") || !strcmp(op, "range");
        if (isop) {
            if (!ports_ok) return "port-on-non-l4-protocol";
            k->i++;
            unsigned a, b = 0;
            if (k->i >= k->n || sscanf(k->t[k->i++], "%u", &a) != 1 || a > 65535) return "bad-port";
            if (!strcmp(op, "range")) { if (k->i >= k->n || sscanf(k->t[k->i++], "%u", &b) != 1 || b > 65535 || b < a) return "bad-port-range"; s->plo = a; s->phi = b; }
            else if (!strcmp(op, "eq")) { s->plo = s->phi = a; }
            else if (!strcmp(op, "gt")) { if (a == 65535) return "bad-port"; s->plo = a + 1; }
            else if (!strcmp(op, "lt")) { if (a == 0) return "bad-port"; s->phi = a - 1; }
            else { return "neq-unsupported-in-range-form"; }
            s->has_port = 1;
        }
    }
    return NULL;
}

static const char *parse_rule(const char *line, Rule *r) {
    Toks k;
    memset(&k, 0, sizeof k);
    char buf[100];
    snprintf(buf, sizeof buf, "%s", line);
    for (char *p = strtok(buf, " "); p; p = strtok(NULL, " ")) { if (k.n == 12) return "too-many-tokens"; snprintf(k.t[k.n++], 24, "%s", p); }
    memset(r, 0, sizeof *r);
    snprintf(r->text, sizeof r->text, "%s", line);
    if (k.n < 4) return "too-short";
    if (!strcmp(k.t[0], "permit")) r->permit = 1; else if (strcmp(k.t[0], "deny") != 0) return "bad-action";
    const char *pr = k.t[1];
    r->proto = !strcmp(pr, "ip") ? 0 : !strcmp(pr, "icmp") ? 1 : !strcmp(pr, "tcp") ? 6 : !strcmp(pr, "udp") ? 17 : -1;
    if (r->proto < 0) return "bad-protocol";
    k.i = 2;
    int l4 = r->proto == 6 || r->proto == 17;
    const char *e = parse_spec(&k, &r->src, l4);
    if (e) return e;
    if ((e = parse_spec(&k, &r->dst, l4))) return e;
    if (k.i != k.n) return "trailing-tokens";
    r->noncontig = !contiguous_wild(r->src.wild) || !contiguous_wild(r->dst.wild);
    return NULL;
}

/* Implementation A: mask arithmetic. */
static int match_a(const Rule *r, const Pkt *p) {
    if (r->proto && r->proto != p->proto) return 0;
    if (((p->src ^ r->src.base) & ~r->src.wild) != 0) return 0;
    if (((p->dst ^ r->dst.base) & ~r->dst.wild) != 0) return 0;
    if (r->src.has_port && (p->sport < r->src.plo || p->sport > r->src.phi)) return 0;
    if (r->dst.has_port && (p->dport < r->dst.plo || p->dport > r->dst.phi)) return 0;
    return 1;
}

/* Implementation B: octet by octet. */
static int match_b(const Rule *r, const Pkt *p) {
    if (r->proto != 0 && r->proto != p->proto) return 0;
    for (int i = 0; i < 4; i++) {
        unsigned sh = (unsigned)(24 - 8 * i);
        unsigned sw = (r->src.wild >> sh) & 255, sb = (r->src.base >> sh) & 255, sa = (p->src >> sh) & 255;
        unsigned dw = (r->dst.wild >> sh) & 255, db = (r->dst.base >> sh) & 255, da = (p->dst >> sh) & 255;
        for (int bit = 0; bit < 8; bit++) {
            if (!((sw >> bit) & 1) && ((sb >> bit) & 1) != ((sa >> bit) & 1)) return 0;
            if (!((dw >> bit) & 1) && ((db >> bit) & 1) != ((da >> bit) & 1)) return 0;
        }
    }
    if (r->src.has_port && !(p->sport >= r->src.plo && p->sport <= r->src.phi)) return 0;
    if (r->dst.has_port && !(p->dport >= r->dst.plo && p->dport <= r->dst.phi)) return 0;
    return 1;
}

static int spec_covers(const Spec *a, const Spec *b) {
    if ((b->wild & ~a->wild) != 0) return 0; /* a must be wild wherever b is */
    if (((a->base ^ b->base) & ~a->wild) != 0) return 0;
    return a->plo <= b->plo && a->phi >= b->phi;
}
static int rule_covers(const Rule *a, const Rule *b) {
    if (a->proto != 0 && a->proto != b->proto) return 0;
    return spec_covers(&a->src, &b->src) && spec_covers(&a->dst, &b->dst);
}

static const char *evaluate(Rule *rules, int n, const Pkt *p, int *idx) {
    for (int i = 0; i < n; i++) {
        int a = match_a(&rules[i], p);
        CHECK(a == match_b(&rules[i], p));
        if (a) { *idx = i; return rules[i].permit ? "permit" : "deny"; }
    }
    *idx = -1;
    return "deny(implicit)";
}

static void ipstr(uint32_t a, char *s) { snprintf(s, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 255), (unsigned)((a >> 8) & 255), (unsigned)(a & 255)); }

static uint32_t rs = 0xac1ac1a1u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

int main(void) {
    static const char *lines[] = {
        "deny ip host 10.1.2.3 any",
        "permit tcp 10.1.0.0 0.0.255.255 any eq 80",
        "permit tcp 10.1.0.0 0.0.255.255 any eq 443",
        "permit udp 192.168.0.0 0.0.0.255 gt 1023 10.0.0.0 0.255.255.255 eq 53",
        "deny tcp any any range 6000 6010",
        "permit ip 10.0.0.5 0.0.255.0 any",
        "permit icmp any any",
        "permit tcp 10.1.5.0 0.0.0.255 any eq 80",
        "deny ip any any",
        "permit tcp any host 10.9.9.9 eq 22",
    };
    int n = (int)(sizeof lines / sizeof lines[0]);
    static Rule rules[16];
    for (int i = 0; i < n; i++) {
        const char *e = parse_rule(lines[i], &rules[i]);
        CHECK(e == NULL);
        printf("%2d: %s%s\n", i + 1, lines[i], rules[i].noncontig ? "   [non-contiguous wildcard]" : "");
    }
    /* Parse errors. */
    static const char *bad[] = { "allow ip any any", "permit gre any any", "permit ip any", "permit tcp any eq 80", "permit tcp 10.0.0.256 0.0.0.0 any any", "permit ip host any any",
                                 "permit icmp any eq 5 any", "permit tcp any any eq 70000", "permit tcp any any range 90 80", "permit ip any any extra", "permit tcp any any neq 5" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Rule r;
        const char *e = parse_rule(bad[i], &r);
        printf("reject '%s': %s\n", bad[i], e ? e : "ACCEPTED");
        CHECK(e != NULL);
    }
    /* Probe packets. */
    static const struct { uint32_t s, d; unsigned sp, dp; int proto; } probes[] = {
        { 0x0a010203u, 0x08080808u, 40000, 80, 6 }, { 0x0a010204u, 0x08080808u, 40000, 80, 6 }, { 0x0a010504u, 0x01010101u, 40000, 80, 6 }, { 0x0a010009u, 0x01010101u, 1, 443, 6 },
        { 0xc0a80005u, 0x0a000001u, 5000, 53, 17 }, { 0xc0a80005u, 0x0a000001u, 500, 53, 17 }, { 0x0a0a0a0au, 0x01010101u, 1, 6005, 6 }, { 0x0a000a05u, 0x01010101u, 1, 6005, 6 },
        { 0x0a00ff05u, 0x01010101u, 1, 9, 6 }, { 0x0b000001u, 0x0b000002u, 0, 0, 1 }, { 0x0b000001u, 0x0a090909u, 1, 22, 6 }, { 0x0c000001u, 0x0c000002u, 1, 2, 17 },
    };
    for (size_t i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        Pkt p = { probes[i].s, probes[i].d, probes[i].sp, probes[i].dp, probes[i].proto };
        int idx;
        const char *v = evaluate(rules, n, &p, &idx);
        char a[16], b[16];
        ipstr(p.src, a); ipstr(p.dst, b);
        printf("%-4s %-15s:%-5u -> %-15s:%-5u  %-15s rule %d\n", p.proto == 6 ? "tcp" : p.proto == 17 ? "udp" : "icmp", a, p.sport, b, p.dport, v, idx + 1);
    }
    /* Shadow analysis. */
    printf("shadowed rules:");
    int shadow[16] = { 0 };
    for (int j = 0; j < n; j++) for (int i = 0; i < j; i++) if (rule_covers(&rules[i], &rules[j])) { shadow[j] = 1; printf(" %d(by %d)", j + 1, i + 1); break; }
    printf("\n");
    /* Random traffic: counters, and shadowed rules must never be hit. */
    static const uint32_t pool[] = { 0x0a010203u, 0x0a010204u, 0x0a010504u, 0x0a000a05u, 0x0a00ff05u, 0xc0a80005u, 0x0a000001u, 0x0b000001u, 0x0a090909u, 0x08080808u, 0x0a630000u };
    unsigned permits = 0, denies = 0, implicit = 0;
    for (int t = 0; t < 20000; t++) {
        uint32_t r1 = rnd(), r2 = rnd(), r3 = rnd(), r4 = rnd();
        Pkt p;
        p.src = pool[r1 % 11]; p.dst = pool[r2 % 11];
        p.proto = (r3 % 4 == 0) ? 1 : (r3 % 4 == 1) ? 17 : 6;
        unsigned pp[8] = { 53, 80, 443, 22, 6005, 1024, 5000, 65535 };
        p.sport = pp[r3 / 4 % 8]; p.dport = pp[r4 % 8];
        if (p.proto == 1) p.sport = p.dport = 0;
        int idx;
        const char *v = evaluate(rules, n, &p, &idx);
        if (idx >= 0) { rules[idx].hits++; CHECK(!shadow[idx]); if (v[0] == 'p') permits++; else denies++; } else implicit++;
    }
    printf("20000 packets: %u permitted, %u denied by rule, %u implicit deny\n", permits, denies, implicit);
    printf("hit counters:");
    for (int i = 0; i < n; i++) printf(" %d:%u", i + 1, rules[i].hits);
    printf("\n");
    return 0;
}
