/* test_emit_web.c — Unity snapshot tests for emit_web_module (WEB-EMIT-05..08).
 *
 * Builds a hand-constructed IronLIR_Module with a canonical
 * while(!WindowShouldClose()) function named "main_loop" and a captured local
 * "score", runs iron_lir_web_main_loop_split to populate web_frame_captures,
 * then calls emit_web_module and asserts the emitted C contains the required
 * Emscripten API calls and struct shape.
 *
 * Tests:
 *   test_emit_web_emits_emscripten_include         WEB-EMIT-06
 *   test_emit_web_emits_main_loop_arg_with_zero_zero  WEB-EMIT-07
 *   test_emit_web_shutdown_branch_textual_order    WEB-EMIT-08
 *   test_emit_web_frame_state_struct_named_after_func  WEB-EMIT-05
 *   test_emit_web_no_iron_main_function            (no native body in web output)
 *   test_emit_web_clean_boundary_invariant         WEB-EMIT-05 structural invariant
 */

#include "unity.h"
#include "lir/lir.h"
#include "lir/lir_optimize.h"
#include "lir/web_main_loop_split.h"
#include "lir/emit_web.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "cli/build.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    g_arena;
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

static Iron_Span ts(void) {
    return iron_span_make("test.iron", 1, 1, 1, 2);
}

/* Build a minimal Iron_FuncDecl stub with only `name` set (is_extern=true). */
static Iron_FuncDecl *make_decl(Iron_Arena *a, const char *name) {
    Iron_FuncDecl *d = ARENA_ALLOC(a, Iron_FuncDecl);
    memset(d, 0, sizeof(*d));
    d->name      = name;
    d->is_extern = true;
    return d;
}

/* ── Canonical module builder ────────────────────────────────────────────── *
 *
 * Builds an IronLIR_Module with one function "main_loop" containing:
 *
 *   entry:
 *     %score = alloca Int  ("score")   <- outer-scope local to be captured
 *     %zero  = const_int 0
 *     store %score, %zero
 *     call InitWindow(800, 600, null)
 *     jump loop_header
 *
 *   loop_header:
 *     %wsc = call WindowShouldClose()
 *     %neg = not %wsc
 *     branch %neg, loop_body, loop_exit
 *
 *   loop_body:
 *     %loaded = load %score
 *     %one    = const_int 1
 *     %added  = add %loaded, %one
 *     store %score, %added
 *     jump loop_header
 *
 *   loop_exit:
 *     return void
 *
 * After iron_lir_web_main_loop_split(..., IRON_TARGET_WEB):
 *   fn->web_frame_capture_count == 1
 *   fn->web_frame_captures[0].name == "score"
 *   fn->web_frame_captures[0].is_mutable == true
 */
static IronLIR_Module *build_canonical_module_with_captures(Iron_Arena *a) {
    IronLIR_Module *mod = iron_lir_module_create(a, "test_emit_web");

    Iron_Type *int_type  = iron_type_make_primitive(IRON_TYPE_INT);
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    Iron_Type *void_type = iron_type_make_primitive(IRON_TYPE_VOID);
    Iron_Type *null_type = iron_type_make_primitive(IRON_TYPE_NULL);
    Iron_Span sp = ts();

    Iron_FuncDecl *init_decl = make_decl(a, "InitWindow");
    Iron_FuncDecl *wsc_decl  = make_decl(a, "WindowShouldClose");

    /* Use "main_loop" as the function name so FrameState_main_loop appears. */
    IronLIR_Func  *fn        = iron_lir_func_create(mod, "main_loop", NULL, 0, void_type);
    IronLIR_Block *entry     = iron_lir_block_create(fn, "entry");
    IronLIR_Block *loop_hdr  = iron_lir_block_create(fn, "loop_header");
    IronLIR_Block *loop_body = iron_lir_block_create(fn, "loop_body");
    IronLIR_Block *loop_exit = iron_lir_block_create(fn, "loop_exit");

    /* entry: alloca score, store 0, call InitWindow, jump to header */
    IronLIR_Instr *score_slot = iron_lir_alloca(fn, entry, int_type, "score", sp);
    IronLIR_Instr *zero       = iron_lir_const_int(fn, entry, 0, int_type, sp);
    iron_lir_store(fn, entry, score_slot->id, zero->id, sp);

    IronLIR_Instr *width  = iron_lir_const_int(fn, entry, 800, int_type, sp);
    IronLIR_Instr *height = iron_lir_const_int(fn, entry, 600, int_type, sp);
    IronLIR_Instr *title  = iron_lir_const_null(fn, entry, null_type, sp);
    IronLIR_ValueId init_args[3] = { width->id, height->id, title->id };
    iron_lir_call(fn, entry, init_decl, IRON_LIR_VALUE_INVALID,
                  init_args, 3, void_type, sp);
    iron_lir_jump(fn, entry, loop_hdr->id, sp);

    /* loop_header: cond = !WindowShouldClose(); branch cond, body, exit */
    IronLIR_Instr *wsc = iron_lir_call(fn, loop_hdr, wsc_decl,
                                       IRON_LIR_VALUE_INVALID,
                                       NULL, 0, bool_type, sp);
    IronLIR_Instr *neg = iron_lir_unop(fn, loop_hdr, IRON_LIR_NOT, wsc->id,
                                       bool_type, sp);
    iron_lir_branch(fn, loop_hdr, neg->id, loop_body->id, loop_exit->id, sp);

    /* loop_body: score++; jump back to header */
    IronLIR_Instr *loaded = iron_lir_load(fn, loop_body, score_slot->id, int_type, sp);
    IronLIR_Instr *one    = iron_lir_const_int(fn, loop_body, 1, int_type, sp);
    IronLIR_Instr *added  = iron_lir_binop(fn, loop_body, IRON_LIR_ADD,
                                           loaded->id, one->id, int_type, sp);
    iron_lir_store(fn, loop_body, score_slot->id, added->id, sp);
    iron_lir_jump(fn, loop_body, loop_hdr->id, sp);

    /* loop_exit: return void */
    iron_lir_return(fn, loop_exit, IRON_LIR_VALUE_INVALID, true, void_type, sp);

    /* Wire CFG edges (preds/succs) */
    arrput(entry->succs,     loop_hdr->id);
    arrput(loop_hdr->preds,  entry->id);
    arrput(loop_hdr->succs,  loop_body->id);
    arrput(loop_hdr->succs,  loop_exit->id);
    arrput(loop_body->preds, loop_hdr->id);
    arrput(loop_body->succs, loop_hdr->id);
    arrput(loop_hdr->preds,  loop_body->id);
    arrput(loop_exit->preds, loop_hdr->id);

    return mod;
}

/* Helper: run the split pass + emit_web_module on a fresh module, return the
 * emitted C string (arena-allocated). Asserts that no errors occurred and that
 * web_frame_capture_count > 0. */
static const char *build_and_emit(void) {
    Iron_Arena ir_arena = iron_arena_create(1024 * 512);
    iron_types_init(&ir_arena);
    IronLIR_Module *mod = build_canonical_module_with_captures(&ir_arena);

    iron_lir_web_main_loop_split(mod, &g_arena, &g_diags, IRON_TARGET_WEB);
    TEST_ASSERT_EQUAL_INT(0, g_diags.error_count);
    TEST_ASSERT_GREATER_THAN_INT(0, mod->funcs[0]->web_frame_capture_count);

    IronLIR_OptimizeInfo opt_info;
    memset(&opt_info, 0, sizeof(opt_info));

    const char *c = emit_web_module(mod, &g_arena, &g_diags, &opt_info,
                                    NULL, false, false);

    iron_lir_module_destroy(mod);
    iron_arena_free(&ir_arena);
    return c;
}

/* ── Test 1: WEB-EMIT-06 — emscripten include present ───────────────────── */

void test_emit_web_emits_emscripten_include(void) {
    const char *c = build_and_emit();
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_NOT_NULL(strstr(c, "#include <emscripten/emscripten.h>"));
}

/* ── Test 2: WEB-EMIT-07 — emscripten_set_main_loop_arg with (0, 0) ──────── */

void test_emit_web_emits_main_loop_arg_with_zero_zero(void) {
    const char *c = build_and_emit();
    TEST_ASSERT_NOT_NULL(c);

    const char *call = strstr(c, "emscripten_set_main_loop_arg(");
    TEST_ASSERT_NOT_NULL(call);

    /* Within 200 chars of the call site, find ", 0, 0)" — covers
     * simulate_infinite_loop=0 + fps=0 (WEB-EMIT-07 part 2). */
    const char *suffix = strstr(call, ", 0, 0)");
    TEST_ASSERT_NOT_NULL(suffix);
    TEST_ASSERT_LESS_THAN_INT(200, (int)(suffix - call));
}

/* ── Test 3: WEB-EMIT-08 — shutdown branch textual order ─────────────────── */

void test_emit_web_shutdown_branch_textual_order(void) {
    const char *c = build_and_emit();
    TEST_ASSERT_NOT_NULL(c);

    /* Find the frame callback function signature */
    const char *cb = strstr(c, "_frame_cb(void *state_arg)");
    TEST_ASSERT_NOT_NULL(cb);

    /* WEB-EMIT-08: in textual order inside the callback:
     * WindowShouldClose() -> CloseWindow() -> iron_runtime_shutdown()
     * -> free( -> emscripten_cancel_main_loop() */
    const char *p = cb;
    p = strstr(p, "WindowShouldClose()");      TEST_ASSERT_NOT_NULL(p); p++;
    p = strstr(p, "CloseWindow()");            TEST_ASSERT_NOT_NULL(p); p++;
    p = strstr(p, "iron_runtime_shutdown()");  TEST_ASSERT_NOT_NULL(p); p++;
    p = strstr(p, "free(");                    TEST_ASSERT_NOT_NULL(p); p++;
    p = strstr(p, "emscripten_cancel_main_loop()"); TEST_ASSERT_NOT_NULL(p);
}

/* ── Test 4: WEB-EMIT-05 — FrameState struct named after the function ─────── */

void test_emit_web_frame_state_struct_named_after_func(void) {
    const char *c = build_and_emit();
    TEST_ASSERT_NOT_NULL(c);
    /* Struct type is FrameState_main_loop */
    TEST_ASSERT_NOT_NULL(strstr(c, "FrameState_main_loop"));
    /* Captured local "score" appears as a field in the struct */
    TEST_ASSERT_NOT_NULL(strstr(c, "score"));
}

/* ── Test 5: No native Iron_main_loop body in the web output ─────────────── */

void test_emit_web_no_iron_main_function(void) {
    const char *c = build_and_emit();
    TEST_ASSERT_NOT_NULL(c);

    /* The main-loop function's original body was split into callback + wrapper.
     * emit_func_body is deliberately NOT called on the main-loop function, so
     * no definition starting with "void Iron_main_loop(" should appear. */
    TEST_ASSERT_NULL(strstr(c, "void Iron_main_loop("));

    /* The web wrapper "int main(" MUST be present instead. */
    TEST_ASSERT_NOT_NULL(strstr(c, "int main("));
}

/* ── Test 6: WEB-EMIT-05 structural invariant — emit_web.c must not
 *   #include "lir/emit_c.h" (boundary enforcement) ─────────────────────── */

void test_emit_web_clean_boundary_invariant(void) {
    /* Load emit_web.c from the source tree. The test binary's working directory
     * is the CMake build directory; use the IRON_SOURCE_DIR define if available,
     * or fall back to "../src". */
#ifdef IRON_SOURCE_DIR
    const char *emit_web_path = IRON_SOURCE_DIR "/lir/emit_web.c";
#else
    const char *emit_web_path = "../src/lir/emit_web.c";
#endif

    FILE *f = fopen(emit_web_path, "r");
    if (!f) {
        /* If the file is not accessible from the test working directory,
         * skip this sub-test rather than fail — the primary grep invariant
         * is already enforced by Plan 02's verify command. */
        TEST_IGNORE_MESSAGE("emit_web.c not accessible from test working directory; skipping boundary invariant sub-test");
        return;
    }

    /* Read the whole file into a heap buffer */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    char *contents = (char *)malloc((size_t)sz + 1);
    TEST_ASSERT_NOT_NULL(contents);

    size_t nread = fread(contents, 1, (size_t)sz, f);
    fclose(f);
    contents[nread] = '\0';

    /* The file must NOT contain #include "lir/emit_c.h" */
    const char *found = strstr(contents, "#include \"lir/emit_c.h\"");
    free(contents);

    TEST_ASSERT_NULL_MESSAGE(found,
        "emit_web.c must not #include \"lir/emit_c.h\" — boundary invariant violated (WEB-EMIT-05)");
}

/* ── Main runner ─────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_emit_web_emits_emscripten_include);
    RUN_TEST(test_emit_web_emits_main_loop_arg_with_zero_zero);
    RUN_TEST(test_emit_web_shutdown_branch_textual_order);
    RUN_TEST(test_emit_web_frame_state_struct_named_after_func);
    RUN_TEST(test_emit_web_no_iron_main_function);
    RUN_TEST(test_emit_web_clean_boundary_invariant);
    return UNITY_END();
}
