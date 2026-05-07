/* Phase 18 Wave 0 (Plan 02): regression-anchor (NOT a Wave 0 RED gate)
 * for PARM-04 (method receiver tier).
 *
 * Verifies the EXISTING Phase 84 receiver-tier behavior that Plan 18-02
 * leaves intentionally untouched:
 *   - Default-mutable receiver: in-object `func name() { self.field = ... }`
 *     analyzes cleanly with zero PARM/readonly diagnostics.
 *   - Readonly opt-out: `readonly func name() { self.field = ... }` emits
 *     IRON_ERR_READONLY_WRITE_SELF=238 (Phase 84 dedicated code).
 *
 * Lock for the CONTEXT.md amendment (researcher OQ-3): code 238 stays
 * dedicated to readonly self-writes; Plan 18-02 does NOT re-route to
 * IRON_ERR_VAL_FIELD_REASSIGN=265 even though CONTEXT.md originally
 * proposed reuse-with-different-hint. test_parm_04_amendment_e238_dedicated
 * locks COUNT(IRON_ERR_READONLY_WRITE_SELF) >= 1 so any future drift
 * back to 265 fails this regression-anchor.
 *
 * NOTE: regression-anchor (NOT a Wave 0 RED gate). All 3 RUN_TESTs are
 * expected to PASS on first run — Plan 18-02 changes do not touch the
 * Phase 84 readonly enforcement code path. Mirrors Phase 17 Plan 03
 * convention.
 *
 * Pattern source: tests/unit/test_analyzer_parm_read_only.c (Plan 18-01)
 * + tests/unit/test_typecheck.c test_readonly_writes_self_emits_E0238
 * (Phase 84). */
#include "unity.h"
#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "stb_ds.h"

#include <stdatomic.h>
#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

/* Lex + parse + analyze via the production buffer entry point. */
static void analyze_src(const char *src) {
    Iron_AnalyzeResult r = iron_analyze_buffer(
        src, strlen(src), "test.iron",
        IRON_ANALYSIS_MODE_CLI,
        &arena, &diags, NULL,
        0);
    (void)r;
}

static int count_with_code(int target) {
    int n = 0;
    for (int i = 0; i < arrlen(diags.items); i++) {
        if (diags.items[i].code == target) n++;
    }
    return n;
}

/* PARM-04 — default-mutable receiver: in-object `func name() {}` writing
 * self.field analyzes cleanly. No PARM-related diagnostic, no readonly
 * diagnostic — the receiver is implicitly mutable per spec §5.4. */
void test_parm_04_default_mutable_self_write_accepts(void) {
    analyze_src(
        "object Counter {\n"
        "    var count: Int\n"
        "    init() { self.count = 0 }\n"
        "    func tick() {\n"
        "        self.count = self.count + 1\n"
        "    }\n"
        "}\n"
        "func main() {\n"
        "    var c = Counter()\n"
        "    c.tick()\n"
        "}\n");
    /* No PARM-01 mutation (default-mutable receiver), no PARM-03 (no
     * read-only arg passed to var slot here), no Phase 84 readonly-self
     * write (the method has no readonly modifier), no VAL-03 field
     * reassign (var field). Plan 18-02 does NOT introduce E0267 in this
     * fixture either — but THIS regression-anchor file builds without
     * E0267 in scope (Plan 18-02 Task 2 adds the symbol). */
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_PARM_READ_ONLY));
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_READONLY_WRITE_SELF));
    TEST_ASSERT_EQUAL_INT(0, count_with_code(IRON_ERR_VAL_FIELD_REASSIGN));
}

/* PARM-04 — readonly opt-out: `readonly func name() {}` writing self.field
 * emits IRON_ERR_READONLY_WRITE_SELF=238 (Phase 84 dedicated code).
 * Plan 18-02 does NOT change this — the existing enforcement is the
 * PARM-04 implementation. */
void test_parm_04_readonly_self_write_rejected(void) {
    analyze_src(
        "object Counter {\n"
        "    var count: Int\n"
        "    init() { self.count = 0 }\n"
        "    readonly func bump() {\n"
        "        self.count = self.count + 1\n"
        "    }\n"
        "}\n"
        "func main() {\n"
        "    var c = Counter()\n"
        "    c.bump()\n"
        "}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_ERR_READONLY_WRITE_SELF));
}

/* CONTEXT.md amendment lock: code 238 stays dedicated to readonly
 * self-writes. Plan 18-02 deviates from CONTEXT.md "reuse 265 with
 * different hint" decision per researcher OQ-3 — KEEP E0238.
 *
 * Asserts COUNT(IRON_ERR_READONLY_WRITE_SELF) >= 1 on the same fixture as
 * the previous test. If a future plan re-routes the readonly self-write
 * path to IRON_ERR_VAL_FIELD_REASSIGN=265 (or any other code), this
 * COUNT-238 assertion drops to 0 and the regression fails loudly. */
void test_parm_04_amendment_e238_dedicated(void) {
    analyze_src(
        "object Counter {\n"
        "    var count: Int\n"
        "    init() { self.count = 0 }\n"
        "    readonly func bump() {\n"
        "        self.count = self.count + 1\n"
        "    }\n"
        "}\n"
        "func main() {\n"
        "    var c = Counter()\n"
        "    c.bump()\n"
        "}\n");
    /* Code 238 fires (dedicated readonly code, NOT re-routed to 265). */
    TEST_ASSERT_GREATER_THAN_INT(0, count_with_code(IRON_ERR_READONLY_WRITE_SELF));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parm_04_default_mutable_self_write_accepts);
    RUN_TEST(test_parm_04_readonly_self_write_rejected);
    RUN_TEST(test_parm_04_amendment_e238_dedicated);
    return UNITY_END();
}
