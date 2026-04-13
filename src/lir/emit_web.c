/* emit_web.c — WEB-EMIT-05/06/07/08: web-target C emitter.
 *
 * Phase 6, Plan 02. Implements emit_web_module(), which produces a web-target
 * C translation unit from an IronLIR_Module.
 *
 * This file consumes emit_c.c helpers ONLY through lir/emit_helpers.h.
 * It does NOT include lir/emit_c.h and has no other direct dependency on
 * emit_c.c internals.
 *
 * Design:
 *   - Plain functions (web_frame_capture_count == 0) are delegated verbatim
 *     to emit_func_body() — the plan-01 promoted helper.
 *   - The unique main-loop function (web_frame_capture_count > 0) is split:
 *       1. FrameState struct: one field per web_frame_captures[] entry.
 *       2. Frame callback: emits every instruction in the loop header and
 *          loop-body blocks via emit_instr(), with capture LOAD/STORE rewrites
 *          driven by the capture_alias_map (same mechanism as lambda closures).
 *          The header block's BRANCH terminator is suppressed and replaced by
 *          a synthetic `if (WindowShouldClose())` shutdown branch.
 *       3. main() wrapper: emits pre-loop preamble blocks, allocates the
 *          FrameState struct, and calls emscripten_set_main_loop_arg().
 *
 * Loop header identification (ew_find_loop_header):
 *   Scans fn->blocks[] for the unique block whose last instruction is an
 *   IRON_LIR_BRANCH whose condition traces to IRON_LIR_NOT(CALL(WindowShouldClose)).
 *   This mirrors ws_find_canonical_loops from Phase 5's pass but without
 *   needing full dominator analysis: fn->web_frame_capture_count > 0 guarantees
 *   by Phase 5's contract that exactly one such header exists.
 *
 * Capture alias map (_e aliasing):
 *   emit_instr()'s LOAD/STORE capture branch emits `_e->field_name` using
 *   ctx->current_captures. We declare `FrameState_<fn> *_e = state;` as a
 *   local alias at the top of both the frame callback and main() wrapper, so
 *   all capture rewrites naturally produce `state->field_name` semantics.
 */

#include "lir/emit_web.h"
#include "lir/emit_helpers.h"    /* EmitCtx + promoted emit_func_body/emit_instr/emit_func_signature */
#include "lir/emit_structs.h"    /* emit_type_decls + emit_extern_prototypes — shared with native emitter */
#include "lir/lir.h"
#include "util/strbuf.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "vendor/stb_ds.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* True iff `in` is a CALL to the named function with the given arg count.
 * Copied from web_main_loop_split.c's ws_is_call_to with ew_ prefix. */
static bool ew_is_call_to(IronLIR_Instr *in, const char *name, int expected_arg_count) {
    if (!in || in->kind != IRON_LIR_CALL) return false;
    if (!in->call.func_decl) return false;
    if (!in->call.func_decl->name) return false;
    if (strcmp(in->call.func_decl->name, name) != 0) return false;
    if (in->call.arg_count != expected_arg_count) return false;
    return true;
}

/* Resolve a ValueId to its defining instruction via fn->value_table.
 * Returns NULL on out-of-range or missing. */
static IronLIR_Instr *ew_value_def(IronLIR_Func *fn, IronLIR_ValueId vid) {
    if (vid == IRON_LIR_VALUE_INVALID) return NULL;
    if ((ptrdiff_t)vid >= arrlen(fn->value_table)) return NULL;
    return fn->value_table[vid];
}

/* Find the loop header block index.
 *
 * Walks fn->blocks[] for the block whose last instruction is IRON_LIR_BRANCH
 * whose condition traces to IRON_LIR_NOT(CALL(WindowShouldClose, 0 args)).
 * Returns the index into fn->blocks[] (not the BlockId), or -1 if not found.
 *
 * fn->web_frame_capture_count > 0 guarantees by Phase 5's contract that
 * exactly one such header exists. This function therefore returns -1 only
 * if the contract is violated (Phase 5 bug or caller error).
 *
 * Note: this mirrors the ws_header_is_canonical shape from Phase 5 but is
 * simpler — it returns the block *index* (for direct block array slicing)
 * rather than doing full dominator analysis.
 */
static int ew_find_loop_header(IronLIR_Func *fn) {
    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        if (!blk || blk->instr_count == 0) continue;
        IronLIR_Instr *term = blk->instrs[blk->instr_count - 1];
        if (!term || term->kind != IRON_LIR_BRANCH) continue;

        /* The condition must be a NOT */
        IronLIR_Instr *cond_def = ew_value_def(fn, term->branch.cond);
        if (!cond_def || cond_def->kind != IRON_LIR_NOT) continue;

        /* The NOT's operand must be a CALL to WindowShouldClose() */
        IronLIR_Instr *inner = ew_value_def(fn, cond_def->unop.operand);
        if (!inner) continue;
        if (ew_is_call_to(inner, "WindowShouldClose", 0)) {
            return bi;
        }
    }
    return -1;
}

/* Build the capture alias map for the main-loop function's web frame captures.
 *
 * Iterates all ALLOCAs in fn, matching each one's name_hint against
 * fn->web_frame_captures[i].name. When matched, records the mapping
 * alloca_id -> capture_index in ctx->capture_alias_map (same stb_ds map
 * that emit_instr() uses internally for LOAD/STORE rewriting).
 *
 * Sets ctx->current_captures and ctx->current_capture_count so that
 * emit_instr()'s capture path can look up the capture metadata.
 */
static void ew_build_frame_capture_alias_map(EmitCtx *ctx, IronLIR_Func *fn) {
    hmfree(ctx->capture_alias_map);
    ctx->capture_alias_map    = NULL;
    ctx->current_captures     = fn->web_frame_captures;
    ctx->current_capture_count = fn->web_frame_capture_count;

    for (int bi = 0; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *ins = blk->instrs[ii];
            if (!ins || ins->kind != IRON_LIR_ALLOCA) continue;
            const char *hint = ins->alloca.name_hint;
            if (!hint) continue;
            for (int ci = 0; ci < fn->web_frame_capture_count; ci++) {
                if (strcmp(hint, fn->web_frame_captures[ci].name) == 0) {
                    hmput(ctx->capture_alias_map, ins->id, ci);
                    break;
                }
            }
        }
    }
}

/* Clear the capture alias map and reset related context fields. */
static void ew_clear_capture_alias_map(EmitCtx *ctx) {
    hmfree(ctx->capture_alias_map);
    ctx->capture_alias_map     = NULL;
    ctx->current_captures      = NULL;
    ctx->current_capture_count = 0;
}

/* Emit the FrameState typedef struct into ctx->struct_bodies.
 *
 * Produces:
 *   typedef struct FrameState_<fn_name> {
 *       <c_type> <field_name>;
 *       ...
 *   } FrameState_<fn_name>;
 *
 * Field types come from fn->web_frame_captures[i].type via emit_type_to_c().
 * Field names are fn->web_frame_captures[i].name (C identifiers set by Phase 5).
 */
static void ew_emit_frame_state_struct(EmitCtx *ctx, IronLIR_Func *fn) {
    iron_strbuf_appendf(&ctx->struct_bodies,
        "typedef struct FrameState_%s {\n", fn->name);
    for (int i = 0; i < fn->web_frame_capture_count; i++) {
        Iron_CaptureEntry *cap = &fn->web_frame_captures[i];
        const char *c_type = emit_type_to_c(cap->type, ctx);
        iron_strbuf_appendf(&ctx->struct_bodies, "    %s %s;\n", c_type, cap->name);
    }
    iron_strbuf_appendf(&ctx->struct_bodies,
        "} FrameState_%s;\n\n", fn->name);
}

/* Emit the frame callback function into ctx->implementations.
 *
 * Signature: static void <fn_name>_frame_cb(void *state_arg)
 *
 * Body:
 *   1. Cast state_arg to FrameState pointer.
 *   2. Declare `FrameState_<fn> *_e = state;` — local alias that lets
 *      emit_instr()'s lambda capture path emit `_e->field` which equals
 *      `state->field` semantically (Option B from the plan recipe).
 *   3. Build the capture alias map so LOAD/STORE rewrites go through _e->.
 *   4. Emit all instructions in the loop header block (index header_bi)
 *      EXCEPT the terminating BRANCH (which would generate a goto — we
 *      suppress it and replace with the synthetic shutdown branch below).
 *   5. Emit all instructions in every subsequent block (indices > header_bi).
 *   6. Emit the synthetic shutdown branch:
 *        if (WindowShouldClose()) {
 *            CloseWindow();
 *            iron_runtime_shutdown();
 *            free(state);
 *            emscripten_cancel_main_loop();
 *            return;
 *        }
 *   7. Close the function.
 *   8. Clear the capture alias map.
 */
static void ew_emit_frame_callback(EmitCtx *ctx, IronLIR_Func *fn, int header_bi) {
    /* Build alias map: LOAD/STORE of captured allocas go through _e-> */
    ew_build_frame_capture_alias_map(ctx, fn);

    /* Callback signature */
    iron_strbuf_appendf(&ctx->implementations,
        "static void %s_frame_cb(void *state_arg) {\n", fn->name);

    /* State cast + _e alias */
    iron_strbuf_appendf(&ctx->implementations,
        "    FrameState_%s *state = (FrameState_%s *)state_arg;\n",
        fn->name, fn->name);
    iron_strbuf_appendf(&ctx->implementations,
        "    FrameState_%s *_e = state;"
        "  /* alias: emit_instr lambda-capture path emits _e-> = state-> */\n",
        fn->name);

    /* Emit the loop header block's instructions (all except the terminating BRANCH) */
    if (header_bi >= 0 && header_bi < fn->block_count) {
        IronLIR_Block *hdr = fn->blocks[header_bi];
        ctx->current_block_id = hdr->id;
        for (int ii = 0; ii < hdr->instr_count; ii++) {
            IronLIR_Instr *ins = hdr->instrs[ii];
            if (!ins) continue;
            /* Skip the BRANCH terminator — replaced by the synthetic shutdown branch */
            if (ins->kind == IRON_LIR_BRANCH) continue;
            emit_instr(&ctx->implementations, ins, fn, ctx);
        }
    }

    /* Emit loop body blocks (all blocks after the header) */
    for (int bi = header_bi + 1; bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        if (!blk) continue;
        ctx->current_block_id = blk->id;
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *ins = blk->instrs[ii];
            if (!ins) continue;
            emit_instr(&ctx->implementations, ins, fn, ctx);
        }
    }

    /* Synthetic shutdown branch */
    iron_strbuf_appendf(&ctx->implementations,
        "    if (WindowShouldClose()) {\n"
        "        CloseWindow();\n"
        "        iron_runtime_shutdown();\n"
        "        free(state);\n"
        "        emscripten_cancel_main_loop();\n"
        "        return;\n"
        "    }\n");

    /* Close callback */
    iron_strbuf_appendf(&ctx->implementations, "}\n\n");

    /* Clean up alias map so subsequent functions start fresh */
    ew_clear_capture_alias_map(ctx);
}

/* Emit the main() wrapper into ctx->main_wrapper.
 *
 * Body:
 *   1. iron_runtime_init(argc, argv)
 *   2. Allocate + zero FrameState on the heap.
 *   3. Declare `FrameState_<fn> *_e = state;` alias (same reason as callback).
 *   4. Build capture alias map.
 *   5. Emit all pre-loop preamble blocks (indices 0..header_bi-1) via emit_instr.
 *      These write captured locals through _e->field (= state->field) so the
 *      frame callback reads consistent initial state.
 *   6. Call emscripten_set_main_loop_arg(<fn>_frame_cb, state, 0, 0).
 *   7. return 0.
 *   8. Clear the capture alias map.
 */
static void ew_emit_main_wrapper(EmitCtx *ctx, IronLIR_Func *fn, int header_bi) {
    /* Build alias map for pre-loop preamble emission */
    ew_build_frame_capture_alias_map(ctx, fn);

    iron_strbuf_appendf(&ctx->main_wrapper,
        "int main(int argc, char **argv) {\n");
    iron_strbuf_appendf(&ctx->main_wrapper,
        "    iron_runtime_init(argc, argv);\n");
    iron_strbuf_appendf(&ctx->main_wrapper,
        "    FrameState_%s *state ="
        " (FrameState_%s *)malloc(sizeof(FrameState_%s));\n",
        fn->name, fn->name, fn->name);
    /* FIX-01 Wasm-W1 (Phase 67-02): web main-loop wrapper malloc guard.
     * Pre-fix the next `memset(state, 0, ...)` dereferenced a possibly-NULL
     * pointer under Emscripten heap exhaustion. Same class as emit_c.c
     * HEAP_ALLOC / RC_ALLOC but in the web main-loop wrapper path. */
    iron_strbuf_appendf(&ctx->main_wrapper,
        "    if (!state) iron_oom_abort(\"emit_web main-loop FrameState\");\n");
    iron_strbuf_appendf(&ctx->main_wrapper,
        "    memset(state, 0, sizeof(FrameState_%s));\n", fn->name);
    iron_strbuf_appendf(&ctx->main_wrapper,
        "    FrameState_%s *_e = state;"
        "  /* alias: emit_instr lambda-capture path emits _e-> = state-> */\n",
        fn->name);

    /* Pre-loop preamble: emit blocks 0..header_bi-1 */
    for (int bi = 0; bi < header_bi && bi < fn->block_count; bi++) {
        IronLIR_Block *blk = fn->blocks[bi];
        if (!blk) continue;
        ctx->current_block_id = blk->id;
        for (int ii = 0; ii < blk->instr_count; ii++) {
            IronLIR_Instr *ins = blk->instrs[ii];
            if (!ins) continue;
            emit_instr(&ctx->main_wrapper, ins, fn, ctx);
        }
    }

    /* Start the Emscripten main loop */
    iron_strbuf_appendf(&ctx->main_wrapper,
        "    emscripten_set_main_loop_arg(%s_frame_cb, state, 0, 0);\n",
        fn->name);
    iron_strbuf_appendf(&ctx->main_wrapper, "    return 0;\n");
    iron_strbuf_appendf(&ctx->main_wrapper, "}\n\n");

    /* Clean up alias map */
    ew_clear_capture_alias_map(ctx);
}

/* ── Public entry point ───────────────────────────────────────────────────── */

const char *emit_web_module(IronLIR_Module *module, Iron_Arena *arena,
                            Iron_DiagList *diags,
                            IronLIR_OptimizeInfo *opt_info,
                            Iron_IfaceRegistry *iface_reg,
                            bool warn_fusion_break,
                            bool report_compression) {
    if (!module) return NULL;
    if (diags && diags->error_count > 0) return NULL;

    /* ── Initialize context (mirrors iron_lir_emit_c lines 5510-5530) ─────── */
    EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena              = arena;
    ctx.diags              = diags;
    ctx.module             = module;
    ctx.next_type_tag      = 1;
    ctx.opt_info           = opt_info;
    ctx.iface_reg          = iface_reg;
    ctx.warn_fusion_break  = warn_fusion_break;
    ctx.report_compression = report_compression;

    ctx.includes        = iron_strbuf_create(512);
    ctx.forward_decls   = iron_strbuf_create(256);
    ctx.struct_bodies   = iron_strbuf_create(1024);
    ctx.enum_defs       = iron_strbuf_create(256);
    ctx.global_consts   = iron_strbuf_create(64);
    ctx.prototypes      = iron_strbuf_create(512);
    ctx.lifted_funcs    = iron_strbuf_create(1024);
    ctx.implementations = iron_strbuf_create(4096);
    ctx.main_wrapper    = iron_strbuf_create(256);

    ctx.emitted_optionals = NULL;
    ctx.mono_registry     = NULL;
    ctx.indent            = 0;

    /* ── Include block: emscripten first, then the standard set ────────────── */
    /* WEB-EMIT-05: emscripten include MUST precede all other includes */
    iron_strbuf_appendf(&ctx.includes, "#include <emscripten/emscripten.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"runtime/iron_runtime.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdint.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdbool.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdlib.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <string.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdio.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_math.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_io.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#define IRON_TIMER_STRUCT_DEFINED\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_time.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_log.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_hint.h\"\n");

    /* Portable prefetch macro (mirrors iron_lir_emit_c Phase 44 block) */
    iron_strbuf_appendf(&ctx.includes,
        "#ifdef __GNUC__\n"
        "  #define IRON_PREFETCH(addr) __builtin_prefetch(addr, 0, 3)\n"
        "#elif defined(_MSC_VER)\n"
        "  #include <xmmintrin.h>\n"
        "  #define IRON_PREFETCH(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)\n"
        "#else\n"
        "  #define IRON_PREFETCH(addr) ((void)0)\n"
        "#endif\n\n");

    /* ── Type declarations + extern prototypes (shared with native emitter) ──
     *
     * Without these, raylib bindings — Color/Vec2/Key struct bodies and
     * InitWindow/ClearBackground/etc. forward declarations — never reach
     * the generated C, so emcc fails with "undeclared function 'InitWindow'"
     * and "use of undeclared identifier 'Color'". The native iron_lir_emit_c
     * path runs the same two helpers; emit_web_module must do likewise.
     */
    emit_type_decls(&ctx);
    emit_extern_prototypes(&ctx);

    /* ── Identify the main-loop function ───────────────────────────────────── */
    int main_loop_fi = -1;
    for (int i = 0; i < module->func_count; i++) {
        IronLIR_Func *fn = module->funcs[i];
        if (!fn) continue;
        if (fn->web_frame_capture_count > 0) {
            main_loop_fi = i;
            break;
        }
    }

    if (main_loop_fi < 0) {
        /* No web main-loop function detected (e.g. program has no raylib loop).
         * Emit all functions using the standard native helper — the output is
         * still a valid web C file with the emscripten include at the top. */
        for (int i = 0; i < module->func_count; i++) {
            IronLIR_Func *fn = module->funcs[i];
            if (!fn || fn->is_extern) continue;
            emit_func_body(&ctx, fn);
        }
    } else {
        /* Emit plain (non-main-loop) functions via the native helper */
        for (int i = 0; i < module->func_count; i++) {
            IronLIR_Func *fn = module->funcs[i];
            if (!fn || fn->is_extern) continue;
            if (i == main_loop_fi) continue;  /* handled separately below */
            emit_func_body(&ctx, fn);
        }

        /* Emit the main-loop function using the web-specific path */
        IronLIR_Func *mfn = module->funcs[main_loop_fi];
        int header_bi = ew_find_loop_header(mfn);
        if (header_bi < 0) {
            /* Phase 5 contract violation: captures set but no canonical loop found.
             * Diagnose and return NULL so the caller sees an error. */
            if (diags) {
                Iron_Span zero_span;
                memset(&zero_span, 0, sizeof(zero_span));
                iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                    IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP, zero_span,
                    "emit_web_module: Phase 5 contract violation — "
                    "web_frame_capture_count > 0 but no canonical "
                    "while(!WindowShouldClose()) header found",
                    NULL);
            }
            emit_ctx_cleanup(&ctx);
            return NULL;
        }

        /* 1. Emit FrameState struct */
        ew_emit_frame_state_struct(&ctx, mfn);

        /* 2. Emit frame callback (<fn>_frame_cb) */
        ew_emit_frame_callback(&ctx, mfn, header_bi);

        /* 3. Emit main() wrapper */
        ew_emit_main_wrapper(&ctx, mfn, header_bi);

        /* NOTE: emit_func_body is deliberately NOT called on mfn.
         * Its content has been fully split into the callback + wrapper pair.
         * The native Iron_<fn> symbol must not appear in the web output. */
    }

    /* ── Concatenate all sections (mirrors iron_lir_emit_c lines 6566-6597) ─ */
    Iron_StrBuf output = iron_strbuf_create(8192);

    iron_strbuf_appendf(&output, "/* Generated by Iron compiler (web target) */\n\n");
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.includes),
                        ctx.includes.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.forward_decls),
                        ctx.forward_decls.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.enum_defs),
                        ctx.enum_defs.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.struct_bodies),
                        ctx.struct_bodies.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.global_consts),
                        ctx.global_consts.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.prototypes),
                        ctx.prototypes.len);
    if (ctx.lifted_funcs.len > 0) {
        iron_strbuf_append(&output, iron_strbuf_get(&ctx.lifted_funcs),
                            ctx.lifted_funcs.len);
    }
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.implementations),
                        ctx.implementations.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.main_wrapper),
                        ctx.main_wrapper.len);

    const char *result = iron_arena_strdup(arena, iron_strbuf_get(&output),
                                           output.len);

    emit_ctx_cleanup(&ctx);
    iron_strbuf_free(&output);

    return result;
}
