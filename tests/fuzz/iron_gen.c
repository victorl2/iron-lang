/* tests/fuzz/iron_gen.c — Phase 68 Plan 02 implementation.
 *
 * Delivers the real IRTB binary blob codec (Task 1), and the
 * structure-aware token mutator + source-text mutator (Task 3).
 *
 * Task 1 ships first as a standalone commit so the codec is usable by
 * Task 2's seed_blobs.c without pulling in the mutator machinery.
 * Task 3 extends this file with the per-bias mutator (see follow-up
 * commit in Plan 02).
 *
 * Wire format spec: tests/fuzz/iron_gen.h + 68-RESEARCH.md Code Examples §3.
 * Pitfalls (68-RESEARCH.md):
 *   - Pitfall 2: no pointer storage in blobs; inline bytes + lengths.
 *   - Pitfall 7: IRON_TOK_EOF invariant enforced at encode + decode.
 */
#include "iron_gen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── Bias state ──────────────────────────────────────────────────────────── */

static Iron_GenBias g_bias = IRON_GEN_BIAS_PARSER;

void iron_gen_set_bias(Iron_GenBias bias) {
    if ((unsigned)bias < (unsigned)IRON_GEN_BIAS_COUNT) g_bias = bias;
}

Iron_GenBias iron_gen_get_bias(void) {
    return g_bias;
}

/* ── IRTB codec ──────────────────────────────────────────────────────────── */

/* Decode a packed IRTB blob into a freshly-arena-allocated Iron_Token array.
 *
 * Validation order (all must pass):
 *   1. size >= sizeof(IronGenBlobHeader)
 *   2. magic == IRON_GEN_BLOB_MAGIC
 *   3. version == IRON_GEN_BLOB_VERSION
 *   4. 1 <= token_count <= IRON_GEN_MAX_TOKEN_COUNT
 *   5. Every per-token record fits inside [off, size).
 *   6. tokens[token_count - 1].kind == IRON_TOK_EOF (Pitfall 7).
 *
 * Every byte read is through memcpy to avoid unaligned access UB on ARM
 * and under UBSan alignment checks.
 */
int iron_gen_blob_decode_into_arena(const uint8_t *data, size_t size,
                                     Iron_Arena  *arena,
                                     Iron_Token **out_tokens,
                                     int         *out_count) {
    if (!data || !arena || !out_tokens || !out_count) return -1;
    if (size < sizeof(IronGenBlobHeader)) return -1;

    IronGenBlobHeader hdr;
    memcpy(&hdr, data, sizeof hdr);
    if (hdr.magic != IRON_GEN_BLOB_MAGIC) return -1;
    if (hdr.version != IRON_GEN_BLOB_VERSION) return -1;
    if (hdr.token_count == 0 || hdr.token_count > IRON_GEN_MAX_TOKEN_COUNT) {
        return -1;
    }

    Iron_Token *tokens = (Iron_Token *)iron_arena_alloc(
        arena, sizeof(Iron_Token) * (size_t)hdr.token_count,
        _Alignof(Iron_Token));
    if (!tokens) return -1;

    size_t off = sizeof hdr;
    for (uint32_t i = 0; i < hdr.token_count; i++) {
        if (off + IRON_GEN_TOKEN_HEADER_BYTES > size) return -1;

        uint16_t kind = 0, value_len = 0;
        uint32_t line = 0, col = 0, len = 0;
        memcpy(&kind,      data + off + 0,  2);
        memcpy(&value_len, data + off + 2,  2);
        memcpy(&line,      data + off + 4,  4);
        memcpy(&col,       data + off + 8,  4);
        memcpy(&len,       data + off + 12, 4);
        off += IRON_GEN_TOKEN_HEADER_BYTES;

        /* Bound-check value bytes BEFORE reading. */
        if (value_len > 0) {
            if (off + value_len > size) return -1;
        }

        const char *value = NULL;
        if (value_len > 0) {
            /* Arena-allocate so the decoded token has arena-lifetime
             * storage (Pitfall 2: never store raw pointers in blobs). */
            value = iron_arena_strdup(arena,
                                       (const char *)(data + off),
                                       value_len);
            if (!value) return -1;
        }
        off += value_len;

        tokens[i].kind  = (Iron_TokenKind)kind;
        tokens[i].value = value;
        tokens[i].line  = line;
        tokens[i].col   = col;
        tokens[i].len   = len;
    }

    /* Pitfall 7: last token must be IRON_TOK_EOF. */
    if (tokens[hdr.token_count - 1].kind != IRON_TOK_EOF) return -1;

    *out_tokens = tokens;
    *out_count  = (int)hdr.token_count;
    return 0;
}

/* Encode an Iron_Token array into an IRTB blob in [out, out+max_out).
 *
 * Preconditions:
 *   - count >= 1
 *   - tokens[count - 1].kind == IRON_TOK_EOF (Pitfall 7)
 *   - required bytes <= max_out
 *
 * Returns the number of bytes written, or 0 on any precondition
 * violation (encode overflow is treated the same as bad input — caller
 * falls back to "keep original"). Every multi-byte field is written via
 * memcpy to avoid alignment UB on unaligned output pointers.
 */
size_t iron_gen_blob_encode_from_tokens(const Iron_Token *tokens, int count,
                                         uint8_t *out, size_t max_out) {
    if (!tokens || !out) return 0;
    if (count < 1) return 0;
    if (tokens[count - 1].kind != IRON_TOK_EOF) return 0;  /* Pitfall 7 */

    /* Compute required size. */
    size_t required = sizeof(IronGenBlobHeader);
    for (int i = 0; i < count; i++) {
        size_t vlen = 0;
        if (tokens[i].value) {
            vlen = strlen(tokens[i].value);
            if (vlen > UINT16_MAX) return 0;  /* value_len is uint16_t */
        }
        if (IRON_GEN_TOKEN_HEADER_BYTES + vlen > max_out) return 0;
        required += IRON_GEN_TOKEN_HEADER_BYTES + vlen;
        if (required > max_out) return 0;
    }
    if (required > max_out) return 0;

    /* Write header via memcpy (unaligned output buffer). */
    IronGenBlobHeader hdr;
    hdr.magic       = IRON_GEN_BLOB_MAGIC;
    hdr.version     = (uint16_t)IRON_GEN_BLOB_VERSION;
    hdr.reserved    = 0;
    hdr.token_count = (uint32_t)count;
    hdr.reserved2   = 0;
    memcpy(out, &hdr, sizeof hdr);

    size_t off = sizeof hdr;
    for (int i = 0; i < count; i++) {
        uint16_t kind = (uint16_t)tokens[i].kind;
        uint16_t value_len = 0;
        const char *value = tokens[i].value;
        if (value) value_len = (uint16_t)strlen(value);

        uint32_t line = tokens[i].line;
        uint32_t col  = tokens[i].col;
        uint32_t len  = tokens[i].len;

        memcpy(out + off + 0,  &kind,      2);
        memcpy(out + off + 2,  &value_len, 2);
        memcpy(out + off + 4,  &line,      4);
        memcpy(out + off + 8,  &col,       4);
        memcpy(out + off + 12, &len,       4);
        off += IRON_GEN_TOKEN_HEADER_BYTES;

        if (value_len > 0) {
            memcpy(out + off, value, value_len);
            off += value_len;
        }
    }
    return off;
}

/* Legacy-signature wrapper kept for Plan 01 call sites. Forwards to
 * iron_gen_blob_encode_from_tokens after a void* → Iron_Token* cast. */
size_t iron_gen_blob_encode(const void *tokens, int count,
                             uint8_t *out, size_t max_out) {
    return iron_gen_blob_encode_from_tokens((const Iron_Token *)tokens,
                                             count, out, max_out);
}

/* ── Mutators (Task 3 placeholders) ──────────────────────────────────────── */

/* Task 3 replaces these with the real structure-aware mutator + per-bias
 * weight tables. For Task 1's standalone commit, keep them as harmless
 * pass-throughs so seed_blobs.c can build against the codec without the
 * mutator machinery existing yet. */

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
