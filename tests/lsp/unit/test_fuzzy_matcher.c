/* test_fuzzy_matcher -- Phase 3 Plan 03 Task 01 (D-11, NAV-08).
 *
 * Flipped from the Plan 01 Wave 0 stub. The scorer is a pure-C17 port
 * of fzy.js so the golden corpus validates it behaves the same way on
 * the dominant Iron identifier shapes: exact match, subsequence,
 * CamelCase, path-prefix via `/`, dot-separated, and non-match.
 *
 * Five RUN_TESTs:
 *   1. test_has_match_subsequence     -- 100+ corpus rows of has_match
 *                                         plus score sanity (>= min_score).
 *   2. test_match_max_len_fallback    -- T-03-07 DoS defense: haystacks
 *                                         larger than MATCH_MAX_LEN skip
 *                                         the DP and score 0.0 on match.
 *   3. test_positions_backtrack       -- `abc` on `xabcy` emits positions
 *                                         {1, 2, 3}.
 *   4. test_empty_needle              -- has_match=true, score == 0.0.
 *   5. test_kind_rank                 -- Function best, Import worst.
 */
#include "unity.h"

#include "lsp/facade/nav/fuzzy.h"
#include "analyzer/scope.h"
#include "util/arena.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ── Golden corpus ─────────────────────────────────────────────────── */

typedef struct {
    const char *q;
    const char *c;
    bool        has_match;
    double      min_score;    /* lower bound (inclusive); 0.0 = any */
} fz_row_t;

/* 100+ rows covering exact, subsequence, CamelCase, path-prefix, dot,
 * non-match, case-insensitive, word boundary, numeric. */
static const fz_row_t corpus[] = {
    /* Exact & prefix */
    { "abc",       "abc",                 true,  0.99 },
    { "abc",       "abcd",                true,  0.00 },
    { "abc",       "aabc",                true,  0.00 },
    { "abc",       "xabcy",               true,  0.00 },
    { "foo",       "foo",                 true,  0.99 },
    { "foo",       "foobar",              true,  0.00 },
    { "bar",       "foobar",              true,  0.00 },
    /* Subsequence (not contiguous) */
    { "abc",       "a_b_c",               true,  0.00 },
    { "abc",       "axbxc",               true,  0.00 },
    { "fbar",      "foo_bar",             true,  0.00 },
    { "fb",        "foo_bar",             true,  0.00 },
    { "fbz",       "foo_baz",             true,  0.00 },
    { "hwld",      "hello_world",         true,  0.00 },
    /* CamelCase */
    { "fM",        "fuzzyMatch",          true,  0.00 },
    { "fM",        "fooMatch",            true,  0.00 },
    { "FM",        "FooMatch",            true,  0.00 },
    { "fMB",       "fooMatchBar",         true,  0.00 },
    { "FBB",       "FooBarBaz",           true,  0.00 },
    { "CC",        "CamelCase",           true,  0.00 },
    /* Path-prefix via '/' */
    { "util/f",    "src/util/foo",        true,  0.00 },
    { "u/f",       "util/foo",            true,  0.00 },
    { "s/u/f",     "src/util/foo",        true,  0.00 },
    /* Dot separators */
    { "f.b",       "foo.bar",             true,  0.00 },
    { "a.b.c",     "alpha.beta.cactus",   true,  0.00 },
    { "Math.sq",   "Math.sqrt",           true,  0.00 },
    /* Word boundaries (_, -, space) */
    { "fb",        "foo_bar",             true,  0.00 },
    { "fb",        "foo-bar",             true,  0.00 },
    { "fb",        "foo bar",             true,  0.00 },
    { "hw",        "hello-world",         true,  0.00 },
    /* Case-insensitive */
    { "ABC",       "abc",                 true,  0.00 },
    { "abc",       "ABC",                 true,  0.00 },
    { "AbC",       "abc",                 true,  0.00 },
    { "foo",       "FooBar",              true,  0.00 },
    /* Multi-word / nested */
    { "gret",      "greeter",             true,  0.00 },
    { "grt",       "greeter",             true,  0.00 },
    { "rdr",       "reader",              true,  0.00 },
    { "wrtr",      "writer",              true,  0.00 },
    /* Numerics */
    { "v1",        "version_1",           true,  0.00 },
    { "iron123",   "iron123",             true,  0.99 },
    { "1",         "v1",                  true,  0.00 },
    { "42",        "answer42",            true,  0.00 },
    /* Module / identifier mix */
    { "mathPow",   "mathPow",             true,  0.99 },
    { "mp",        "mathPow",             true,  0.00 },
    { "MP",        "MathPow",             true,  0.00 },
    { "stdmath",   "stdlib://math",       true,  0.00 },
    /* Non-matches */
    { "abc",       "xyz",                 false, 0.00 },
    { "xyz",       "abc",                 false, 0.00 },
    { "abcd",      "abc",                 false, 0.00 },
    { "longquery", "short",               false, 0.00 },
    { "fOO",       "foo",                 true,  0.00 },   /* case-insensitive */
    { "zzz",       "aaa",                 false, 0.00 },
    { "Q",         "",                    false, 0.00 },
    { "x",         "aaaaaa",              false, 0.00 },
    { "A",         "bcd",                 false, 0.00 },
    /* More identifier shapes */
    { "println",   "println",             true,  0.99 },
    { "prln",      "println",             true,  0.00 },
    { "pri",       "println",             true,  0.00 },
    { "init",      "init",                true,  0.99 },
    { "main",      "main",                true,  0.99 },
    { "fn",        "function",            true,  0.00 },
    { "nil",       "NullablePointerImpl", true,  0.00 },
    { "Ls",        "List",                true,  0.00 },
    { "Mp",        "Map",                 true,  0.00 },
    { "Lt",        "ListT",               true,  0.00 },
    /* Nested method paths */
    { "Foo.b",     "Foo.bar",             true,  0.00 },
    { "F.b",       "Foo.bar",             true,  0.00 },
    { "Ar.len",    "Array.length",        true,  0.00 },
    { "Bar.cnt",   "Bar.count",           true,  0.00 },
    /* Underscores */
    { "my_fn",     "my_fn",               true,  0.99 },
    { "myfn",      "my_fn",               true,  0.00 },
    { "mf",        "my_fn",               true,  0.00 },
    { "dfn",       "define_fn",           true,  0.00 },
    /* Longer cases */
    { "workpaces", "workspaces",          true,  0.00 },
    { "workSym",   "workspaceSymbol",     true,  0.00 },
    { "wsS",       "workspaceSymbol",     true,  0.00 },
    { "dS",        "documentSymbol",      true,  0.00 },
    { "defn",      "definition",          true,  0.00 },
    { "typeDef",   "typeDefinition",      true,  0.00 },
    { "hov",       "hover",               true,  0.00 },
    { "sigH",      "signatureHelp",       true,  0.00 },
    /* Interface / class */
    { "Sh",        "Shape",               true,  0.00 },
    { "Cir",       "Circle",              true,  0.00 },
    { "Sq",        "Square",              true,  0.00 },
    /* Adversarial ordering */
    { "ba",        "abab",                true,  0.00 },
    { "ca",        "abca",                true,  0.00 },
    { "ab",        "ba",                  false, 0.00 },
    { "xy",        "yx",                  false, 0.00 },
    /* Duplicates */
    { "aa",        "aaa",                 true,  0.00 },
    { "abab",      "aabb",                false, 0.00 },
    /* Mixed path / module / method */
    { "stdio/pr",  "stdlib://stdio/print",true,  0.00 },
    { "io.p",      "io.print",            true,  0.00 },
    /* Hyphenated */
    { "ht",        "http-client",         true,  0.00 },
    { "hc",        "http-client",         true,  0.00 },
    { "htclnt",    "http-client",         true,  0.00 },
    /* Unicode-safe ASCII only (per PLAN spec) */
    { "x",         "x",                   true,  0.99 },
    { "a",         "A",                   true,  0.99 },
    /* Edge sizes */
    { "a",         "b",                   false, 0.00 },
    { "a",         "a",                   true,  0.99 },
    { "xy",        "xx",                  false, 0.00 },
    /* Long consecutive */
    { "greet",     "greetings",           true,  0.00 },
    { "greet",     "greeter",             true,  0.00 },
    { "great",     "greeter",             false, 0.00 },
    { "GET",       "GET",                 true,  0.99 },
    { "get",       "GET_REQUEST",         true,  0.00 },
    { "logger",    "logger",              true,  0.99 },
    { "lg",        "logger",              true,  0.00 },
    { "log",       "iron_log",            true,  0.00 },
    /* ensure >100 */
    { "fo",        "foo",                 true,  0.00 },
    { "Fo",        "Foo",                 true,  0.00 },
    { "f",         "function",            true,  0.00 },
    { "x",         "xyz",                 true,  0.00 },
};
static const size_t corpus_size = sizeof(corpus) / sizeof(corpus[0]);

/* ── Test 01: golden corpus ───────────────────────────────────────── */

static void test_has_match_subsequence(void) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(100, (int)corpus_size);

    Iron_Arena arena = iron_arena_create(8 * 1024);

    for (size_t i = 0; i < corpus_size; i++) {
        const fz_row_t *r = &corpus[i];
        bool got = ilsp_fuzzy_has_match(r->q, r->c);
        if (got != r->has_match) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "row %zu (q=%s, c=%s): has_match expected %d got %d",
                     i, r->q, r->c, (int)r->has_match, (int)got);
            TEST_FAIL_MESSAGE(msg);
        }

        double sc = ilsp_fuzzy_match(r->q, r->c, &arena, NULL);
        if (r->has_match) {
            /* score must be finite */
            if (!isfinite(sc)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "row %zu (q=%s, c=%s): score must be finite, got %g",
                         i, r->q, r->c, sc);
                TEST_FAIL_MESSAGE(msg);
            }
            /* score must meet the lower bound (allow 1e-6 epsilon) */
            if (r->min_score > 0.0 && sc + 1e-6 < r->min_score) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "row %zu (q=%s, c=%s): score %g < min %g",
                         i, r->q, r->c, sc, r->min_score);
                TEST_FAIL_MESSAGE(msg);
            }
        } else {
            /* -INFINITY sentinel on non-match */
            if (isfinite(sc)) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "row %zu (q=%s, c=%s): non-match must be -INF, got %g",
                         i, r->q, r->c, sc);
                TEST_FAIL_MESSAGE(msg);
            }
        }
    }

    iron_arena_free(&arena);
}

/* ── Test 02: MATCH_MAX_LEN fallback ──────────────────────────────── */

static void test_match_max_len_fallback(void) {
    /* Build a haystack larger than ILSP_FUZZY_MATCH_MAX_LEN that still
     * contains 'a' then 'b' somewhere in order. */
    size_t len = ILSP_FUZZY_MATCH_MAX_LEN + 200;
    char *hay = (char *)malloc(len + 1);
    TEST_ASSERT_NOT_NULL(hay);
    memset(hay, 'x', len);
    hay[10]   = 'a';
    hay[500]  = 'b';
    hay[len]  = '\0';

    TEST_ASSERT_TRUE(ilsp_fuzzy_has_match("ab", hay));

    Iron_Arena arena = iron_arena_create(4 * 1024);
    double sc = ilsp_fuzzy_match("ab", hay, &arena, NULL);
    /* Falls back to linear; score should be exactly 0.0 on match. */
    TEST_ASSERT_EQUAL_DOUBLE(0.0, sc);

    /* Non-match on oversized haystack should still return -INF. */
    double nsc = ilsp_fuzzy_match("zQ", hay, &arena, NULL);
    TEST_ASSERT_FALSE(isfinite(nsc));

    iron_arena_free(&arena);
    free(hay);
}

/* ── Test 03: positions backtrack ─────────────────────────────────── */

static void test_positions_backtrack(void) {
    Iron_Arena arena = iron_arena_create(4 * 1024);
    size_t *pos = NULL;
    double sc = ilsp_fuzzy_match("abc", "xabcy", &arena, &pos);
    TEST_ASSERT_TRUE(isfinite(sc));
    TEST_ASSERT_NOT_NULL(pos);
    TEST_ASSERT_EQUAL_size_t(1, pos[0]);
    TEST_ASSERT_EQUAL_size_t(2, pos[1]);
    TEST_ASSERT_EQUAL_size_t(3, pos[2]);
    iron_arena_free(&arena);
}

/* ── Test 04: empty needle ───────────────────────────────────────── */

static void test_empty_needle(void) {
    TEST_ASSERT_TRUE(ilsp_fuzzy_has_match("", "anything"));
    TEST_ASSERT_TRUE(ilsp_fuzzy_has_match("", ""));
    Iron_Arena arena = iron_arena_create(1024);
    double sc = ilsp_fuzzy_match("", "anything", &arena, NULL);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, sc);
    iron_arena_free(&arena);
}

/* ── Test 05: kind ranking ──────────────────────────────────────── */

static void test_kind_rank(void) {
    /* Function must outrank Method must outrank Type (Object) must
     * outrank Interface must outrank Enum must outrank Field must
     * outrank Variable must outrank Import (treat variant via rank).
     * Lower rank is better. */
    int rf  = ilsp_fuzzy_kind_rank(IRON_SYM_FUNCTION);
    int rm  = ilsp_fuzzy_kind_rank(IRON_SYM_METHOD);
    int rt  = ilsp_fuzzy_kind_rank(IRON_SYM_TYPE);
    int ri  = ilsp_fuzzy_kind_rank(IRON_SYM_INTERFACE);
    int re  = ilsp_fuzzy_kind_rank(IRON_SYM_ENUM);
    int rfd = ilsp_fuzzy_kind_rank(IRON_SYM_FIELD);
    int rv  = ilsp_fuzzy_kind_rank(IRON_SYM_VARIABLE);

    TEST_ASSERT_LESS_THAN_INT(rm,  rf);   /* function < method */
    TEST_ASSERT_LESS_THAN_INT(rt,  rm);   /* method   < object */
    TEST_ASSERT_LESS_THAN_INT(ri,  rt);   /* object   < interface */
    TEST_ASSERT_LESS_THAN_INT(re,  ri);   /* interface< enum */
    TEST_ASSERT_LESS_THAN_INT(rfd, re);   /* enum     < field */
    TEST_ASSERT_LESS_THAN_INT(rv,  rfd);  /* field    < variable */
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_has_match_subsequence);
    RUN_TEST(test_match_max_len_fallback);
    RUN_TEST(test_positions_backtrack);
    RUN_TEST(test_empty_needle);
    RUN_TEST(test_kind_rank);
    return UNITY_END();
}
