/* tests/fuzz/fuzz_emit.c — libFuzzer target for the back end: LIR
 * optimization and C emission.
 *
 * Pipeline per iteration (the order of src/cli/build.c):
 *   1. Decode the IRTB blob into tokens (same format as fuzz_hir_to_lir).
 *   2. Parse; reject inputs with parse errors.
 *   3. Analyze; reject inputs with analyzer errors.
 *   4. Lower to HIR and to LIR; reject when lowering reports errors.
 *   5. iron_lir_optimize with every pass (the default build), then
 *      iron_lir_emit_c. Any crash, sanitizer report or LIR verifier abort
 *      (optimize_verify_or_die exits) from here is the target bug.
 *
 * The emitted C is not compiled (that is cg_fuzz.py's job): this target
 * looks for crashes and memory errors in the optimizer and the emitter on
 * programs no generator writes. Seeds: the blobs seed_blobs.c makes from
 * tests/integration for fuzz_hir_to_lir (corpus/hir_to_lir).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "hir/hir.h"
#include "hir/hir_lower.h"
#include "hir/hir_to_lir.h"
#include "lir/lir.h"
#include "lir/lir_optimize.h"
#include "lir/emit_c.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

#include "iron_gen.h"

#define IRON_FUZZ_EMIT_MAX_INPUT 16384

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    iron_gen_set_bias(IRON_GEN_BIAS_HIR_TO_LIR);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    if (Size > IRON_FUZZ_EMIT_MAX_INPUT) return -1;

    Iron_Arena    arena = iron_arena_create(IRON_ARENA_CHUNK_SIZE);
    Iron_DiagList diags = iron_diaglist_create();

    Iron_Token *tokens = NULL;
    int token_count = 0;
    if (iron_gen_blob_decode_into_arena(Data, Size, &arena, &tokens, &token_count) != 0) {
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }

    Iron_Parser p = iron_parser_create(tokens, token_count, "", "<fuzz>", &arena, &diags);
    Iron_Node *prog_node = iron_parse(&p);
    if (!prog_node || diags.error_count > 0 || prog_node->kind != IRON_NODE_PROGRAM) {
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }
    Iron_Program *prog = (Iron_Program *)prog_node;

    iron_type_track_fixed_arrays();
    Iron_AnalyzeResult ar = iron_analyze(prog, &arena, &diags, NULL, NULL, 0,
                                         /*force_comptime*/ false, IRON_TARGET_NATIVE);
    if (ar.has_errors || !ar.global_scope) {
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }

    iron_type_fixed_arrays_as_bounded();
    IronHIR_Module *hir = iron_hir_lower(prog, ar.global_scope, &arena, &diags);
    if (!hir || diags.error_count > 0) {
        if (hir) iron_hir_module_destroy(hir);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }
    IronLIR_Module *lir = iron_hir_to_lir(hir, prog, ar.global_scope, &arena, &diags);
    if (!lir || diags.error_count > 0) {
        if (lir) iron_lir_module_destroy(lir);
        iron_hir_module_destroy(hir);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }

    /* THE TARGET: the optimizer (every pass, as a default build runs them)
     * and the C emitter. */
    IronLIR_OptimizeInfo info;
    iron_lir_optimize(lir, &info, &arena, /*dump_passes*/ false,
                      /*skip_new_passes*/ false, /*elision_enabled*/ true);
    (void)iron_lir_emit_c(lir, &arena, &diags, &info, &ar.iface_registry,
                          /*warn_fusion_break*/ false, /*report_compression*/ false);

    iron_lir_optimize_info_free(&info);
    iron_lir_module_destroy(lir);
    iron_hir_module_destroy(hir);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    return 0;
}
