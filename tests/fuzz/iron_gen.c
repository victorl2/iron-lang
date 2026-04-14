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
#include <stdio.h>
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

/* ── Structure-aware mutator (Task 3) ────────────────────────────────────── */

/* libFuzzer runtime helper — re-enter the built-in byte mutator from a
 * custom mutator. Normally provided at link time by libclang_rt.fuzzer.
 *
 * We provide a weak fallback definition so non-fuzz executables linking
 * libiron_gen.a (the iron_seed_blobs build-time helper) don't need
 * -fsanitize=fuzzer on their link line. When the libFuzzer runtime is
 * linked into a fuzz executable, its strong definition wins and ours
 * is discarded. When it isn't (seed_blobs), our weak no-op stub is
 * used instead — which is fine because iron_seed_blobs never actually
 * invokes any mutator. */
__attribute__((weak))
size_t LLVMFuzzerMutate(uint8_t *Data, size_t Size, size_t MaxSize) {
    (void)Data; (void)MaxSize;
    return Size;
}

/* Deterministic xorshift32 — reproducible for a given seed, unlike
 * rand(). Never returns 0. */
static uint32_t iron_gen_xorshift(uint32_t *state) {
    uint32_t x = *state;
    if (x == 0) x = 0x9e3779b1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Per-op enumeration. Each op mutates the in-arena token array. */
typedef enum {
    IRON_GEN_OP_INSERT_KEYWORD,   /* insert a random keyword token */
    IRON_GEN_OP_INSERT_IDENT,     /* insert a synthetic identifier */
    IRON_GEN_OP_INSERT_LITERAL,   /* int/string/bool literal */
    IRON_GEN_OP_DELETE_TOKEN,     /* drop a random non-EOF token */
    IRON_GEN_OP_SWAP_TOKENS,      /* swap two adjacent tokens */
    IRON_GEN_OP_DUPLICATE_RANGE,  /* duplicate a short [i..j] range */
    IRON_GEN_OP_INSERT_BALANCED,  /* insert matched { ... } or ( ... ) */
    IRON_GEN_OP_BYTE_PERTURB,     /* re-enter LLVMFuzzerMutate on a span */
    IRON_GEN_OP_COUNT,
} Iron_GenOp;

/* Bias weights: higher = more frequently selected.
 *
 * Rationale per bias profile:
 *   - PARSER: blob mutator only runs for the parser in edge cases
 *     (source path is primary). Favours byte-level perturbation so
 *     the lexer boundary still sees novel byte sequences.
 *   - TYPECHECK: favours keyword + identifier insertion and balanced
 *     constructs. The resolver/typechecker care about shape variety
 *     (number of decls, nesting depth, distinct identifiers).
 *   - HIR_TO_LIR: biased hardest toward keywords + balanced blocks.
 *     The motivating bug (rank-13 collect_mono_enums_node SIGSEGV)
 *     hid in enum-in-match walkers, so over-generating `match`/`enum`
 *     tokens and balanced `{ ... }` blocks maximises the chance of
 *     hitting the same surface via structural mutation.
 */
static const int g_bias_weights[IRON_GEN_BIAS_COUNT][IRON_GEN_OP_COUNT] = {
    /*                KW  ID  LIT DEL SWP DUP BAL BYTE */
    /* PARSER      */ { 15, 15, 10, 20, 10,  5,  5, 40 },
    /* TYPECHECK   */ { 30, 25, 15,  5,  5, 10, 25,  5 },
    /* HIR_TO_LIR  */ { 40, 30, 10,  5, 10, 20, 40,  5 },
};

/* Keyword palette for insert-keyword and insert-balanced ops. */
static const Iron_TokenKind g_keyword_palette[] = {
    IRON_TOK_VAL,    IRON_TOK_VAR,    IRON_TOK_IF,     IRON_TOK_ELIF,
    IRON_TOK_ELSE,   IRON_TOK_WHILE,  IRON_TOK_FOR,    IRON_TOK_IN,
    IRON_TOK_MATCH,  IRON_TOK_ENUM,   IRON_TOK_OBJECT, IRON_TOK_INTERFACE,
    IRON_TOK_IMPL,   IRON_TOK_FUNC,   IRON_TOK_RETURN, IRON_TOK_TRUE,
    IRON_TOK_FALSE,  IRON_TOK_NULL_KW,IRON_TOK_SPAWN,  IRON_TOK_PARALLEL,
    IRON_TOK_DEFER,  IRON_TOK_HEAP,   IRON_TOK_RC,     IRON_TOK_LEAK,
    IRON_TOK_SUPER,  IRON_TOK_SELF,   IRON_TOK_IS,     IRON_TOK_NOT,
    IRON_TOK_AND,    IRON_TOK_OR,     IRON_TOK_IMPORT, IRON_TOK_EXTENDS,
};
static const size_t g_keyword_palette_count =
    sizeof(g_keyword_palette) / sizeof(g_keyword_palette[0]);

/* Keyword/punctuation → literal source text (used by emit_tokens_to_source).
 * Returns NULL for kinds that should use the token's value field. */
static const char *iron_gen_keyword_lit(Iron_TokenKind kind) {
    switch (kind) {
        case IRON_TOK_VAL:       return "val";
        case IRON_TOK_VAR:       return "var";
        case IRON_TOK_IF:        return "if";
        case IRON_TOK_ELIF:      return "elif";
        case IRON_TOK_ELSE:      return "else";
        case IRON_TOK_WHILE:     return "while";
        case IRON_TOK_FOR:       return "for";
        case IRON_TOK_IN:        return "in";
        case IRON_TOK_MATCH:     return "match";
        case IRON_TOK_ENUM:      return "enum";
        case IRON_TOK_OBJECT:    return "object";
        case IRON_TOK_INTERFACE: return "interface";
        case IRON_TOK_IMPL:      return "impl";
        case IRON_TOK_EXTENDS:   return "extends";
        case IRON_TOK_FUNC:      return "func";
        case IRON_TOK_RETURN:    return "return";
        case IRON_TOK_TRUE:      return "true";
        case IRON_TOK_FALSE:     return "false";
        case IRON_TOK_NULL_KW:   return "null";
        case IRON_TOK_SPAWN:     return "spawn";
        case IRON_TOK_PARALLEL:  return "parallel_for";
        case IRON_TOK_DEFER:     return "defer";
        case IRON_TOK_HEAP:      return "heap";
        case IRON_TOK_RC:        return "rc";
        case IRON_TOK_LEAK:      return "leak";
        case IRON_TOK_FREE:      return "free";
        case IRON_TOK_SUPER:     return "super";
        case IRON_TOK_SELF:      return "self";
        case IRON_TOK_IS:        return "is";
        case IRON_TOK_NOT:       return "not";
        case IRON_TOK_AND:       return "and";
        case IRON_TOK_OR:        return "or";
        case IRON_TOK_IMPORT:    return "import";
        case IRON_TOK_COMPTIME:  return "comptime";
        case IRON_TOK_AWAIT:     return "await";
        case IRON_TOK_POOL:      return "pool";
        case IRON_TOK_PRIVATE:   return "private";
        case IRON_TOK_EXTERN:    return "extern";

        case IRON_TOK_PLUS:         return "+";
        case IRON_TOK_MINUS:        return "-";
        case IRON_TOK_STAR:         return "*";
        case IRON_TOK_SLASH:        return "/";
        case IRON_TOK_PERCENT:      return "%";
        case IRON_TOK_ASSIGN:       return "=";
        case IRON_TOK_EQUALS:       return "==";
        case IRON_TOK_NOT_EQUALS:   return "!=";
        case IRON_TOK_LESS:         return "<";
        case IRON_TOK_GREATER:      return ">";
        case IRON_TOK_LESS_EQ:      return "<=";
        case IRON_TOK_GREATER_EQ:   return ">=";
        case IRON_TOK_DOT:          return ".";
        case IRON_TOK_DOTDOT:       return "..";
        case IRON_TOK_COMMA:        return ",";
        case IRON_TOK_COLON:        return ":";
        case IRON_TOK_ARROW:        return "->";
        case IRON_TOK_QUESTION:     return "?";
        case IRON_TOK_PLUS_ASSIGN:  return "+=";
        case IRON_TOK_MINUS_ASSIGN: return "-=";
        case IRON_TOK_STAR_ASSIGN:  return "*=";
        case IRON_TOK_SLASH_ASSIGN: return "/=";
        case IRON_TOK_SHL:          return "<<";
        case IRON_TOK_SHR:          return ">>";
        case IRON_TOK_AMP:          return "&";
        case IRON_TOK_PIPE:         return "|";
        case IRON_TOK_CARET:        return "^";
        case IRON_TOK_TILDE:        return "~";
        case IRON_TOK_SHL_ASSIGN:   return "<<=";
        case IRON_TOK_SHR_ASSIGN:   return ">>=";
        case IRON_TOK_AMP_ASSIGN:   return "&=";
        case IRON_TOK_PIPE_ASSIGN:  return "|=";
        case IRON_TOK_CARET_ASSIGN: return "^=";
        case IRON_TOK_LPAREN:       return "(";
        case IRON_TOK_RPAREN:       return ")";
        case IRON_TOK_LBRACKET:     return "[";
        case IRON_TOK_RBRACKET:     return "]";
        case IRON_TOK_LBRACE:       return "{";
        case IRON_TOK_RBRACE:       return "}";
        case IRON_TOK_SEMICOLON:    return ";";
        case IRON_TOK_AT:           return "@";
        case IRON_TOK_NEWLINE:      return "\n";
        case IRON_TOK_WILDCARD:     return "_";

        /* Literal + special kinds — source text is held in the token's
         * `value` field (or generated by the lexer on lexing); the
         * emitter falls back to `value` for these. */
        case IRON_TOK_INTEGER:       return NULL;
        case IRON_TOK_FLOAT:         return NULL;
        case IRON_TOK_STRING:        return NULL;
        case IRON_TOK_INTERP_STRING: return NULL;
        case IRON_TOK_IDENTIFIER:    return NULL;
        case IRON_TOK_EOF:           return NULL;
        case IRON_TOK_ERROR:         return NULL;
        case IRON_TOK_COUNT:         return NULL;
    }
    return NULL;
}

/* Weighted op sampling against the active bias row. */
static Iron_GenOp iron_gen_sample_op(uint32_t *rng_state) {
    Iron_GenBias bias = ((unsigned)g_bias < IRON_GEN_BIAS_COUNT)
                         ? g_bias : IRON_GEN_BIAS_PARSER;
    const int *row = g_bias_weights[bias];
    int total = 0;
    for (int i = 0; i < IRON_GEN_OP_COUNT; i++) total += row[i];
    if (total <= 0) return IRON_GEN_OP_BYTE_PERTURB;

    uint32_t pick = iron_gen_xorshift(rng_state) % (uint32_t)total;
    int acc = 0;
    for (int i = 0; i < IRON_GEN_OP_COUNT; i++) {
        acc += row[i];
        if ((uint32_t)acc > pick) return (Iron_GenOp)i;
    }
    return IRON_GEN_OP_BYTE_PERTURB;
}

/* Build a trivial valid Iron program: `val x = 0` (5 tokens + EOF).
 * Used as the "fresh seed" fallback when the decoder rejects input. */
static int iron_gen_build_minimal(Iron_Token *buf, int cap, Iron_Arena *arena) {
    if (cap < 6) return 0;
    buf[0] = (Iron_Token){ .kind = IRON_TOK_VAL,
                            .value = iron_arena_strdup(arena, "val", 3),
                            .line = 1, .col = 1, .len = 3 };
    buf[1] = (Iron_Token){ .kind = IRON_TOK_IDENTIFIER,
                            .value = iron_arena_strdup(arena, "x", 1),
                            .line = 1, .col = 5, .len = 1 };
    buf[2] = (Iron_Token){ .kind = IRON_TOK_ASSIGN,
                            .value = NULL,
                            .line = 1, .col = 7, .len = 1 };
    buf[3] = (Iron_Token){ .kind = IRON_TOK_INTEGER,
                            .value = iron_arena_strdup(arena, "0", 1),
                            .line = 1, .col = 9, .len = 1 };
    buf[4] = (Iron_Token){ .kind = IRON_TOK_NEWLINE,
                            .value = NULL,
                            .line = 1, .col = 10, .len = 1 };
    buf[5] = (Iron_Token){ .kind = IRON_TOK_EOF,
                            .value = NULL,
                            .line = 2, .col = 1, .len = 0 };
    return 6;
}

/* Write a minimal blob directly into [data, data+max_size). Used by the
 * mutator when the decoder rejects input. Returns written bytes or 0 on
 * overflow (caller treats 0 as "keep original"). */
static size_t iron_gen_emit_minimal_blob(uint8_t *data, size_t max_size,
                                          unsigned int seed) {
    (void)seed;
    Iron_Arena scratch = iron_arena_create(4 * 1024);
    Iron_Token buf[8];
    int n = iron_gen_build_minimal(buf, 8, &scratch);
    size_t w = 0;
    if (n > 0) {
        w = iron_gen_blob_encode_from_tokens(buf, n, data, max_size);
    }
    iron_arena_free(&scratch);
    return w;
}

/* Insert `ins` at index `at`. `cap` is the allocated token buffer size;
 * caller must ensure count+1 <= cap. Returns new count, or -1 on overflow. */
static int iron_gen_insert_token_at(Iron_Token *tokens, int count, int cap,
                                     int at, Iron_Token ins) {
    if (count + 1 > cap) return -1;
    if (at < 0 || at > count) at = count;
    memmove(&tokens[at + 1], &tokens[at],
            (size_t)(count - at) * sizeof(Iron_Token));
    tokens[at] = ins;
    return count + 1;
}

/* Remove tokens[at]. Refuses to delete the trailing EOF. */
static int iron_gen_delete_token_at(Iron_Token *tokens, int count, int at) {
    if (count <= 1) return count;                 /* keep at least EOF */
    if (at < 0 || at >= count - 1) return count;  /* never delete EOF */
    memmove(&tokens[at], &tokens[at + 1],
            (size_t)(count - at - 1) * sizeof(Iron_Token));
    return count - 1;
}

static Iron_Token iron_gen_make_keyword(Iron_Arena *arena, uint32_t *rng) {
    Iron_TokenKind kind =
        g_keyword_palette[iron_gen_xorshift(rng) % g_keyword_palette_count];
    const char *lit = iron_gen_keyword_lit(kind);
    size_t lit_len = lit ? strlen(lit) : 0;
    return (Iron_Token){
        .kind  = kind,
        .value = lit ? iron_arena_strdup(arena, lit, lit_len) : NULL,
        .line = 1, .col = 1, .len = (uint32_t)lit_len,
    };
}

static Iron_Token iron_gen_make_ident(Iron_Arena *arena, uint32_t *rng) {
    /* Synthetic identifier: "id" + 4 hex digits. */
    uint32_t r = iron_gen_xorshift(rng);
    char buf[8];
    buf[0] = 'i';
    buf[1] = 'd';
    const char hex[] = "0123456789abcdef";
    buf[2] = hex[(r >> 28) & 0xf];
    buf[3] = hex[(r >> 24) & 0xf];
    buf[4] = hex[(r >> 20) & 0xf];
    buf[5] = hex[(r >> 16) & 0xf];
    buf[6] = '\0';
    return (Iron_Token){
        .kind  = IRON_TOK_IDENTIFIER,
        .value = iron_arena_strdup(arena, buf, 6),
        .line = 1, .col = 1, .len = 6,
    };
}

static Iron_Token iron_gen_make_literal(Iron_Arena *arena, uint32_t *rng) {
    uint32_t r = iron_gen_xorshift(rng);
    switch (r & 3u) {
        case 0: {
            char buf[16];
            int n = snprintf(buf, sizeof buf, "%u", r & 0xffffu);
            if (n < 0) n = 1;
            return (Iron_Token){
                .kind  = IRON_TOK_INTEGER,
                .value = iron_arena_strdup(arena, buf, (size_t)n),
                .line = 1, .col = 1, .len = (uint32_t)n,
            };
        }
        case 1:
            return (Iron_Token){
                .kind  = IRON_TOK_TRUE,
                .value = iron_arena_strdup(arena, "true", 4),
                .line = 1, .col = 1, .len = 4,
            };
        case 2:
            return (Iron_Token){
                .kind  = IRON_TOK_FALSE,
                .value = iron_arena_strdup(arena, "false", 5),
                .line = 1, .col = 1, .len = 5,
            };
        default:
            return (Iron_Token){
                .kind  = IRON_TOK_STRING,
                .value = iron_arena_strdup(arena, "fuzz", 4),
                .line = 1, .col = 1, .len = 4,
            };
    }
}

/* Apply one op to the token array. Returns the new active count.
 * Treats the trailing IRON_TOK_EOF as immutable — it stays at the end
 * after every op. Never writes past `cap`. Never aborts or calls
 * iron_ice; any internal inconsistency falls through as "no change". */
static int iron_gen_apply_op(Iron_GenOp op, Iron_Token *tokens, int count,
                              int cap, Iron_Arena *arena, uint32_t *rng) {
    if (count < 1) return count;
    int core_count = count - 1;  /* non-EOF prefix length */

    switch (op) {
        case IRON_GEN_OP_INSERT_KEYWORD: {
            if (count + 1 > cap) return count;
            int at = (int)(iron_gen_xorshift(rng) % (uint32_t)(core_count + 1));
            Iron_Token t = iron_gen_make_keyword(arena, rng);
            int n = iron_gen_insert_token_at(tokens, count, cap, at, t);
            return (n < 0) ? count : n;
        }
        case IRON_GEN_OP_INSERT_IDENT: {
            if (count + 1 > cap) return count;
            int at = (int)(iron_gen_xorshift(rng) % (uint32_t)(core_count + 1));
            Iron_Token t = iron_gen_make_ident(arena, rng);
            int n = iron_gen_insert_token_at(tokens, count, cap, at, t);
            return (n < 0) ? count : n;
        }
        case IRON_GEN_OP_INSERT_LITERAL: {
            if (count + 1 > cap) return count;
            int at = (int)(iron_gen_xorshift(rng) % (uint32_t)(core_count + 1));
            Iron_Token t = iron_gen_make_literal(arena, rng);
            int n = iron_gen_insert_token_at(tokens, count, cap, at, t);
            return (n < 0) ? count : n;
        }
        case IRON_GEN_OP_DELETE_TOKEN: {
            if (core_count <= 0) return count;
            int at = (int)(iron_gen_xorshift(rng) % (uint32_t)core_count);
            return iron_gen_delete_token_at(tokens, count, at);
        }
        case IRON_GEN_OP_SWAP_TOKENS: {
            if (core_count < 2) return count;
            int at = (int)(iron_gen_xorshift(rng) % (uint32_t)(core_count - 1));
            Iron_Token tmp = tokens[at];
            tokens[at] = tokens[at + 1];
            tokens[at + 1] = tmp;
            return count;
        }
        case IRON_GEN_OP_DUPLICATE_RANGE: {
            if (core_count < 1 || count + 1 > cap) return count;
            int at = (int)(iron_gen_xorshift(rng) % (uint32_t)core_count);
            Iron_Token dup = tokens[at];
            int n = iron_gen_insert_token_at(tokens, count, cap, at, dup);
            return (n < 0) ? count : n;
        }
        case IRON_GEN_OP_INSERT_BALANCED: {
            if (count + 2 > cap) return count;
            int at = (int)(iron_gen_xorshift(rng) % (uint32_t)(core_count + 1));
            bool braces = (iron_gen_xorshift(rng) & 1u) != 0u;
            Iron_Token open_t = {
                .kind  = braces ? IRON_TOK_LBRACE : IRON_TOK_LPAREN,
                .value = NULL,
                .line = 1, .col = 1, .len = 1,
            };
            Iron_Token close_t = {
                .kind  = braces ? IRON_TOK_RBRACE : IRON_TOK_RPAREN,
                .value = NULL,
                .line = 1, .col = 1, .len = 1,
            };
            int n = iron_gen_insert_token_at(tokens, count, cap, at, open_t);
            if (n < 0) return count;
            int n2 = iron_gen_insert_token_at(tokens, n, cap, at + 1, close_t);
            return (n2 < 0) ? n : n2;
        }
        case IRON_GEN_OP_BYTE_PERTURB: {
            /* Pick a token with a value field, re-enter libFuzzer's
             * byte mutator on a scratch buffer, then scrub non-printable
             * bytes so the value remains a valid C-string. */
            if (core_count <= 0) return count;
            int start = (int)(iron_gen_xorshift(rng) % (uint32_t)core_count);
            for (int i = 0; i < core_count; i++) {
                int idx = (start + i) % core_count;
                if (tokens[idx].value) {
                    size_t vlen = strlen(tokens[idx].value);
                    if (vlen == 0 || vlen > 64) continue;
                    uint8_t scratch[80];
                    memcpy(scratch, tokens[idx].value, vlen);
                    size_t nn = LLVMFuzzerMutate(scratch, vlen, sizeof scratch);
                    if (nn == 0) nn = vlen;
                    if (nn > 64) nn = 64;
                    for (size_t k = 0; k < nn; k++) {
                        if (scratch[k] < 0x20 || scratch[k] >= 0x7f ||
                            scratch[k] == '"' || scratch[k] == '\\') {
                            scratch[k] = (uint8_t)('a' + (scratch[k] % 26));
                        }
                    }
                    tokens[idx].value =
                        iron_arena_strdup(arena, (const char *)scratch, nn);
                    tokens[idx].len = (uint32_t)nn;
                    break;
                }
            }
            return count;
        }
        case IRON_GEN_OP_COUNT:
        default:
            return count;
    }
}

/* Public blob-mode mutator. Decodes the input, applies 1-3 ops against
 * the active bias, re-encodes. On decode failure emits a minimal fresh
 * blob so libFuzzer never freezes its corpus on a bad seed. */
size_t iron_gen_mutate_blob(uint8_t *data, size_t size,
                             size_t max_size, unsigned int seed) {
    if (!data || max_size < sizeof(IronGenBlobHeader)) return size;

    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_Token *decoded = NULL;
    int n = 0;

    if (iron_gen_blob_decode_into_arena(data, size, &arena, &decoded, &n) != 0) {
        iron_arena_free(&arena);
        size_t w = iron_gen_emit_minimal_blob(data, max_size, seed);
        return w > 0 ? w : size;
    }

    /* Copy decoded tokens into a resizable working buffer. Cap at 1024
     * tokens per mutation step — comfortably fits in max_size for all
     * realistic libFuzzer settings. */
    const int cap = 1024;
    if (n > cap) n = cap;
    Iron_Token *work = (Iron_Token *)iron_arena_alloc(
        &arena, sizeof(Iron_Token) * (size_t)cap, _Alignof(Iron_Token));
    if (!work) {
        iron_arena_free(&arena);
        return size;
    }
    memcpy(work, decoded, (size_t)n * sizeof(Iron_Token));

    /* Apply 1-3 ops selected via bias-weighted sampling. */
    uint32_t rng = seed ? seed : 0x9e3779b1u;
    int ops = 1 + (int)(iron_gen_xorshift(&rng) % 3u);
    for (int i = 0; i < ops; i++) {
        Iron_GenOp op = iron_gen_sample_op(&rng);
        n = iron_gen_apply_op(op, work, n, cap, &arena, &rng);
    }

    /* Maintain EOF invariant — ops don't move the EOF but guard anyway. */
    if (n < 1 || work[n - 1].kind != IRON_TOK_EOF) {
        if (n < cap) {
            work[n++] = (Iron_Token){
                .kind = IRON_TOK_EOF, .value = NULL,
                .line = 1, .col = 1, .len = 0
            };
        } else {
            work[cap - 1] = (Iron_Token){
                .kind = IRON_TOK_EOF, .value = NULL,
                .line = 1, .col = 1, .len = 0
            };
            n = cap;
        }
    }

    size_t written = iron_gen_blob_encode_from_tokens(work, n, data, max_size);
    iron_arena_free(&arena);
    if (written == 0) return size;  /* encode overflow — keep original */
    return written;
}

/* Emit a token array as space-separated Iron source text. Truncates at
 * max_out - 1 and null-terminates. Returns bytes written (excluding NUL). */
static size_t emit_tokens_to_source(const Iron_Token *tokens, int n,
                                     uint8_t *out, size_t max_out) {
    if (max_out == 0) return 0;
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        if (tokens[i].kind == IRON_TOK_EOF) break;
        const char *piece = NULL;
        size_t plen = 0;
        if (tokens[i].value) {
            piece = tokens[i].value;
            plen  = strlen(piece);
        } else {
            piece = iron_gen_keyword_lit(tokens[i].kind);
            plen  = piece ? strlen(piece) : 0;
        }
        if (plen == 0) continue;
        if (off > 0 && off + 1 < max_out) {
            out[off++] = ' ';
        }
        if (off + plen + 1 > max_out) {
            size_t room = (max_out > off + 1) ? (max_out - off - 1) : 0;
            if (room > 0) {
                memcpy(out + off, piece, room);
                off += room;
            }
            break;
        }
        memcpy(out + off, piece, plen);
        off += plen;
    }
    if (off >= max_out) off = max_out - 1;
    out[off] = '\0';
    return off;
}

/* Public source-mode mutator for the parser target. Dispatches 50/50
 * between libFuzzer's built-in byte mutator and a token-level pass. */
size_t iron_gen_mutate_source(uint8_t *data, size_t size,
                               size_t max_size, unsigned int seed) {
    if (!data || max_size == 0) return size;

    uint32_t rng = seed ? seed : 0x243f6a88u;
    if ((iron_gen_xorshift(&rng) & 1u) == 0u) {
        return LLVMFuzzerMutate(data, size, max_size);
    }

    /* Token-level path: synthesise a minimal Iron program, apply a few
     * structure ops, emit as source text. */
    Iron_Arena scratch = iron_arena_create(8 * 1024);
    Iron_Token buf[64];
    int n = iron_gen_build_minimal(buf, 64, &scratch);

    int ops = 1 + (int)(iron_gen_xorshift(&rng) % 3u);
    for (int i = 0; i < ops; i++) {
        Iron_GenOp op = iron_gen_sample_op(&rng);
        n = iron_gen_apply_op(op, buf, n, 64, &scratch, &rng);
    }
    if (n < 1 || buf[n - 1].kind != IRON_TOK_EOF) {
        if (n < 64) buf[n++] = (Iron_Token){ .kind = IRON_TOK_EOF };
    }

    size_t w = emit_tokens_to_source(buf, n, data, max_size);
    iron_arena_free(&scratch);

    /* If source emission came out too small, fall through to byte-level. */
    if (w < 4 && size > 0) {
        return LLVMFuzzerMutate(data, size, max_size);
    }
    return w;
}

/* Single entry point libFuzzer calls on every mutation step. Dispatches
 * on g_bias: parser target runs source-level mutation; the two blob-mode
 * targets run blob-level mutation. libFuzzer resolves this symbol from
 * libiron_gen.a at link time. */
size_t LLVMFuzzerCustomMutator(uint8_t *Data, size_t Size,
                                size_t MaxSize, unsigned int Seed) {
    if (g_bias == IRON_GEN_BIAS_PARSER) {
        return iron_gen_mutate_source(Data, Size, MaxSize, Seed);
    }
    return iron_gen_mutate_blob(Data, Size, MaxSize, Seed);
}
