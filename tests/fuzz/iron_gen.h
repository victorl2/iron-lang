/* tests/fuzz/iron_gen.h — shared fuzz generator API.
 *
 * Phase 68 Plan 01: skeleton only. Real mutator logic + blob format
 * lands in Plan 02 (FUZZ-04 seed corpora). This header exists now so
 * the three fuzz target .c files and seed_blobs.c all #include it
 * from day one and evolve against a stable public surface.
 *
 * Design contract: CONTEXT.md "Input generation strategy".
 */
#ifndef IRON_FUZZ_GEN_H
#define IRON_FUZZ_GEN_H

#include <stdint.h>
#include <stddef.h>

/* Per-target bias profile. Selected in LLVMFuzzerInitialize. */
typedef enum {
    IRON_GEN_BIAS_PARSER      = 0,
    IRON_GEN_BIAS_TYPECHECK   = 1,
    IRON_GEN_BIAS_HIR_TO_LIR  = 2
} Iron_GenBias;

/* Set the active bias profile for the generator. Called once per
 * process in LLVMFuzzerInitialize. Plan 01 skeleton: stores the value
 * but the generator does nothing with it yet. */
void iron_gen_set_bias(Iron_GenBias bias);

/* Blob format magic + version (see 68-RESEARCH.md Code Examples §3
 * for the wire format). These constants are referenced by Plan 02's
 * encode/decode functions; defined here so the whole fuzz subdir
 * uses the same values. */
#define IRON_GEN_BLOB_MAGIC   0x42545249u  /* 'IRTB' little-endian */
#define IRON_GEN_BLOB_VERSION 1u

/* Forward declarations. Real implementations land in Plan 02.
 * Each returns a failure sentinel in the skeleton so the fuzz targets
 * link successfully and reject inputs until Plan 02 ships real logic. */
struct Iron_Arena;
struct Iron_TokenTag;  /* forward-decl avoids pulling lexer.h into this header */

/* Decode a packed blob into a freshly-arena-allocated Iron_Token array.
 * Returns 0 on success and writes *out_tokens + *out_count; returns -1
 * on malformed input. Plan 01 skeleton: always returns -1. */
int iron_gen_blob_decode_into_arena(const uint8_t *data, size_t size,
                                     struct Iron_Arena *arena,
                                     void **out_tokens,
                                     int  *out_count);

/* Encode an Iron_Token array into a packed blob in [out, out+max_out).
 * Returns the number of bytes written, or 0 on overflow / error.
 * Plan 01 skeleton: always returns 0. */
size_t iron_gen_blob_encode(const void *tokens, int count,
                             uint8_t *out, size_t max_out);

/* Top-level LLVMFuzzerCustomMutator-shaped helper used by Plan 03+.
 * Plan 01 skeleton: pass-through (return Size unchanged). */
size_t iron_gen_mutate_blob(uint8_t *data, size_t size,
                             size_t max_size, unsigned int seed);

/* Parser-target variant: mutate raw Iron source bytes to another
 * coherent-ish program. Plan 01 skeleton: pass-through. */
size_t iron_gen_mutate_source(uint8_t *data, size_t size,
                               size_t max_size, unsigned int seed);

#endif /* IRON_FUZZ_GEN_H */
