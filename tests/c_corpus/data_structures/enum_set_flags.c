/*
 * title: Enum sets as bitmasks with named members and subset enumeration
 * topic: data_structures
 * covers: enum set, bitmask flags, name tables, set algebra on masks, subset enumeration by mask trick, power set, permission lattice, closure under implications
 * deps: libc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long long rs = 0xE9075E7ULL * 0x9E37ULL;
static unsigned rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (unsigned)(rs >> 32); }
static void check(int c, const char *w) { if (!c) { fprintf(stderr, "check failed: %s\n", w); exit(1); } }

typedef enum { P_READ, P_WRITE, P_EXEC, P_DELETE, P_ADMIN, P_SHARE, P_AUDIT, P_OWN, P_COUNT } Perm;
typedef unsigned PermSet;
static const char *names[P_COUNT] = {"read", "write", "exec", "delete", "admin", "share", "audit", "own"};
#define BIT(p) (1u << (p))
#define ALL (BIT(P_COUNT) - 1u)

static int has(PermSet s, Perm p) { return (s >> p) & 1u; }
static int card(PermSet s) { int c = 0; while (s) { s &= s - 1; c++; } return c; }
static const char *fmt(PermSet s, char *buf, size_t n) {
    size_t len = 0; buf[0] = 0;
    for (int p = 0; p < P_COUNT; p++) if (has(s, (Perm)p)) len += (size_t)snprintf(buf + len, n - len, "%s%s", len ? "|" : "", names[p]);
    if (!len) snprintf(buf, n, "(none)");
    return buf;
}
/* implication rules: having `from` implies `to` (e.g. own => admin) */
static const struct { Perm from, to; } rules[] = { {P_OWN, P_ADMIN}, {P_ADMIN, P_WRITE}, {P_WRITE, P_READ}, {P_DELETE, P_WRITE}, {P_SHARE, P_READ}, {P_EXEC, P_READ} };
#define NR ((int)(sizeof rules / sizeof rules[0]))

static PermSet closure(PermSet s) {
    int changed = 1;
    while (changed) {
        changed = 0;
        for (int i = 0; i < NR; i++) if (has(s, rules[i].from) && !has(s, rules[i].to)) { s |= BIT(rules[i].to); changed = 1; }
    }
    return s;
}
/* reference closure with explicit boolean array */
static PermSet closure_ref(PermSet s) {
    int in[P_COUNT];
    for (int p = 0; p < P_COUNT; p++) in[p] = has(s, (Perm)p);
    for (int pass = 0; pass < P_COUNT; pass++) for (int i = 0; i < NR; i++) if (in[rules[i].from]) in[rules[i].to] = 1;
    PermSet o = 0; for (int p = 0; p < P_COUNT; p++) if (in[p]) o |= BIT(p);
    return o;
}

int main(void) {
    char b1[96], b2[96];
    /* full lattice properties over all 256 sets */
    int closed_sets = 0, idempotent = 1, monotone = 1;
    for (PermSet s = 0; s <= ALL; s++) {
        PermSet c = closure(s);
        check(c == closure_ref(s), "closure vs reference");
        if (closure(c) != c) idempotent = 0;
        if ((c & s) != s) monotone = 0;
        if (c == s) closed_sets++;
    }
    check(idempotent && monotone, "closure operator laws");
    printf("permission sets=%u closed under implication=%d\n", ALL + 1u, closed_sets);
    /* subset enumeration of a mask: sub = (sub - 1) & mask visits every subset in decreasing order */
    PermSet m = BIT(P_READ) | BIT(P_EXEC) | BIT(P_AUDIT) | BIT(P_OWN);
    int visited = 0; PermSet sub = m, prev_sub = m + 1;
    for (;;) {
        check((sub & ~m) == 0 && sub < prev_sub, "subset of mask, strictly decreasing");
        prev_sub = sub; visited++;
        if (sub == 0) break;
        sub = (sub - 1) & m;
    }
    check(visited == (1 << card(m)), "2^k subsets");
    printf("subsets of {%s}: %d\n", fmt(m, b1, sizeof b1), visited);
    /* random role assignments and effective permissions */
    PermSet roles[4] = { BIT(P_READ), BIT(P_WRITE) | BIT(P_SHARE), BIT(P_EXEC) | BIT(P_AUDIT), BIT(P_OWN) };
    const char *rname[4] = {"viewer", "editor", "runner", "owner"};
    for (int u = 0; u < 5; u++) {
        PermSet s = 0; int nr = 1 + (int)(rnd() % 3);
        for (int i = 0; i < nr; i++) { unsigned k = rnd() % 4; s |= roles[k]; }
        unsigned dp = rnd() % P_COUNT;
        PermSet denied = BIT(dp);
        PermSet eff = closure(s) & ~denied;
        PermSet missing = ALL & ~eff;
        printf("user%d: granted=%s | effective=%s (%d) | denied=%s | missing %d\n", u, fmt(s, b1, sizeof b1), fmt(eff, b2, sizeof b2), card(eff), names[dp], card(missing));
        check(card(eff) + card(missing) == P_COUNT, "partition");
    }
    (void)rname;
    /* minimal generating set: sets not implied by any other member of the closure */
    PermSet full = closure(BIT(P_OWN) | BIT(P_DELETE) | BIT(P_SHARE) | BIT(P_EXEC));
    PermSet gens = full;
    for (int p = 0; p < P_COUNT; p++) {
        PermSet without = full & ~BIT(p);
        if (has(full, (Perm)p) && has(closure(without & ~BIT(p)), (Perm)p)) gens &= ~BIT(p);
    }
    printf("closure of owner+delete+share+exec = %s\n", fmt(full, b1, sizeof b1));
    printf("irreducible members = %s\n", fmt(gens, b2, sizeof b2));
    /* count antichains: subsets where no member implies another member directly */
    int anti = 0;
    for (PermSet s = 0; s <= ALL; s++) {
        int ok = 1;
        for (int i = 0; i < NR; i++) if (has(s, rules[i].from) && has(s, rules[i].to)) ok = 0;
        anti += ok;
    }
    printf("sets with no direct redundancy: %d\n", anti);
    return 0;
}
