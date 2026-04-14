/* tests/fuzz/iron_gen.c — Phase 68 Plan 01 skeleton.
 *
 * Real generator, blob codec, and mutator logic lands in Plan 02
 * (FUZZ-04 seed corpora + shared structure-aware generator).
 *
 * This file exists now so:
 *   1. tests/fuzz/CMakeLists.txt's add_library(iron_gen STATIC iron_gen.c)
 *      has a source file to compile.
 *   2. Plan 02's diff is additive inside functions, not "new file".
 *   3. The public iron_gen.h surface is locked and every later plan
 *      can grep for the function names.
 */
#include "iron_gen.h"

/* Current bias profile. Stored but unused in Plan 01. */
static Iron_GenBias g_bias = IRON_GEN_BIAS_PARSER;

void iron_gen_set_bias(Iron_GenBias bias) {
    g_bias = bias;
}

/* Plan 01 skeleton: always report "malformed blob" so the fuzz target
 * rejects the input. Real decoder in Plan 02 reads the IRTB header. */
int iron_gen_blob_decode_into_arena(const uint8_t *data, size_t size,
                                     struct Iron_Arena *arena,
                                     void **out_tokens,
                                     int  *out_count) {
    (void)data; (void)size; (void)arena;
    if (out_tokens) *out_tokens = NULL;
    if (out_count)  *out_count  = 0;
    return -1;
}

/* Plan 01 skeleton: encode-failure always. Real encoder in Plan 02. */
size_t iron_gen_blob_encode(const void *tokens, int count,
                             uint8_t *out, size_t max_out) {
    (void)tokens; (void)count; (void)out; (void)max_out;
    return 0;
}

/* Plan 01 skeleton: pass-through. Real mutator in Plan 02. */
size_t iron_gen_mutate_blob(uint8_t *data, size_t size,
                             size_t max_size, unsigned int seed) {
    (void)data; (void)max_size; (void)seed;
    return size;
}

size_t iron_gen_mutate_source(uint8_t *data, size_t size,
                               size_t max_size, unsigned int seed) {
    (void)data; (void)max_size; (void)seed;
    return size;
}
