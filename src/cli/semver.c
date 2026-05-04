/* Phase 95 PIN: minimal semver constraint parser + comparator.
 * Standalone module: depends only on libc. No toml.h, no pkg_build.h.
 * See semver.h for API contract and v3.2 simplifications. */

#include "cli/semver.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SV_GE, SV_GT, SV_LE, SV_LT, SV_EQ, SV_CARET, SV_TILDE
} SemverOp;

typedef struct {
    int major, minor, patch;
} SemverVersion;

typedef struct {
    SemverOp op;
    SemverVersion version;
} SemverClause;

struct IronSemverConstraint {
    SemverClause *clauses;
    int           clause_count;
    char         *lower_bound_str; /* heap "X.Y.Z" or NULL */
};

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Skip leading whitespace; returns pointer to first non-space. */
static const char *skip_ws(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

/* Parse a non-negative decimal integer. Advances *p past digits.
 * Returns false if no digit was consumed or value would overflow. */
static bool parse_uint(const char **p, int *out) {
    const char *s = *p;
    if (!isdigit((unsigned char)*s)) return false;
    long v = 0;
    while (isdigit((unsigned char)*s)) {
        v = v * 10 + (*s - '0');
        if (v > 1000000000L) return false;
        s++;
    }
    *p = s;
    *out = (int)v;
    return true;
}

/* Parse "X.Y.Z" optionally followed by "-suffix" or "+build". On success,
 * advances *p past the suffix (so the caller can see what's left). The
 * X.Y.Z components must each be non-negative integers. Returns false on
 * any malformed component. */
static bool parse_version_triple(const char **p, SemverVersion *out) {
    const char *s = *p;
    if (!parse_uint(&s, &out->major)) return false;
    if (*s != '.') return false;
    s++;
    if (!parse_uint(&s, &out->minor)) return false;
    if (*s != '.') return false;
    s++;
    if (!parse_uint(&s, &out->patch)) return false;
    /* Optional pre-release / build suffix, stripped. */
    if (*s == '-' || *s == '+') {
        s++;
        while (*s && *s != ',' && !isspace((unsigned char)*s)) s++;
    }
    *p = s;
    return true;
}

/* Compare two versions: returns <0, 0, >0 (per major, then minor, then patch). */
static int version_cmp(const SemverVersion *a, const SemverVersion *b) {
    if (a->major != b->major) return a->major - b->major;
    if (a->minor != b->minor) return a->minor - b->minor;
    return a->patch - b->patch;
}

/* Match an operator prefix (longest first). On success, sets *op and
 * advances *p past the operator. If no operator matches, sets op=SV_EQ
 * and leaves *p untouched (default "exact" form). */
static void match_op(const char **p, SemverOp *op) {
    const char *s = *p;
    if (s[0] == '>' && s[1] == '=') { *op = SV_GE;    *p = s + 2; return; }
    if (s[0] == '<' && s[1] == '=') { *op = SV_LE;    *p = s + 2; return; }
    if (s[0] == '>')                 { *op = SV_GT;    *p = s + 1; return; }
    if (s[0] == '<')                 { *op = SV_LT;    *p = s + 1; return; }
    if (s[0] == '=')                 { *op = SV_EQ;    *p = s + 1; return; }
    if (s[0] == '^')                 { *op = SV_CARET; *p = s + 1; return; }
    if (s[0] == '~')                 { *op = SV_TILDE; *p = s + 1; return; }
    *op = SV_EQ; /* no operator -> exact */
}

/* ── Public API ─────────────────────────────────────────────────────────── */

IronSemverConstraint *iron_semver_parse(const char *constraint_str) {
    if (!constraint_str || !*constraint_str) return NULL;

    /* Up-front bound: comma count + 1. Reject obvious wildcards. */
    const char *probe = skip_ws(constraint_str);
    if (*probe == '*' || *probe == '\0') return NULL;

    int max_clauses = 1;
    for (const char *q = constraint_str; *q; q++) if (*q == ',') max_clauses++;

    SemverClause *clauses = calloc((size_t)max_clauses, sizeof(*clauses));
    if (!clauses) return NULL;

    int    count           = 0;
    char  *lower_bound_str = NULL;
    const char *cur        = constraint_str;

    while (*cur) {
        cur = skip_ws(cur);
        if (!*cur) { free(clauses); free(lower_bound_str); return NULL; }

        SemverOp op;
        match_op(&cur, &op);
        cur = skip_ws(cur);

        SemverVersion v;
        if (!parse_version_triple(&cur, &v)) {
            free(clauses); free(lower_bound_str); return NULL;
        }
        clauses[count].op      = op;
        clauses[count].version = v;

        /* Capture lower-bound string from the first GE/EQ/CARET/TILDE clause. */
        if (!lower_bound_str &&
            (op == SV_GE || op == SV_EQ || op == SV_CARET || op == SV_TILDE)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d.%d.%d", v.major, v.minor, v.patch);
            lower_bound_str = strdup(buf);
            if (!lower_bound_str) { free(clauses); return NULL; }
        }
        count++;

        cur = skip_ws(cur);
        if (*cur == ',') {
            cur++;
            cur = skip_ws(cur);
            if (!*cur) { free(clauses); free(lower_bound_str); return NULL; }
            continue;
        }
        if (*cur != '\0') {
            free(clauses); free(lower_bound_str); return NULL;
        }
    }

    if (count == 0) { free(clauses); free(lower_bound_str); return NULL; }

    IronSemverConstraint *c = calloc(1, sizeof(*c));
    if (!c) { free(clauses); free(lower_bound_str); return NULL; }
    c->clauses         = clauses;
    c->clause_count    = count;
    c->lower_bound_str = lower_bound_str;
    return c;
}

bool iron_semver_satisfies(const IronSemverConstraint *c, const char *version_str) {
    if (!c || !version_str) return false;

    const char *p = skip_ws(version_str);
    SemverVersion v;
    if (!parse_version_triple(&p, &v)) return false;
    /* Trailing whitespace-only is OK; anything else is malformed input. */
    p = skip_ws(p);
    if (*p != '\0') return false;

    for (int i = 0; i < c->clause_count; i++) {
        const SemverClause *cl = &c->clauses[i];
        int cmp = version_cmp(&v, &cl->version);
        bool ok = false;
        switch (cl->op) {
            case SV_GE: ok = (cmp >= 0); break;
            case SV_GT: ok = (cmp >  0); break;
            case SV_LE: ok = (cmp <= 0); break;
            case SV_LT: ok = (cmp <  0); break;
            case SV_EQ: ok = (cmp == 0); break;
            case SV_CARET:
                /* ^X.Y.Z: same major, version >= clause.
                 * ^0.X.Y (Cargo): same minor, version >= clause. */
                if (cl->version.major == 0) {
                    ok = (v.major == 0 && v.minor == cl->version.minor && cmp >= 0);
                } else {
                    ok = (v.major == cl->version.major && cmp >= 0);
                }
                break;
            case SV_TILDE:
                /* ~X.Y.Z: same major+minor, version >= clause. */
                ok = (v.major == cl->version.major &&
                      v.minor == cl->version.minor &&
                      cmp >= 0);
                break;
        }
        if (!ok) return false;
    }
    return true;
}

const char *iron_semver_suggest_version(const IronSemverConstraint *c) {
    if (!c) return NULL;
    return c->lower_bound_str;
}

void iron_semver_free(IronSemverConstraint *c) {
    if (!c) return;
    free(c->clauses);
    free(c->lower_bound_str);
    free(c);
}
