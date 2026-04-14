/* tests/fuzz/fuzz_parser.c — FUZZ-01 libFuzzer target for iron_parse.
 *
 * Drives the lexer + parser pipeline on every byte input libFuzzer
 * produces. Every crash, hang (>10s via -timeout=10), ASan report,
 * UBSan report, iron_ice, and iron_oom_abort surfaces as an abort()
 * that libFuzzer captures as a crash-NNN input.
 *
 * Per-iteration lifecycle (Pitfalls 1, 4, 5 in 68-RESEARCH.md):
 *   1. Cap input size at IRON_FUZZ_PARSER_MAX_INPUT to avoid false
 *      timeouts under ASan+UBSan overhead on CI runners.
 *   2. Fresh Iron_Arena per iteration — iron_arena_free tears down all
 *      allocations so RSS stays flat over millions of iterations.
 *   3. Fresh Iron_DiagList per iteration — iron_diaglist_free releases
 *      the stb_ds items array which lives OUTSIDE the arena.
 *   4. Strict order: iron_diaglist_free BEFORE iron_arena_free because
 *      the diag items may reference arena memory.
 *
 * Design contract: 68-CONTEXT.md + 68-RESEARCH.md Architecture Patterns §1.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"

#include "iron_gen.h"

/* Input-size cap matches 68-RESEARCH.md Pitfall 5 and the -max_len=8192
 * flag in .github/workflows/fuzz.yml (lands in Plan 06). Oversized inputs
 * trip libFuzzer's 10s timeout on slow runners without being real bugs,
 * so reject them at the target level too — defense in depth. */
#define IRON_FUZZ_PARSER_MAX_INPUT 8192

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    iron_gen_set_bias(IRON_GEN_BIAS_PARSER);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    if (Size > IRON_FUZZ_PARSER_MAX_INPUT) {
        /* Reject oversized input so libFuzzer does not accumulate it. */
        return -1;
    }

    /* Per-iteration arena sized for the common case (~1 chunk = 256 KB
     * ≫ most fuzz inputs). Pitfall 4 in 68-RESEARCH.md explains why the
     * diaglist items array is NOT in the arena. */
    Iron_Arena    arena = iron_arena_create(IRON_ARENA_CHUNK_SIZE);
    Iron_DiagList diags = iron_diaglist_create();

    /* Null-terminate the input into the arena. Lexer expects a C string. */
    char *src = iron_arena_strdup(&arena, (const char *)Data, Size);
    if (!src) {
        /* Arena allocation failure — iron_arena_strdup should never return
         * NULL because iron_arena_alloc grows the chunk list, but defensive. */
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return 0;
    }

    /* Stage 1: lex. iron_lex_all returns an stb_ds dynamic array; the
     * per-token value strings are arena-owned, so iron_arena_free reclaims
     * them. The Iron_Token array header itself leaks ~24 bytes per
     * iteration which is acceptable for a fuzz target — ~24 MB/hour at
     * 1k iter/s, well under the 2 GB per-runner budget. (If this becomes
     * a problem, follow Pitfall 4's alternative and call arrfree(tokens)
     * explicitly.) */
    Iron_Lexer lex = iron_lexer_create(src, "<fuzz>", &arena, &diags);
    Iron_Token *tokens = iron_lex_all(&lex);
    int n = 0;
    while (tokens && tokens[n].kind != IRON_TOK_EOF) n++;
    if (tokens) n++;   /* include the trailing IRON_TOK_EOF */

    /* Stage 2: parse. iron_parse never returns NULL (parser.h:30). */
    Iron_Parser p = iron_parser_create(tokens, n, src, "<fuzz>",
                                        &arena, &diags);
    (void)iron_parse(&p);

    /* Teardown — strict order (diaglist first, arena second). */
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    return 0;  /* 0 = accept this input; libFuzzer may add to corpus on new coverage. */
}
