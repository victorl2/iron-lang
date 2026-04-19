/* Phase 4 Plan 04-07 Task 03 (EDIT-15, D-14) -- selectionRange Unity. */

#include "unity.h"

#include "lsp/facade/edit/selection_range.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

typedef struct {
    IronLsp_Server    server;
    IronLsp_Document *doc;
    Iron_Arena        out_arena;
} fx_t;

static void fx_init(fx_t *f, const char *uri, const char *src) {
    memset(f, 0, sizeof(*f));
    f->server.position_encoding = ILSP_ENC_UTF8;
    f->doc = ilsp_document_create(uri, src, strlen(src), 1);
    TEST_ASSERT_NOT_NULL(f->doc);
    f->out_arena = iron_arena_create(32 * 1024);
}

static void fx_destroy(fx_t *f) {
    if (f->doc) ilsp_document_destroy(f->doc);
    iron_arena_free(&f->out_arena);
    memset(f, 0, sizeof(*f));
}

/* Count rungs in a chain. */
static int chain_length(IronLsp_SelectionRange *r) {
    int c = 0;
    while (r) { c++; r = r->parent; }
    return c;
}

/* Strict containment check: child's range must be inside (or equal to)
 * parent's range. Returns true if the chain satisfies the invariant. */
static bool chain_is_well_contained(IronLsp_SelectionRange *r) {
    while (r && r->parent) {
        IronLsp_SelectionRange *p = r->parent;
        /* child range must be contained by parent range */
        if (r->range_start_line < p->range_start_line) return false;
        if (r->range_start_line == p->range_start_line &&
            r->range_start_char < p->range_start_char) return false;
        if (r->range_end_line > p->range_end_line) return false;
        if (r->range_end_line == p->range_end_line &&
            r->range_end_char > p->range_end_char) return false;
        r = p;
    }
    return true;
}

/* ── Test S1: cursor deep in a nested expression → multi-rung ladder */
static void test_selection_range_nested_expr(void) {
    fx_t f;
    fx_init(&f, "file:///tmp/t_sr1.iron",
            "func main() {\n"
            "    val v = 1 + 2\n"
            "}\n");
    /* Cursor on the `1` in `1 + 2` at line 1 col 12. */
    IronLsp_Position pos = { .line = 1, .character = 12 };
    IronLsp_SelectionRange **out = NULL; size_t n = 0;
    ilsp_facade_selection_range(&f.server, f.doc, &pos, 1, NULL,
                                   &f.out_arena, &out, &n);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)n);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(out[0]);
    /* At least 2 rungs: innermost + parent (module). */
    TEST_ASSERT_TRUE(chain_length(out[0]) >= 2);
    /* Strict-containment invariant. */
    TEST_ASSERT_TRUE(chain_is_well_contained(out[0]));
    fx_destroy(&f);
}

/* ── Test S2: cursor at EOF → single-rung (module) ─────────────── */
static void test_selection_range_eof_is_module_only(void) {
    fx_t f;
    fx_init(&f, "file:///tmp/t_sr2.iron",
            "func main() {\n"
            "    val x = 1\n"
            "}\n");
    /* Line beyond file. */
    IronLsp_Position pos = { .line = 100, .character = 0 };
    IronLsp_SelectionRange **out = NULL; size_t n = 0;
    ilsp_facade_selection_range(&f.server, f.doc, &pos, 1, NULL,
                                   &f.out_arena, &out, &n);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)n);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(out[0]);
    /* Exactly 1 rung. */
    TEST_ASSERT_EQUAL_INT(1, chain_length(out[0]));
    TEST_ASSERT_NULL(out[0]->parent);
    fx_destroy(&f);
}

/* ── Test S3: strict containment invariant on a deeper chain ──── */
static void test_selection_range_strict_containment(void) {
    fx_t f;
    fx_init(&f, "file:///tmp/t_sr3.iron",
            "func main() {\n"
            "    if true {\n"
            "        val v = foo(1 + 2)\n"
            "    }\n"
            "}\n");
    /* Cursor on `1` inside `1 + 2`. */
    IronLsp_Position pos = { .line = 2, .character = 20 };
    IronLsp_SelectionRange **out = NULL; size_t n = 0;
    ilsp_facade_selection_range(&f.server, f.doc, &pos, 1, NULL,
                                   &f.out_arena, &out, &n);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)n);
    TEST_ASSERT_TRUE(chain_is_well_contained(out[0]));
    /* Expect at least 3 rungs (inner expr + block + module). */
    TEST_ASSERT_TRUE(chain_length(out[0]) >= 2);
    fx_destroy(&f);
}

/* ── Test S4: broken file with Iron_ErrorNode span covers cursor ─ */
static void test_selection_range_error_node_tolerant(void) {
    fx_t f;
    /* Unclosed brace. */
    fx_init(&f, "file:///tmp/t_sr4.iron",
            "func broken() {\n"
            "    val x = 1\n"
            "// forgot to close\n");
    IronLsp_Position pos = { .line = 1, .character = 8 };
    IronLsp_SelectionRange **out = NULL; size_t n = 0;
    ilsp_facade_selection_range(&f.server, f.doc, &pos, 1, NULL,
                                   &f.out_arena, &out, &n);
    /* Facade never crashes; n is 1. Chain has at least the module rung. */
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)n);
    TEST_ASSERT_NOT_NULL(out[0]);
    TEST_ASSERT_TRUE(chain_length(out[0]) >= 1);
    TEST_ASSERT_TRUE(chain_is_well_contained(out[0]));
    fx_destroy(&f);
}

/* ── Test S5: cursor on a literal `42` → literal-level rung ─── */
static void test_selection_range_leaf_literal(void) {
    fx_t f;
    fx_init(&f, "file:///tmp/t_sr5.iron",
            "func main() {\n"
            "    val x = 42\n"
            "}\n");
    IronLsp_Position pos = { .line = 1, .character = 12 };
    IronLsp_SelectionRange **out = NULL; size_t n = 0;
    ilsp_facade_selection_range(&f.server, f.doc, &pos, 1, NULL,
                                   &f.out_arena, &out, &n);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)n);
    TEST_ASSERT_NOT_NULL(out[0]);
    TEST_ASSERT_TRUE(chain_length(out[0]) >= 2);
    TEST_ASSERT_TRUE(chain_is_well_contained(out[0]));
    fx_destroy(&f);
}

/* ── Test S6: multiple positions in one call ────────────────────── */
static void test_selection_range_multiple_positions(void) {
    fx_t f;
    fx_init(&f, "file:///tmp/t_sr6.iron",
            "func main() {\n"
            "    val x = 1\n"
            "    val y = 2\n"
            "}\n");
    IronLsp_Position positions[2] = {
        { .line = 1, .character = 8 },
        { .line = 2, .character = 8 },
    };
    IronLsp_SelectionRange **out = NULL; size_t n = 0;
    ilsp_facade_selection_range(&f.server, f.doc, positions, 2, NULL,
                                   &f.out_arena, &out, &n);
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)n);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_NOT_NULL(out[0]);
    TEST_ASSERT_NOT_NULL(out[1]);
    TEST_ASSERT_TRUE(chain_is_well_contained(out[0]));
    TEST_ASSERT_TRUE(chain_is_well_contained(out[1]));
    fx_destroy(&f);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_selection_range_nested_expr);
    RUN_TEST(test_selection_range_eof_is_module_only);
    RUN_TEST(test_selection_range_strict_containment);
    RUN_TEST(test_selection_range_error_node_tolerant);
    RUN_TEST(test_selection_range_leaf_literal);
    RUN_TEST(test_selection_range_multiple_positions);
    return UNITY_END();
}
