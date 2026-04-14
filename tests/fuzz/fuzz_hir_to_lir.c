/* tests/fuzz/fuzz_hir_to_lir.c — FUZZ-03 libFuzzer target for iron_hir_to_lir.
 *
 * Phase 68 Plan 01 skeleton: the entry point exists and returns 0
 * on every input. Real implementation lands in Plan 05 (FUZZ-03).
 *
 * This file's only job in Plan 01 is to prove:
 *   1. The libFuzzer runtime can link it (-fsanitize=fuzzer).
 *   2. LLVMFuzzerTestOneInput is callable with -runs=1.
 *   3. The executable exits cleanly on an empty corpus.
 */
#include <stdint.h>
#include <stddef.h>

#include "iron_gen.h"

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc; (void)argv;
    iron_gen_set_bias(IRON_GEN_BIAS_HIR_TO_LIR);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
    (void)Data; (void)Size;
    /* Plan 01: skeleton — real hir_to_lir driver in Plan 05. */
    return 0;
}
