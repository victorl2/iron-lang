#ifndef IRON_DIAGNOSTICS_H
#define IRON_DIAGNOSTICS_H

#include "util/arena.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Source span ─────────────────────────────────────────────────────────── */

/* Identifies a range of source text from start (line:col) to end (end_line:end_col).
 * filename is an interned string (arena-allocated, compare by pointer is valid within
 * a single compilation unit).
 * Lines and columns are 1-indexed. Columns are byte-based.
 */
typedef struct {
    const char *filename;
    uint32_t    line;
    uint32_t    col;
    uint32_t    end_line;
    uint32_t    end_col;
} Iron_Span;

/* Construct a span from explicit components. */
Iron_Span iron_span_make(const char *filename,
                          uint32_t line, uint32_t col,
                          uint32_t end_line, uint32_t end_col);

/* Merge two spans: result spans from the start of `start` to the end of `end`.
 * filename is taken from `start`.
 */
Iron_Span iron_span_merge(Iron_Span start, Iron_Span end);

/* ── Diagnostic level ────────────────────────────────────────────────────── */

typedef enum {
    IRON_DIAG_ERROR,
    IRON_DIAG_WARNING,
    IRON_DIAG_NOTE
} Iron_DiagLevel;

/* ── Single diagnostic ───────────────────────────────────────────────────── */

typedef struct {
    Iron_DiagLevel  level;
    int             code;         /* E-code number, e.g. 1 for E0001 */
    Iron_Span       span;
    const char     *message;     /* arena-allocated */
    const char     *suggestion;  /* arena-allocated, NULL if none */
} Iron_Diagnostic;

/* ── Diagnostic list ─────────────────────────────────────────────────────── */

typedef struct {
    Iron_Diagnostic *items;        /* stb_ds dynamic array */
    int              count;
    int              error_count;
    int              warning_count;
} Iron_DiagList;

Iron_DiagList iron_diaglist_create(void);

void iron_diag_emit(Iron_DiagList *list,
                    Iron_Arena    *arena,
                    Iron_DiagLevel level,
                    int            code,
                    Iron_Span      span,
                    const char    *message,
                    const char    *suggestion);

/* Print a single diagnostic with optional source context.
 * source_text may be NULL; if non-NULL, a 3-line context window is shown.
 */
void iron_diag_print(const Iron_Diagnostic *d, const char *source_text);

/* Print all diagnostics in the list. */
void iron_diag_print_all(const Iron_DiagList *list, const char *source_text);

void iron_diaglist_free(Iron_DiagList *list);

/* ── Error codes ─────────────────────────────────────────────────────────── */

/* Lexer errors */
#define IRON_ERR_UNTERMINATED_STRING   1
#define IRON_ERR_INVALID_CHAR          2
#define IRON_ERR_INVALID_NUMBER        3

/* Parser errors */
#define IRON_ERR_UNEXPECTED_TOKEN    101
#define IRON_ERR_EXPECTED_EXPR       102
#define IRON_ERR_EXPECTED_RBRACE     103
#define IRON_ERR_EXPECTED_RPAREN     104
#define IRON_ERR_EXPECTED_COLON      105
#define IRON_ERR_EXPECTED_ARROW      106

/* Semantic errors */
#define IRON_ERR_UNDEFINED_VAR        200
#define IRON_ERR_DUPLICATE_DECL       201
#define IRON_ERR_TYPE_MISMATCH        202
#define IRON_ERR_VAL_REASSIGN         203
#define IRON_ERR_NULLABLE_ACCESS      204
#define IRON_ERR_MISSING_IFACE_METHOD 205
#define IRON_ERR_GENERIC_CONSTRAINT   206
#define IRON_ERR_ESCAPE_NO_FREE       207
#define IRON_ERR_PARALLEL_MUTATION    208
#define IRON_ERR_IMPORT_NOT_FOUND     209
#define IRON_ERR_SELF_OUTSIDE_METHOD  210
#define IRON_ERR_SUPER_NO_PARENT      211
#define IRON_ERR_FREE_NON_HEAP        212
#define IRON_ERR_LEAK_NON_HEAP        213
#define IRON_ERR_LEAK_RC              214
#define IRON_ERR_RETURN_TYPE          215
#define IRON_ERR_ARG_COUNT            216
#define IRON_ERR_ARG_TYPE             217
#define IRON_ERR_NOT_CALLABLE         218
#define IRON_ERR_NO_SUCH_FIELD        219
#define IRON_ERR_NO_SUCH_METHOD       220
#define IRON_ERR_PRIVATE_ACCESS       221
#define IRON_ERR_NUMERIC_CONVERSION   222
#define IRON_ERR_CIRCULAR_TYPE        223
#define IRON_ERR_NONEXHAUSTIVE_MATCH  224
#define IRON_ERR_PATTERN_ARITY        225
#define IRON_ERR_UNREACHABLE_ARM      226
#define IRON_ERR_BINDING_SHADOWS      227
#define IRON_ERR_UNKNOWN_VARIANT      228
#define IRON_ERR_EMPTY_LITERAL_NO_TYPE 229

/* Comptime errors */
#define IRON_ERR_COMPTIME_STEP_LIMIT  230
#define IRON_ERR_COMPTIME_RESTRICTION 231
#define IRON_ERR_COMPTIME_ERROR       232

/* Bitwise operator errors */
#define IRON_ERR_BITWISE_NON_INT      233

/* MUT (Phase 80) — mutable-receiver enforcement errors */
#define IRON_ERR_MUT_FIELD_IMMUT_RECV 234
#define IRON_ERR_MUT_CALL_ON_VAL      235
#define IRON_ERR_MUT_ON_PRIMITIVE     236

/* ACCESS (Phase 83) — visibility / accessor synthesis errors.
 * IRON_ERR_ACCESSOR_NAME_RESERVED fires when a user-declared method in an
 * object body shares a name with a synthesized getter/setter from a `pub`
 * field in the same object. Locks ACCESS-06. */
#define IRON_ERR_ACCESSOR_NAME_RESERVED 237

/* MUTTIER (Phase 84) — mutation-tier enforcement errors.
 * 238/239 fire from readonly-method context; 240..244 fire from pure-method
 * context; 245 is the parse-time placement/exclusivity error allocated by
 * Plan 84-01. Each tier-violation code carries a tier-specific message so
 * users see the distinct violation category without squinting at a shared
 * diagnostic. Plan 84-02 wires the enforcement into the IRON_NODE_ASSIGN,
 * IRON_NODE_METHOD_CALL, IRON_NODE_CALL, and IRON_NODE_IDENT handlers in
 * typecheck.c; flag propagation rides on TypeCtx.in_readonly_method /
 * TypeCtx.in_pure_method, save/restored at method boundary. */
#define IRON_ERR_READONLY_WRITE_SELF        238
#define IRON_ERR_READONLY_CALLS_MUTATING    239
#define IRON_ERR_PURE_IO                    240
#define IRON_ERR_PURE_MUTABLE_GLOBAL        241
#define IRON_ERR_PURE_NON_PURE_CALL         242
#define IRON_ERR_PURE_PARAM_WRITE           243
#define IRON_ERR_PURE_WRITE_SELF            244
#define IRON_ERR_TIER_MODIFIER_PLACEMENT    245

/* INIT (Phase 85) - mandatory-construction enforcement errors.
 * Plan 85-01 reserves the constants; Plan 85-02 wires the emit sites in
 * typecheck.c definite-assignment + delegation-rejection + return-value
 * paths. Each code carries a category-specific message so users see the
 * distinct violation without squinting at a shared diagnostic. */
#define IRON_ERR_INIT_READ_BEFORE_ASSIGN    246   /* INIT-05 */
#define IRON_ERR_INIT_UNASSIGNED_EXIT       247   /* INIT-06 */
#define IRON_ERR_INIT_VAL_DOUBLE_ASSIGN     248   /* INIT-12 */
#define IRON_ERR_INIT_METHOD_ON_PARTIAL     249   /* INIT-09 */
#define IRON_ERR_INIT_EARLY_RETURN          250   /* INIT-10 */
#define IRON_ERR_INIT_DELEGATION            251   /* INIT-14 */
#define IRON_ERR_INIT_RETURN_VALUE          252   /* INIT-11 typecheck branch */

/* Phase 86 PATCH: open-extension diagnostics.
 *
 * PATCH-01 lands the parse-surface for `patch object T { ... }`; the parser
 * emits E0253 when a field declaration appears inside a patch body. E0254
 * (target not found) and E0255 (conflicting patch definitions) are reserved
 * here so Plan 86-02's resolver + typechecker collision scan have stable
 * IDs at the time Plan 86-01 lands. All three live in the 2xx typecheck
 * range; PATCH does not touch the 3xx LIR or 4xx lowering ranges. */
#define IRON_ERR_PATCH_ADDS_FIELD           253   /* PATCH-05 */
#define IRON_ERR_PATCH_TARGET_NOT_FOUND     254   /* PATCH-04 */
#define IRON_ERR_PATCH_CONFLICT             255   /* PATCH-03 */

/* Phase 87 IFACE + SELF range (256-259).
 * E0256: interfaces cannot declare init (IFACE-04 upgrade from the Phase 85
 *   generic IRON_ERR_UNEXPECTED_TOKEN path to a dedicated code).
 * E0257: interface method tier-strengthening violation — implementation is
 *   weaker than its interface sig tier (IFACE-02).
 * E0258 reserved for Plan 87-02 PATCH-08: patch adds interface conformance
 *   but is missing required methods (retroactive-conformance completeness).
 * E0259 reserved for Plan 87-02 SELF: Self used outside method / interface
 *   context. */
#define IRON_ERR_IFACE_CANNOT_DECLARE_INIT  256   /* IFACE-04 */
#define IRON_ERR_IFACE_METHOD_TIER_MISMATCH 257   /* IFACE-02 */
/* Phase 87-02 PATCH-08: retroactive conformance completeness check.
 * Emitted when a patch or object declares `implements I` but a required
 * interface method is not provided across in-object + patch decls. */
#define IRON_ERR_IFACE_CONFORMANCE_MISSING  258   /* PATCH-08 */
/* Phase 87-02 SELF: Self type used outside a method or interface sig.
 * Emitted when `Self` appears as a return-type annotation in a top-level
 * free function or any other non-method context. */
#define IRON_ERR_SELF_OUTSIDE_CONTEXT       259   /* SELF outside method/iface */

/* IR verifier errors */
#define IRON_ERR_LIR_MISSING_TERMINATOR     300
#define IRON_ERR_LIR_INVALID_BRANCH_TARGET  301
#define IRON_ERR_LIR_USE_BEFORE_DEF         302
#define IRON_ERR_LIR_INSTR_AFTER_TERMINATOR 303
#define IRON_ERR_LIR_NO_ENTRY_BLOCK         304
#define IRON_ERR_LIR_RETURN_TYPE_MISMATCH   305
#define IRON_ERR_LIR_PHI_TYPE_MISMATCH      306
#define IRON_ERR_LIR_CALL_TYPE_MISMATCH     307

/* Type validation errors (309+ range) */
#define IRON_ERR_DUPLICATE_MATCH_ARM    309
#define IRON_ERR_INVALID_CAST           310
#define IRON_ERR_CAST_OVERFLOW          311
#define IRON_ERR_INDEX_OUT_OF_BOUNDS    312
#define IRON_ERR_INVALID_SLICE_BOUNDS   313
#define IRON_ERR_POSSIBLY_UNINITIALIZED 314

/* Lowering error codes (400 range) */
#define IRON_ERR_LOWER_UNSUPPORTED         400
#define IRON_ERR_LOWER_UNRESOLVED_IDENT    401
#define IRON_ERR_LOWER_INVALID_ASSIGN      402
#define IRON_ERR_LOWER_INVALID_MATCH       403

/* HIR verifier errors (500 range) */
#define IRON_ERR_HIR_NULL_POINTER          500
#define IRON_ERR_HIR_USE_BEFORE_DEF        501
#define IRON_ERR_HIR_DUPLICATE_BINDING     502
#define IRON_ERR_HIR_TYPE_MISMATCH         503
#define IRON_ERR_HIR_ARG_COUNT_MISMATCH    504
#define IRON_ERR_HIR_INVALID_SCOPE         505
#define IRON_ERR_HIR_MISSING_RETURN_VALUE  506
#define IRON_ERR_HIR_STRUCTURAL            507

/* Warning codes (600 range) */
#define IRON_WARN_SPAWN_NO_HANDLE     600

/* Type validation warnings (601+ range) */
#define IRON_WARN_NARROWING_CAST        601
#define IRON_WARN_NOT_STRINGABLE        602
#define IRON_WARN_POSSIBLE_OVERFLOW     603
#define IRON_WARN_SPAWN_DATA_RACE      604

/* Phase 88 BREAK range (260-264): hard rejection of removed v2 syntax.
 * All five are gated behind Iron_Parser.v3_strict_mode (default false in Phase 88).
 * Phase 89 flips the default to true after codemod migrates the tree. */
#define IRON_ERR_V3_RECEIVER_SYNTAX    260   /* BREAK-01: func (recv: T) name() */
#define IRON_ERR_V3_MUT_RECEIVER       261   /* BREAK-02: func (mut recv: T) name() */
#define IRON_ERR_V3_INLINE_DEFAULT     262   /* BREAK-03: var x: T = expr in object body */
#define IRON_ERR_V3_MUT_KEYWORD        263   /* BREAK-04: mut keyword removed */
#define IRON_ERR_V3_NO_INIT            264   /* INIT-02: object with fields but no init */

/* Web-target LIR main-loop split pass errors (700 range) — Phase 5 WEB-EMIT-04.
 *
 * Emitted by src/lir/web_main_loop_split.c when the canonical
 * `while (!WindowShouldClose()) { body }` shape cannot be located in a
 * function containing InitWindow() under --target=web. Each error cites
 * the canonical alternative in its message so users get an actionable fix.
 *
 * The 700 range is reserved for future --target=web LIR passes; Plan 06
 * and later web phases should allocate upward from 704.
 */
#define IRON_ERR_WEB_MULTIPLE_MAIN_LOOPS       700
#define IRON_ERR_WEB_NON_CANONICAL_MAIN_LOOP   701
#define IRON_ERR_WEB_NESTED_MAIN_LOOP          702
#define IRON_ERR_WEB_MAIN_LOOP_WRONG_FUNCTION  703

/* ── Internal compiler error (ICE) helper (PROT-03) ──────────────────────────
 * iron_ice is the canonical abort path for compiler-internal invariants that
 * should never be reachable in a correct build. It prints a formatted message
 * prefixed with "iron: internal compiler error: " to stderr and calls abort().
 *
 * Use iron_ice when:
 *   - An AST node is the wrong kind after a sym_kind check that should have
 *     guaranteed the correct kind (PROT-03 kind-assert failure path).
 *   - A switch over an Iron_*Kind hits a case the compiler thought was
 *     unreachable.
 *   - An invariant that the type system should have enforced is violated at
 *     runtime.
 *
 * Do NOT use iron_ice for user-facing errors — those go through iron_diag_emit
 * with the Iron_DiagList surface. iron_ice is compiler-bug territory only.
 *
 * The function is declared noreturn. It takes a printf-style format string.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn, format(printf, 1, 2)))
#endif
void iron_ice(const char *fmt, ...);

/* ── Out-of-memory abort helper (FIX-01, Phase 67) ──────────────────────────
 * iron_oom_abort is the canonical abort path for unrecoverable OOM in
 * contexts that have no error channel: runtime macros (IRON_LIST/MAP/SET),
 * generated C code from emit_c.c (HEAP_ALLOC / RC_ALLOC / closure env /
 * parallel-for ctx / boxed ADT), and compiler-internal allocation paths
 * where malloc failure is treated as fatal.
 *
 * Prints "iron: out of memory at <where>\n" to stderr, flushes, and aborts.
 * The `where` string should be a compile-time literal identifying the call
 * site — typically a file:line or function name — so OOM aborts are
 * bisectable from a stderr grep without attaching a debugger.
 *
 * Distinct from iron_ice: iron_ice reports internal compiler errors
 * (unreachable code paths / invariant violations); iron_oom_abort reports
 * a legitimate runtime failure that the codebase has no recovery channel
 * for. Keep the two distinct so downstream telemetry can tell them apart.
 *
 * The function is declared noreturn. Callers do NOT need `break;` or a
 * dummy return value after calling it.
 *
 * Definition lives in src/runtime/iron_oom.c (linked into iron_runtime
 * static library so every generated user binary and every runtime unit
 * test gets the symbol without pulling in iron_compiler's parser/ast
 * transitive dependencies).
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_oom_abort(const char *where);

#endif /* IRON_DIAGNOSTICS_H */
