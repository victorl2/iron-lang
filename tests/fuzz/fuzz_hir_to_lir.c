/* tests/fuzz/fuzz_hir_to_lir.c — FUZZ-03 libFuzzer target for the
 * HIR-to-LIR lowering pass. The motivating target for Phase 68:
 * this is the one that would have caught the Phase 65 rank-13
 * collect_mono_enums_node SIGSEGV, which hid in a walker-dispatch
 * switch that missed ~16 AST node kinds.
 *
 * Pipeline per iteration:
 *   1. Decode IRTB blob -> Iron_Token[] in per-iteration arena.
 *   2. iron_parser_create + iron_parse. Reject on parser errors
 *      (diags.error_count > 0) or non-PROGRAM node.
 *   3. iron_analyze. Reject on analyzer errors (has_errors == true).
 *      Rationale: analyzer-error crashes are fuzz_typecheck's blast
 *      radius. This target should spend cycles on hir_to_lir, so we
 *      pre-filter inputs that don't make it through analyze cleanly.
 *   4. iron_hir_lower. Reject if it returns NULL (lowering error).
 *   5. iron_hir_to_lir. Any crash here is the target bug.
 *   6. Per-iteration teardown: iron_diaglist_free, iron_arena_free.
 *
 * Generator bias: IRON_GEN_BIAS_HIR_TO_LIR (set in
 * LLVMFuzzerInitialize) weighs match/enum/if-elif/balanced-brace
 * ops heavily — see the g_bias_weights table in iron_gen.c and the
 * rationale comment in Plan 02.
 *
 * Per-iteration lifecycle mirrors fuzz_parser.c / fuzz_typecheck.c:
 *   - Fresh Iron_Arena + Iron_DiagList per iteration (flat RSS).
 *   - Strict teardown order: iron_diaglist_free BEFORE iron_arena_free
 *     because the stb_ds items array may reference arena memory
 *     (Pitfall 4 in 68-RESEARCH.md).
 *
 * Plan 05 note: passes a single per-iteration arena for both the HIR
 * and LIR stages. Production code may split hir_arena / lir_arena for
 * lifetime hygiene, but a fuzz target benefits from one arena (simpler
 * teardown, one free call, no cross-arena pointer confusion). The LIR
 * result lives in the same arena and is freed alongside HIR when the
 * arena is destroyed at the end of the iteration.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "hir/hir.h"
#include "hir/hir_lower.h"
#include "hir/hir_to_lir.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

#include "iron_gen.h"

/* Blobs are larger than raw source text because every token carries a
 * 16-byte header plus optional value bytes. 16384 matches the planned
 * -max_len=16384 flag for the blob-mode targets in fuzz.yml (Plan 06). */
#define IRON_FUZZ_HIR_MAX_INPUT 16384

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    iron_gen_set_bias(IRON_GEN_BIAS_HIR_TO_LIR);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    if (Size > IRON_FUZZ_HIR_MAX_INPUT) {
        /* Reject oversized input so libFuzzer does not accumulate it. */
        return -1;
    }

    /* Per-iteration arena + diaglist. Same 256 KB chunk size as the
     * other fuzz targets — premature tuning would mask real crashes. */
    Iron_Arena    arena = iron_arena_create(IRON_ARENA_CHUNK_SIZE);
    Iron_DiagList diags = iron_diaglist_create();

    Iron_Token *tokens = NULL;
    int token_count = 0;
    if (iron_gen_blob_decode_into_arena(Data, Size, &arena,
                                         &tokens, &token_count) != 0) {
        /* Malformed blob — reject so libFuzzer drops it from the corpus. */
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }

    /* Stage 1: parse. iron_parse never returns NULL (parser.h:30) but may
     * return an ErrorNode or a Program with ErrorNode children. Reject any
     * parse that emitted a diagnostic or returned a non-PROGRAM root. */
    Iron_Parser p = iron_parser_create(tokens, token_count,
                                        /*source*/ "",
                                        "<fuzz>", &arena, &diags);
    Iron_Node *prog_node = iron_parse(&p);
    if (!prog_node || diags.error_count > 0
        || prog_node->kind != IRON_NODE_PROGRAM) {
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }
    Iron_Program *prog = (Iron_Program *)prog_node;

    /* Stage 2: analyze = resolve + typecheck + escape + capture + concurrency.
     * Reject inputs that produced analyzer errors — their crashes belong to
     * fuzz_typecheck's blast radius, not this target. Unlike fuzz_typecheck,
     * fuzz_hir_to_lir filters on has_errors because we want to spend cycles
     * on the hir_to_lir pass, not re-discover analyzer crashes here. */
    Iron_AnalyzeResult ar = iron_analyze(prog, &arena, &diags,
                                          /*source_file_dir*/ NULL,
                                          /*source_text*/     NULL,
                                          /*source_len*/      0,
                                          /*force_comptime*/  false,
                                          /*target*/          IRON_TARGET_NATIVE);
    if (ar.has_errors || !ar.global_scope) {
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }

    /* Stage 3: HIR lowering. Returns NULL on lowering errors — reject so
     * we never hand a broken HIR module to iron_hir_to_lir. */
    IronHIR_Module *hir = iron_hir_lower(prog, ar.global_scope, &arena, &diags);
    if (!hir) {
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }

    /* Stage 4: HIR -> LIR. THIS IS THE TARGET STAGE. Any abort()/SIGSEGV/
     * ASan/UBSan/iron_ice/iron_oom_abort from here downstream surfaces as
     * a libFuzzer crash-NNN file. Return value intentionally discarded —
     * we fuzz for crashes, not diagnostics. A NULL return is a valid
     * error path we want to exercise, not a filter. */
    (void)iron_hir_to_lir(hir, prog, ar.global_scope, &arena, &diags);

    /* Teardown — strict order (diaglist first, arena second). */
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    return 0;
}
