/* tests/fuzz/iron_gen.h — shared fuzz generator API.
 *
 * Phase 68 Plan 02: real IRTB binary blob format + structure-aware
 * token mutator + source-text mutator. All three fuzz targets and
 * seed_blobs.c include this header to reach the generator.
 *
 * Design contract: CONTEXT.md "Input generation strategy".
 * Wire format spec: 68-RESEARCH.md Code Examples §3.
 */
#ifndef IRON_FUZZ_GEN_H
#define IRON_FUZZ_GEN_H

#include <stdint.h>
#include <stddef.h>

#include "lexer/lexer.h"   /* Iron_Token, Iron_TokenKind */
#include "util/arena.h"    /* Iron_Arena, iron_arena_alloc, iron_arena_strdup */

/* Per-target bias profile. Selected in LLVMFuzzerInitialize. */
typedef enum {
    IRON_GEN_BIAS_PARSER      = 0,
    IRON_GEN_BIAS_TYPECHECK   = 1,
    IRON_GEN_BIAS_HIR_TO_LIR  = 2,
    IRON_GEN_BIAS_COUNT       = 3
} Iron_GenBias;

/* Set the active bias profile for the generator. Called once per
 * process in LLVMFuzzerInitialize. */
void iron_gen_set_bias(Iron_GenBias bias);

/* Return the active bias (for diagnostic prints, not hot-path use). */
Iron_GenBias iron_gen_get_bias(void);

/* ── Blob wire format constants ──────────────────────────────────────────── */

/* Magic word 'IRTB' (little-endian): I=0x49 R=0x52 T=0x54 B=0x42.
 * As a uint32_t on a little-endian host: 0x42545249. */
#define IRON_GEN_BLOB_MAGIC   0x42545249u
#define IRON_GEN_BLOB_VERSION 1u

/* Hard upper bound on decoded token count — protects decoder against a
 * malicious header claiming a huge count. Keeps per-iteration memory
 * bounded at ~1 MB worst-case (65536 * sizeof(Iron_Token)). */
#define IRON_GEN_MAX_TOKEN_COUNT 65536u

/* Packed blob header (16 bytes). Little-endian on-disk.
 *
 * NOTE: Although `__attribute__((packed))` nominally allows unaligned
 * field access, this codebase (and the decoder below) never accesses
 * fields directly — always via memcpy into aligned locals — so UBSan
 * under `-fsanitize=alignment` stays clean on ARM.
 */
typedef struct {
    uint32_t magic;        /* IRON_GEN_BLOB_MAGIC = 'IRTB' */
    uint16_t version;      /* IRON_GEN_BLOB_VERSION = 1 */
    uint16_t reserved;     /* 0 */
    uint32_t token_count;  /* includes trailing IRON_TOK_EOF */
    uint32_t reserved2;    /* 0 (future expansion) */
} __attribute__((packed)) IronGenBlobHeader;
_Static_assert(sizeof(IronGenBlobHeader) == 16, "blob header must be 16 bytes");

/* Per-token record layout (reference only — decoder/encoder read/write
 * individual fields via memcpy, never via a struct).
 *
 *   uint16_t kind;        // Iron_TokenKind
 *   uint16_t value_len;   // 0 if NULL; else byte length (NO null terminator)
 *   uint32_t line;
 *   uint32_t col;
 *   uint32_t len;         // source-byte length (lexer convention)
 *   uint8_t  value[value_len];
 */
#define IRON_GEN_TOKEN_HEADER_BYTES 16u

/* ── Codec ───────────────────────────────────────────────────────────────── */

/* Decode a packed IRTB blob into a freshly-arena-allocated Iron_Token
 * array. Returns 0 on success and writes *out_tokens + *out_count.
 * Returns -1 on malformed input (bad magic/version, out-of-bounds
 * value_len, token_count out of range, missing trailing EOF).
 *
 * The output tokens have arena-lifetime value strings (copied via
 * iron_arena_strdup) and are safe to pass to iron_parser_create.
 */
int iron_gen_blob_decode_into_arena(const uint8_t *data, size_t size,
                                     Iron_Arena  *arena,
                                     Iron_Token **out_tokens,
                                     int         *out_count);

/* Encode an Iron_Token array into an IRTB blob in [out, out+max_out).
 * Returns the number of bytes written, or 0 on overflow / precondition
 * violation (count < 1 or tokens[count-1].kind != IRON_TOK_EOF).
 *
 * Takes a typed Iron_Token pointer (Plan 02 canonical surface).
 */
size_t iron_gen_blob_encode_from_tokens(const Iron_Token *tokens, int count,
                                         uint8_t *out, size_t max_out);

/* Legacy-shaped wrapper kept for Plan 01 signature compatibility. Forwards
 * to iron_gen_blob_encode_from_tokens after a void* → Iron_Token* cast. */
size_t iron_gen_blob_encode(const void *tokens, int count,
                             uint8_t *out, size_t max_out);

/* ── Mutators ────────────────────────────────────────────────────────────── */

/* LLVMFuzzerCustomMutator-shaped helper for the blob-mode targets
 * (typecheck + hir_to_lir). Decodes the blob, applies one structure-aware
 * operation selected by the active bias, re-encodes, returns new size.
 * On decode failure, emits a fresh minimal valid blob so libFuzzer never
 * freezes its corpus on a bad seed. */
size_t iron_gen_mutate_blob(uint8_t *data, size_t size,
                             size_t max_size, unsigned int seed);

/* Parser-target variant: mutate raw Iron source bytes. Dispatches 50/50
 * between libFuzzer's built-in byte mutator and a token-level pass that
 * lexes, mutates, and re-emits Iron source text. */
size_t iron_gen_mutate_source(uint8_t *data, size_t size,
                               size_t max_size, unsigned int seed);

#endif /* IRON_FUZZ_GEN_H */
