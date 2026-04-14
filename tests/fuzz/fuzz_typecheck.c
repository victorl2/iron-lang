/* tests/fuzz/fuzz_typecheck.c — FUZZ-02 libFuzzer target for the
 * analyzer pipeline (typecheck + escape + capture + concurrency).
 *
 * Input format: IRTB blob (defined in tests/fuzz/iron_gen.h, filled
 * by tests/fuzz/seed_blobs.c at build time, mutated at runtime by
 * iron_gen.c's structure-aware mutator with IRON_GEN_BIAS_TYPECHECK
 * weights).
 *
 * Pipeline per iteration:
 *   1. Decode blob -> Iron_Token[] in per-iteration arena.
 *   2. iron_parser_create + iron_parse to get an Iron_Program.
 *   3. Filter: if parse produced any diagnostics or returned a
 *      non-PROGRAM node, reject (return -1) so libFuzzer doesn't
 *      accumulate it. Pitfall 3 in 68-RESEARCH.md: typecheck is NOT
 *      robust to ErrorNode children, and fuzzing an analyzer on
 *      parser-broken ASTs burns cycles on bugs that are parser bugs.
 *   4. iron_analyze on the clean program. This calls iron_typecheck,
 *      iron_escape, iron_capture, iron_concurrency — all four stages
 *      are valid blast targets for this fuzz target.
 *   5. Per-iteration teardown: iron_diaglist_free, iron_arena_free.
 *
 * Note: we do NOT filter on iron_analyze's has_errors result.
 * Open Question 2 in 68-RESEARCH.md explains why: the whole point
 * of this target is to catch crashes on inputs where analyze emits
 * errors but then crashes on a downstream pass. If a single bug
 * class fills the issue tracker, address it as a Phase 67-style
 * hardening pass rather than filtering it out here.
 *
 * Per-iteration lifecycle mirrors fuzz_parser.c (Plan 03):
 *   - Fresh Iron_Arena + Iron_DiagList per iteration (flat RSS).
 *   - Strict teardown order: iron_diaglist_free BEFORE iron_arena_free
 *     because the stb_ds items array may reference arena memory
 *     (Pitfall 4 in 68-RESEARCH.md).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

#include "iron_gen.h"

/* Blobs are larger than raw source text because every token carries a
 * 16-byte header plus optional value bytes. 16384 matches the planned
 * -max_len=16384 flag for the blob-mode targets in fuzz.yml (Plan 06). */
#define IRON_FUZZ_TC_MAX_INPUT 16384

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    iron_gen_set_bias(IRON_GEN_BIAS_TYPECHECK);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    if (Size > IRON_FUZZ_TC_MAX_INPUT) {
        /* Reject oversized input so libFuzzer does not accumulate it. */
        return -1;
    }

    /* Per-iteration arena + diaglist. Arena chunk size matches fuzz_parser
     * (256 KB) — premature tuning would mask real crashes (Open Question 4
     * in 68-RESEARCH.md). */
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

    /* Stage 1: parse (into an arena-owned AST). iron_parse never returns
     * NULL (parser.h:30) but may return an ErrorNode or a Program with
     * ErrorNode children. */
    Iron_Parser p = iron_parser_create(tokens, token_count,
                                        /*source*/ "",
                                        "<fuzz>", &arena, &diags);
    Iron_Node *prog_node = iron_parse(&p);

    /* Parser-error filter (Pitfall 3 in 68-RESEARCH.md). Typecheck is not
     * robust to ErrorNode children, so reject any parse that emitted a
     * diagnostic or returned a non-PROGRAM root. diags.error_count > 0
     * catches both ErrorNode-root and ErrorNode-child cases because the
     * parser always emits at least one diagnostic when recovering. */
    if (!prog_node || diags.error_count > 0
        || prog_node->kind != IRON_NODE_PROGRAM) {
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return -1;
    }

    Iron_Program *prog = (Iron_Program *)prog_node;

    /* Stage 2: analyze = resolve + typecheck + escape + capture + concurrency.
     * Any abort()/SIGSEGV/ASan/UBSan/iron_ice/iron_oom_abort surfaces as
     * a crash libFuzzer captures. Return value intentionally discarded —
     * we're fuzzing for crashes, not diagnostics. */
    (void)iron_analyze(prog, &arena, &diags,
                        /*source_file_dir*/ NULL,
                        /*source_text*/     NULL,
                        /*source_len*/      0,
                        /*force_comptime*/  false,
                        /*target*/          IRON_TARGET_NATIVE);

    /* Teardown — strict order (diaglist first, arena second). */
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    return 0;
}
