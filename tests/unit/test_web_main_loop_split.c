/* test_web_main_loop_split.c — Unity tests for the LIR web main-loop split pass.
 *
 * Tests cover WEB-EMIT-01..04:
 *   - Canonical while(!WindowShouldClose()) shape is detected; captures populated
 *   - Multiple top-level canonical loops => IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS (700)
 *   - Nested canonical loop               => IRON_ERR_WEB_NESTED_MAIN_LOOP    (702)
 *   - Non-canonical loop condition        => IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP (701)
 *   - --target=native                     => pass is a zero-cost no-op
 *   - No InitWindow call                  => pass is silent no-op
 *
 * All fixtures are hand-built using the LIR constructor API (no parser).
 * Pattern follows tests/lir/test_lir_optimize.c.
 *
 * NOTE: iron_types_init() is called once per test in setUp() because some
 * tests create their own ir_arena and the module arena passed to the pass
 * must be separate from the ir_arena that owns the module (as in build.c).
 */

#include "unity.h"
#include "lir/lir.h"
#include "lir/web_main_loop_split.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "cli/build.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;    /* "build.c-equivalent" pipeline arena */
static Iron_DiagList g_diags;

void setUp(void) {
    g_arena = iron_arena_create(1024 * 512);
    g_diags = iron_diaglist_create();
    iron_types_init(&g_arena);
}

void tearDown(void) {
    iron_arena_free(&g_arena);
    iron_diaglist_free(&g_diags);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static bool has_code(int code) {
    for (int i = 0; i < g_diags.count; i++) {
        if (g_diags.items[i].code == code) return true;
    }
    return false;
}

static Iron_Span ts(void) {
    return iron_span_make("test.iron", 1, 1, 1, 2);
}

/* Build a minimal Iron_FuncDecl stub with only `name` set.
 * The pass reads only func_decl->name in ws_is_call_to(); the rest can be zeroed. */
static Iron_FuncDecl *make_decl(Iron_Arena *a, const char *name) {
    Iron_FuncDecl *d = ARENA_ALLOC(a, Iron_FuncDecl);
    memset(d, 0, sizeof(*d));
    d->name      = name;
    d->is_extern = true;
    return d;
}

/* ── Fixture builder: canonical main loop function ───────────────────────── *
 *
 * Builds the following CFG in the returned IronLIR_Func:
 *
 *   entry:
 *     %1 = alloca Int  ("x")        <- outer-scope local to be captured
 *     %2 = const_int 0              <- initialiser
 *     store %1, %2
 *     call InitWindow(...)           <- marks function as init-window function
 *     jump loop_header
 *
 *   loop_header:
 *     %3 = call WindowShouldClose()
 *     %4 = not %3
 *     branch %4, loop_body, loop_exit
 *
 *   loop_body:
 *     %5 = load %1                  <- reads outer-scope "x" => captured
 *     %6 = const_int 1
 *     %7 = add %5, %6
 *     store %1, %7                  <- writes outer-scope "x" => is_mutable=true
 *     jump loop_header
 *
 *   loop_exit:
 *     return void
 *
 * After iron_lir_web_main_loop_split(..., IRON_TARGET_WEB):
 *   fn->web_frame_capture_count == 1
 *   fn->web_frame_captures[0].name == "x"
 *   fn->web_frame_captures[0].is_mutable == true
 */
static IronLIR_Func *build_canonical_fn(IronLIR_Module *mod) {
    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_Span sp = ts();

    Iron_FuncDecl *init_decl = make_decl(mod->arena, "InitWindow");
    Iron_FuncDecl *wsc_decl  = make_decl(mod->arena, "WindowShouldClose");

    IronLIR_Func  *fn   = iron_lir_func_create(mod, "Iron_main", NULL, 0, void_type);
    IronLIR_Block *entry      = iron_lir_block_create(fn, "entry");
    IronLIR_Block *loop_hdr   = iron_lir_block_create(fn, "loop_header");
    IronLIR_Block *loop_body  = iron_lir_block_create(fn, "loop_body");
    IronLIR_Block *loop_exit  = iron_lir_block_create(fn, "loop_exit");

    /* entry: alloca x, init to 0, call InitWindow, jump to header */
    IronLIR_Instr *x_slot = iron_lir_alloca(fn, entry, int_type, "x", sp);
    IronLIR_Instr *zero   = iron_lir_const_int(fn, entry, 0, int_type, sp);
    iron_lir_store(fn, entry, x_slot->id, zero->id, sp);

    /* InitWindow call — 3 args, use const_int 0 as width/height, const_null as title */
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    IronLIR_Instr *width   = iron_lir_const_int(fn, entry, 800,  int_type, sp);
    IronLIR_Instr *height  = iron_lir_const_int(fn, entry, 600,  int_type, sp);
    IronLIR_Instr *title   = iron_lir_const_null(fn, entry, null_type, sp);
    IronLIR_ValueId init_args[3] = { width->id, height->id, title->id };
    iron_lir_call(fn, entry, init_decl, IRON_LIR_VALUE_INVALID,
                  init_args, 3, void_type, sp);
    iron_lir_jump(fn, entry, loop_hdr->id, sp);

    /* loop_header: cond = !WindowShouldClose(); branch cond, body, exit */
    IronLIR_Instr *wsc = iron_lir_call(fn, loop_hdr, wsc_decl, IRON_LIR_VALUE_INVALID,
                                       NULL, 0, bool_type, sp);
    IronLIR_Instr *neg = iron_lir_unop(fn, loop_hdr, IRON_LIR_NOT, wsc->id, bool_type, sp);
    iron_lir_branch(fn, loop_hdr, neg->id, loop_body->id, loop_exit->id, sp);

    /* loop_body: x++; jump back to header */
    IronLIR_Instr *loaded = iron_lir_load(fn, loop_body, x_slot->id, int_type, sp);
    IronLIR_Instr *one    = iron_lir_const_int(fn, loop_body, 1, int_type, sp);
    IronLIR_Instr *added  = iron_lir_binop(fn, loop_body, IRON_LIR_ADD,
                                           loaded->id, one->id, int_type, sp);
    iron_lir_store(fn, loop_body, x_slot->id, added->id, sp);
    iron_lir_jump(fn, loop_body, loop_hdr->id, sp);

    /* loop_exit: return void */
    iron_lir_return(fn, loop_exit, IRON_LIR_VALUE_INVALID, true, void_type, sp);

    /* Wire CFG edges (preds/succs) manually — the pass uses them for dominators */
    arrput(entry->succs,     loop_hdr->id);
    arrput(loop_hdr->preds,  entry->id);
    arrput(loop_hdr->succs,  loop_body->id);
    arrput(loop_hdr->succs,  loop_exit->id);
    arrput(loop_body->preds, loop_hdr->id);
    arrput(loop_body->succs, loop_hdr->id);   /* back edge: body -> header */
    arrput(loop_hdr->preds,  loop_body->id);  /* header gets body as pred too */
    arrput(loop_exit->preds, loop_hdr->id);

    return fn;
}

/* ── Test 1: Canonical shape — captures populated ────────────────────────── */

void test_canonical_shape_populates_captures(void) {
    /* Module with one function: canonical while(!WindowShouldClose()) that
     * reads/writes outer-scope local "x". Pass must populate web_frame_captures.
     */
    Iron_Arena ir_arena = iron_arena_create(1024 * 512);
    iron_types_init(&ir_arena);
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_canonical");

    IronLIR_Func *fn = build_canonical_fn(mod);

    iron_lir_web_main_loop_split(mod, &g_arena, &g_diags, IRON_TARGET_WEB);

    /* No errors */
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* Exactly one capture: "x", mutable */
    TEST_ASSERT_NOT_NULL(fn->web_frame_captures);
    TEST_ASSERT_EQUAL_INT(1, fn->web_frame_capture_count);
    TEST_ASSERT_EQUAL_STRING("x", fn->web_frame_captures[0].name);
    TEST_ASSERT_TRUE(fn->web_frame_captures[0].is_mutable);

    /* Existing capture_metadata is NOT touched (must stay NULL for normal fns) */
    TEST_ASSERT_NULL(fn->capture_metadata);
    TEST_ASSERT_EQUAL_INT(0, fn->capture_count);

    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 2: Multiple canonical loops => IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS ── */

void test_multiple_canonical_loops_emits_error(void) {
    /* Module with one function containing TWO top-level canonical loops.
     * The pass must emit IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS (700). */
    Iron_Arena ir_arena = iron_arena_create(1024 * 512);
    iron_types_init(&ir_arena);
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_multi");

    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Span sp = ts();

    Iron_FuncDecl *init_decl = make_decl(&ir_arena, "InitWindow");
    Iron_FuncDecl *wsc_decl  = make_decl(&ir_arena, "WindowShouldClose");

    /* Build a function with:
     *   entry -> init_window call, jump loop1_hdr
     *   loop1_hdr: cond = !WSC(); branch -> loop1_body / between
     *   loop1_body: jump -> loop1_hdr    (back edge)
     *   between: jump -> loop2_hdr
     *   loop2_hdr: cond = !WSC(); branch -> loop2_body / exit
     *   loop2_body: jump -> loop2_hdr    (back edge)
     *   exit: return
     */
    IronLIR_Func  *fn         = iron_lir_func_create(mod, "Iron_main", NULL, 0, void_type);
    IronLIR_Block *entry      = iron_lir_block_create(fn, "entry");
    IronLIR_Block *loop1_hdr  = iron_lir_block_create(fn, "loop1_hdr");
    IronLIR_Block *loop1_body = iron_lir_block_create(fn, "loop1_body");
    IronLIR_Block *between    = iron_lir_block_create(fn, "between");
    IronLIR_Block *loop2_hdr  = iron_lir_block_create(fn, "loop2_hdr");
    IronLIR_Block *loop2_body = iron_lir_block_create(fn, "loop2_body");
    IronLIR_Block *exit_blk   = iron_lir_block_create(fn, "exit");

    /* entry: call InitWindow, jump to loop1 */
    IronLIR_Instr *w1 = iron_lir_const_int(fn, entry, 800, int_type, sp);
    IronLIR_Instr *h1 = iron_lir_const_int(fn, entry, 600, int_type, sp);
    IronLIR_Instr *t1 = iron_lir_const_null(fn, entry, null_type, sp);
    IronLIR_ValueId init_args[3] = { w1->id, h1->id, t1->id };
    iron_lir_call(fn, entry, init_decl, IRON_LIR_VALUE_INVALID,
                  init_args, 3, void_type, sp);
    iron_lir_jump(fn, entry, loop1_hdr->id, sp);

    /* loop1_hdr */
    IronLIR_Instr *wsc1 = iron_lir_call(fn, loop1_hdr, wsc_decl, IRON_LIR_VALUE_INVALID,
                                        NULL, 0, bool_type, sp);
    IronLIR_Instr *neg1 = iron_lir_unop(fn, loop1_hdr, IRON_LIR_NOT, wsc1->id, bool_type, sp);
    iron_lir_branch(fn, loop1_hdr, neg1->id, loop1_body->id, between->id, sp);

    /* loop1_body: back edge to loop1_hdr */
    iron_lir_jump(fn, loop1_body, loop1_hdr->id, sp);

    /* between: jump to loop2 */
    iron_lir_jump(fn, between, loop2_hdr->id, sp);

    /* loop2_hdr */
    IronLIR_Instr *wsc2 = iron_lir_call(fn, loop2_hdr, wsc_decl, IRON_LIR_VALUE_INVALID,
                                        NULL, 0, bool_type, sp);
    IronLIR_Instr *neg2 = iron_lir_unop(fn, loop2_hdr, IRON_LIR_NOT, wsc2->id, bool_type, sp);
    iron_lir_branch(fn, loop2_hdr, neg2->id, loop2_body->id, exit_blk->id, sp);

    /* loop2_body: back edge to loop2_hdr */
    iron_lir_jump(fn, loop2_body, loop2_hdr->id, sp);

    /* exit */
    iron_lir_return(fn, exit_blk, IRON_LIR_VALUE_INVALID, true, void_type, sp);

    /* Wire CFG edges */
    arrput(entry->succs,       loop1_hdr->id);
    arrput(loop1_hdr->preds,   entry->id);
    arrput(loop1_hdr->succs,   loop1_body->id);
    arrput(loop1_hdr->succs,   between->id);
    arrput(loop1_body->preds,  loop1_hdr->id);
    arrput(loop1_body->succs,  loop1_hdr->id);
    arrput(loop1_hdr->preds,   loop1_body->id);
    arrput(between->preds,     loop1_hdr->id);
    arrput(between->succs,     loop2_hdr->id);
    arrput(loop2_hdr->preds,   between->id);
    arrput(loop2_hdr->succs,   loop2_body->id);
    arrput(loop2_hdr->succs,   exit_blk->id);
    arrput(loop2_body->preds,  loop2_hdr->id);
    arrput(loop2_body->succs,  loop2_hdr->id);
    arrput(loop2_hdr->preds,   loop2_body->id);
    arrput(exit_blk->preds,    loop2_hdr->id);

    iron_lir_web_main_loop_split(mod, &g_arena, &g_diags, IRON_TARGET_WEB);

    TEST_ASSERT_TRUE(g_diags.error_count >= 1);
    TEST_ASSERT_TRUE(has_code(IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS));

    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 3: Nested canonical loop => IRON_ERR_WEB_NESTED_MAIN_LOOP ──────── */

void test_nested_canonical_loop_emits_error(void) {
    /* Module with one function containing a canonical loop NESTED inside an
     * outer non-canonical loop.
     *
     * CFG:
     *   entry -> outer_hdr
     *   outer_hdr: cond = const_bool true; branch -> outer_body / exit    (non-canonical)
     *   outer_body: jump -> inner_hdr
     *   inner_hdr: cond = !WSC(); branch -> inner_body / outer_body_cont  (canonical, nested)
     *   inner_body: jump -> inner_hdr  (back edge — makes inner_hdr a loop header)
     *   outer_body_cont: jump -> outer_hdr  (back edge — makes outer_hdr a loop header too)
     *   exit: return
     *
     * The inner loop (canonical) is nested inside the outer loop.
     * The outer loop header (outer_hdr) does NOT use WindowShouldClose.
     *
     * This function also calls InitWindow so it enters the pass's main logic.
     */
    Iron_Arena ir_arena = iron_arena_create(1024 * 512);
    iron_types_init(&ir_arena);
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_nested");

    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Span sp = ts();

    Iron_FuncDecl *init_decl = make_decl(&ir_arena, "InitWindow");
    Iron_FuncDecl *wsc_decl  = make_decl(&ir_arena, "WindowShouldClose");

    IronLIR_Func  *fn         = iron_lir_func_create(mod, "Iron_main", NULL, 0, void_type);
    IronLIR_Block *entry      = iron_lir_block_create(fn, "entry");
    IronLIR_Block *outer_hdr  = iron_lir_block_create(fn, "outer_hdr");
    IronLIR_Block *outer_body = iron_lir_block_create(fn, "outer_body");
    IronLIR_Block *inner_hdr  = iron_lir_block_create(fn, "inner_hdr");
    IronLIR_Block *inner_body = iron_lir_block_create(fn, "inner_body");
    IronLIR_Block *outer_cont = iron_lir_block_create(fn, "outer_cont");
    IronLIR_Block *exit_blk   = iron_lir_block_create(fn, "exit");

    /* entry: call InitWindow, jump to outer_hdr */
    IronLIR_Instr *w1 = iron_lir_const_int(fn, entry, 800, int_type, sp);
    IronLIR_Instr *h1 = iron_lir_const_int(fn, entry, 600, int_type, sp);
    IronLIR_Instr *t1 = iron_lir_const_null(fn, entry, null_type, sp);
    IronLIR_ValueId init_args[3] = { w1->id, h1->id, t1->id };
    iron_lir_call(fn, entry, init_decl, IRON_LIR_VALUE_INVALID,
                  init_args, 3, void_type, sp);
    iron_lir_jump(fn, entry, outer_hdr->id, sp);

    /* outer_hdr: use const_bool as condition (non-canonical outer loop) */
    IronLIR_Instr *outer_cond = iron_lir_const_bool(fn, outer_hdr, true, bool_type, sp);
    iron_lir_branch(fn, outer_hdr, outer_cond->id, outer_body->id, exit_blk->id, sp);

    /* outer_body: jump to inner_hdr */
    iron_lir_jump(fn, outer_body, inner_hdr->id, sp);

    /* inner_hdr: canonical !WSC() condition — but it's nested inside outer loop */
    IronLIR_Instr *wsc_i = iron_lir_call(fn, inner_hdr, wsc_decl, IRON_LIR_VALUE_INVALID,
                                         NULL, 0, bool_type, sp);
    IronLIR_Instr *neg_i = iron_lir_unop(fn, inner_hdr, IRON_LIR_NOT, wsc_i->id, bool_type, sp);
    iron_lir_branch(fn, inner_hdr, neg_i->id, inner_body->id, outer_cont->id, sp);

    /* inner_body: back edge to inner_hdr */
    iron_lir_jump(fn, inner_body, inner_hdr->id, sp);

    /* outer_cont: back edge to outer_hdr */
    iron_lir_jump(fn, outer_cont, outer_hdr->id, sp);

    /* exit */
    iron_lir_return(fn, exit_blk, IRON_LIR_VALUE_INVALID, true, void_type, sp);

    /* Wire CFG edges */
    arrput(entry->succs,      outer_hdr->id);
    arrput(outer_hdr->preds,  entry->id);
    arrput(outer_hdr->succs,  outer_body->id);
    arrput(outer_hdr->succs,  exit_blk->id);
    arrput(outer_body->preds, outer_hdr->id);
    arrput(outer_body->succs, inner_hdr->id);
    arrput(inner_hdr->preds,  outer_body->id);
    arrput(inner_hdr->succs,  inner_body->id);
    arrput(inner_hdr->succs,  outer_cont->id);
    arrput(inner_body->preds, inner_hdr->id);
    arrput(inner_body->succs, inner_hdr->id);
    arrput(inner_hdr->preds,  inner_body->id);  /* back edge pred */
    arrput(outer_cont->preds, inner_hdr->id);
    arrput(outer_cont->succs, outer_hdr->id);
    arrput(outer_hdr->preds,  outer_cont->id);  /* back edge pred */
    arrput(exit_blk->preds,   outer_hdr->id);

    iron_lir_web_main_loop_split(mod, &g_arena, &g_diags, IRON_TARGET_WEB);

    TEST_ASSERT_TRUE(g_diags.error_count >= 1);
    TEST_ASSERT_TRUE(has_code(IRON_ERR_WEB_NESTED_MAIN_LOOP));

    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 4: Non-canonical loop condition => IRON_ERR_WEB_NON_CANONICAL ──── */

void test_non_canonical_condition_emits_error(void) {
    /* Function with InitWindow + a while loop whose header condition is a
     * simple const_bool (not the !WindowShouldClose() shape).
     * Pass must emit IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP (701). */
    Iron_Arena ir_arena = iron_arena_create(1024 * 512);
    iron_types_init(&ir_arena);
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_non_canonical");

    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Span sp = ts();

    Iron_FuncDecl *init_decl = make_decl(&ir_arena, "InitWindow");

    IronLIR_Func  *fn       = iron_lir_func_create(mod, "Iron_main", NULL, 0, void_type);
    IronLIR_Block *entry    = iron_lir_block_create(fn, "entry");
    IronLIR_Block *loop_hdr = iron_lir_block_create(fn, "loop_hdr");
    IronLIR_Block *loop_bod = iron_lir_block_create(fn, "loop_body");
    IronLIR_Block *exit_blk = iron_lir_block_create(fn, "exit");

    /* entry: call InitWindow, jump to loop_hdr */
    IronLIR_Instr *w = iron_lir_const_int(fn, entry, 800, int_type, sp);
    IronLIR_Instr *h = iron_lir_const_int(fn, entry, 600, int_type, sp);
    IronLIR_Instr *t = iron_lir_const_null(fn, entry, null_type, sp);
    IronLIR_ValueId init_args[3] = { w->id, h->id, t->id };
    iron_lir_call(fn, entry, init_decl, IRON_LIR_VALUE_INVALID,
                  init_args, 3, void_type, sp);
    iron_lir_jump(fn, entry, loop_hdr->id, sp);

    /* loop_hdr: condition is const_bool true (not the canonical shape) */
    IronLIR_Instr *cond = iron_lir_const_bool(fn, loop_hdr, true, bool_type, sp);
    iron_lir_branch(fn, loop_hdr, cond->id, loop_bod->id, exit_blk->id, sp);

    /* loop_body: back edge */
    iron_lir_jump(fn, loop_bod, loop_hdr->id, sp);

    /* exit: return */
    iron_lir_return(fn, exit_blk, IRON_LIR_VALUE_INVALID, true, void_type, sp);

    /* Wire CFG edges */
    arrput(entry->succs,    loop_hdr->id);
    arrput(loop_hdr->preds, entry->id);
    arrput(loop_hdr->succs, loop_bod->id);
    arrput(loop_hdr->succs, exit_blk->id);
    arrput(loop_bod->preds, loop_hdr->id);
    arrput(loop_bod->succs, loop_hdr->id);
    arrput(loop_hdr->preds, loop_bod->id);
    arrput(exit_blk->preds, loop_hdr->id);

    iron_lir_web_main_loop_split(mod, &g_arena, &g_diags, IRON_TARGET_WEB);

    TEST_ASSERT_TRUE(g_diags.error_count >= 1);
    TEST_ASSERT_TRUE(has_code(IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP));

    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 5: Native target is a zero-cost no-op ──────────────────────────── */

void test_native_target_is_noop(void) {
    /* Canonical fixture, but invoked with IRON_TARGET_NATIVE.
     * Pass must return immediately without touching the IR. */
    Iron_Arena ir_arena = iron_arena_create(1024 * 512);
    iron_types_init(&ir_arena);
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_native_noop");

    IronLIR_Func *fn = build_canonical_fn(mod);

    iron_lir_web_main_loop_split(mod, &g_arena, &g_diags, IRON_TARGET_NATIVE);

    /* No errors emitted */
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_EQUAL_INT(0, g_diags.count);

    /* No captures populated — pass is a no-op */
    TEST_ASSERT_NULL(fn->web_frame_captures);
    TEST_ASSERT_EQUAL_INT(0, fn->web_frame_capture_count);

    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 6: Function without InitWindow is silent no-op ─────────────────── */

void test_no_init_window_is_noop(void) {
    /* A function with a canonical while(!WindowShouldClose()) loop but NO
     * InitWindow call. Pass must be a silent no-op (no captures, no error).
     *
     * NOTE: the pass also checks for "orphan canonical loops in non-InitWindow
     * functions" and emits IRON_ERR_WEB_MAIN_LOOP_WRONG_FUNCTION. This test
     * creates a function with a WindowShouldClose call but the loop header does
     * NOT use the canonical NOT(CALL(WSC)) shape — it uses a const_bool.
     * This avoids the WRONG_FUNCTION path and strictly tests the
     * "no InitWindow => skip ws_process_function" path.
     */
    Iron_Arena ir_arena = iron_arena_create(1024 * 512);
    iron_types_init(&ir_arena);
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_no_init_window");

    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_Span sp = ts();

    IronLIR_Func  *fn       = iron_lir_func_create(mod, "helper", NULL, 0, void_type);
    IronLIR_Block *entry    = iron_lir_block_create(fn, "entry");
    IronLIR_Block *loop_hdr = iron_lir_block_create(fn, "loop_hdr");
    IronLIR_Block *loop_bod = iron_lir_block_create(fn, "loop_body");
    IronLIR_Block *exit_blk = iron_lir_block_create(fn, "exit");

    /* entry: const_bool, jump to loop_hdr (no InitWindow call) */
    IronLIR_Instr *cond_entry = iron_lir_const_bool(fn, entry, true, bool_type, sp);
    (void)cond_entry;
    iron_lir_jump(fn, entry, loop_hdr->id, sp);

    /* loop_hdr: const_bool condition (not canonical) */
    IronLIR_Instr *cond = iron_lir_const_bool(fn, loop_hdr, true, bool_type, sp);
    iron_lir_branch(fn, loop_hdr, cond->id, loop_bod->id, exit_blk->id, sp);

    /* loop_body: back edge */
    iron_lir_jump(fn, loop_bod, loop_hdr->id, sp);

    /* exit */
    iron_lir_return(fn, exit_blk, IRON_LIR_VALUE_INVALID, true, void_type, sp);

    /* Wire CFG edges */
    arrput(entry->succs,    loop_hdr->id);
    arrput(loop_hdr->preds, entry->id);
    arrput(loop_hdr->succs, loop_bod->id);
    arrput(loop_hdr->succs, exit_blk->id);
    arrput(loop_bod->preds, loop_hdr->id);
    arrput(loop_bod->succs, loop_hdr->id);
    arrput(loop_hdr->preds, loop_bod->id);
    arrput(exit_blk->preds, loop_hdr->id);

    iron_lir_web_main_loop_split(mod, &g_arena, &g_diags, IRON_TARGET_WEB);

    /* No errors — no InitWindow in the function */
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);

    /* No captures */
    TEST_ASSERT_NULL(fn->web_frame_captures);
    TEST_ASSERT_EQUAL_INT(0, fn->web_frame_capture_count);

    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Test 7: Capture name and mutability detail ──────────────────────────── */

void test_canonical_read_only_capture_not_mutable(void) {
    /* Function with InitWindow + canonical loop that only READS (no write)
     * an outer-scope local "y". is_mutable must be false. */
    Iron_Arena ir_arena = iron_arena_create(1024 * 512);
    iron_types_init(&ir_arena);
    IronLIR_Module *mod = iron_lir_module_create(&ir_arena, "test_readonly_capture");

    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Span sp = ts();

    Iron_FuncDecl *init_decl = make_decl(&ir_arena, "InitWindow");
    Iron_FuncDecl *wsc_decl  = make_decl(&ir_arena, "WindowShouldClose");

    IronLIR_Func  *fn        = iron_lir_func_create(mod, "Iron_main", NULL, 0, void_type);
    IronLIR_Block *entry     = iron_lir_block_create(fn, "entry");
    IronLIR_Block *loop_hdr  = iron_lir_block_create(fn, "loop_hdr");
    IronLIR_Block *loop_body = iron_lir_block_create(fn, "loop_body");
    IronLIR_Block *exit_blk  = iron_lir_block_create(fn, "exit");

    /* entry: alloca y, init to 42, call InitWindow, jump to loop_hdr */
    IronLIR_Instr *y_slot = iron_lir_alloca(fn, entry, int_type, "y", sp);
    IronLIR_Instr *c42    = iron_lir_const_int(fn, entry, 42, int_type, sp);
    iron_lir_store(fn, entry, y_slot->id, c42->id, sp);

    IronLIR_Instr *w = iron_lir_const_int(fn, entry, 800, int_type, sp);
    IronLIR_Instr *h = iron_lir_const_int(fn, entry, 600, int_type, sp);
    IronLIR_Instr *t = iron_lir_const_null(fn, entry, null_type, sp);
    IronLIR_ValueId init_args[3] = { w->id, h->id, t->id };
    iron_lir_call(fn, entry, init_decl, IRON_LIR_VALUE_INVALID,
                  init_args, 3, void_type, sp);
    iron_lir_jump(fn, entry, loop_hdr->id, sp);

    /* loop_hdr: canonical condition */
    IronLIR_Instr *wsc = iron_lir_call(fn, loop_hdr, wsc_decl, IRON_LIR_VALUE_INVALID,
                                       NULL, 0, bool_type, sp);
    IronLIR_Instr *neg = iron_lir_unop(fn, loop_hdr, IRON_LIR_NOT, wsc->id, bool_type, sp);
    iron_lir_branch(fn, loop_hdr, neg->id, loop_body->id, exit_blk->id, sp);

    /* loop_body: only LOAD y (no STORE => not mutable) */
    IronLIR_Instr *loaded = iron_lir_load(fn, loop_body, y_slot->id, int_type, sp);
    (void)loaded; /* just read, nothing written */
    iron_lir_jump(fn, loop_body, loop_hdr->id, sp);

    /* exit */
    iron_lir_return(fn, exit_blk, IRON_LIR_VALUE_INVALID, true, void_type, sp);

    /* Wire CFG */
    arrput(entry->succs,     loop_hdr->id);
    arrput(loop_hdr->preds,  entry->id);
    arrput(loop_hdr->succs,  loop_body->id);
    arrput(loop_hdr->succs,  exit_blk->id);
    arrput(loop_body->preds, loop_hdr->id);
    arrput(loop_body->succs, loop_hdr->id);
    arrput(loop_hdr->preds,  loop_body->id);
    arrput(exit_blk->preds,  loop_hdr->id);

    iron_lir_web_main_loop_split(mod, &g_arena, &g_diags, IRON_TARGET_WEB);

    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_NOT_NULL(fn->web_frame_captures);
    TEST_ASSERT_EQUAL_INT(1, fn->web_frame_capture_count);
    TEST_ASSERT_EQUAL_STRING("y", fn->web_frame_captures[0].name);
    /* Read-only: is_mutable must be false */
    TEST_ASSERT_FALSE(fn->web_frame_captures[0].is_mutable);

    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_canonical_shape_populates_captures);
    RUN_TEST(test_multiple_canonical_loops_emits_error);
    RUN_TEST(test_nested_canonical_loop_emits_error);
    RUN_TEST(test_non_canonical_condition_emits_error);
    RUN_TEST(test_native_target_is_noop);
    RUN_TEST(test_no_init_window_is_noop);
    RUN_TEST(test_canonical_read_only_capture_not_mutable);
    return UNITY_END();
}
