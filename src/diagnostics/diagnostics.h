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
/* HARD-09: lexer-side OOM during arena allocation — emitted as a diagnostic
 * instead of aborting the process, so iron_analyze_buffer stays fallible
 * on the hot path (CR-01). */
#define IRON_ERR_LEXER_OOM             4
/* HARD-09: string literal exceeded the lexer's 4KB buffer capacity. Emitted
 * once per overflow to avoid log spam on pathological input (WR-07). */
#define IRON_ERR_STRING_TOO_LONG       5

/* Parser errors */
#define IRON_ERR_UNEXPECTED_TOKEN    101
#define IRON_ERR_EXPECTED_EXPR       102
#define IRON_ERR_EXPECTED_RBRACE     103
#define IRON_ERR_EXPECTED_RPAREN     104
#define IRON_ERR_EXPECTED_COLON      105
#define IRON_ERR_EXPECTED_ARROW      106
#define IRON_ERR_PARSE_DEPTH_EXCEEDED 107  /* HARD-08: recursion-depth guard (Plan 04) */
/* Phase 16: keyword used in binding-name position (val X / var X / for X in / func param).
 * Emitted when a v4-reserved keyword (copy, drop, nocopy, unchecked, weak) appears where
 * the parser expects an identifier for a binding name. Parser range 101-199. */
#define IRON_ERR_KEYWORD_NOT_BINDING_NAME 175

/* Phase 17 VAL-01/VAL-02: missing val/var on local binding or field decl.
 * Spec §5.1/§5.2 require explicit val or var; omitting both is a parser
 * error with the spec-mandated message "must specify val or var". The
 * single code covers both the local-binding (parser.c iron_parse_stmt_impl
 * lookahead at line 2530) and field-decl (parser.c:3702-3708 in-place
 * wording change) emission sites — they share quickfix-target semantics
 * for Phase 34 LSP-06 (insert 'val' or 'var'). */
#define IRON_ERR_MISSING_VAL_VAR  176   /* VAL-01, VAL-02 */

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

/* Phase 17 VAL-03: post-init assignment to a non-pub val field. Distinct
 * from IRON_ERR_VAL_REASSIGN=203 (local val rebinding) AND from the
 * pub-val branch (which reuses 203 because pub val is a special case).
 * This code names the field-storage class for non-pub val, enabling
 * Phase 34 LSP-06 quickfix to offer "change field declaration to var"
 * rather than "remove rebinding". Slotted at 265 (next free in semantic
 * range 200-289 after Phase 88 BREAK 260-264). */
#define IRON_ERR_VAL_FIELD_REASSIGN  265   /* VAL-03 */

/* Phase 18 PARM-01: read-only parameter mutation. Function parameters
 * default to read-only borrow (spec §5.3); mutation in body is a compile
 * error unless the parameter is declared with 'var'. Routes the
 * read-only-param subset of E0203 (val rebind) and E0234 (immutable-receiver
 * field write) to a dedicated quickfix-target for Phase 34 LSP-06 to offer
 * "add 'var' modifier" rather than "remove rebinding". Slotted at 266
 * (next free in semantic range 200-289 after Phase 17's E0265). */
#define IRON_ERR_PARM_READ_ONLY      266   /* PARM-01 */

/* Phase 18 PARM-03: read-only argument passed to 'var' parameter slot.
 * Call-site dual of PARM-01 — even if the callee opts in to mutation via
 * `var p`, the caller cannot supply a read-only source (val binding,
 * literal rvalue, val field). Distinct from E0203/E0234/E0266 which fire
 * on the assign target inside the callee body; this code fires on the
 * argument expression at the call site. Quickfix-target: "make argument
 * source mutable" (Phase 34 LSP-06). Hint deliberately omits `*var`
 * pointer suggestion — pointer-typed parameters are Phase 20 territory.
 * Slotted at 267 (next free after Plan 18-01's E0266). */
#define IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT  267   /* PARM-03 */

/* Phase 20 PTR-* — checked pointer types (Plan 20-01 declares; Plan 20-01
 * emits 268 + 272 at typecheck. Codes 269/270/271 are declared here and
 * emitted in Plan 20-02. Slot range 268-272 is the next-free block in the
 * 200-289 semantic range after Phase 18's E0267.
 *
 * Code references and quickfix-targets (Phase 34 LSP-06):
 *   268 PTR-11: pointer arithmetic in checked regime — quickfix points to
 *               *unchecked T + Ptr.offset escape hatch (Phase 25).
 *   269 (Plan 20-02): Ptr.cast[T] same-size violation — quickfix shows the
 *                     two type sizes side-by-side.
 *   270 (Plan 20-02): `&` on rvalue (literal, function-call result) —
 *                     quickfix offers "bind to local first then take &".
 *   271 (Plan 20-02): compile-time stack-escape (returning &local) —
 *                     quickfix offers "wrap in heap T(...)" (Phase 21).
 *   272 (Plan 20-01): null-to-non-nullable-pointer at binding-init AND
 *                     runtime null-deref panic identifier reused via
 *                     iron_panic_stale_pointer hdr=NULL path (Phase 19
 *                     panic infra; Plan 20-02 wires runtime emission). */
#define IRON_ERR_PTR_NO_ARITH             268   /* PTR-11 */
#define IRON_ERR_PTR_CAST_SIZE_MISMATCH   269   /* Ptr.cast[T] (Plan 20-02) */
#define IRON_ERR_PTR_AMP_ON_RVALUE        270   /* `&` on rvalue (Plan 20-02) */
#define IRON_ERR_PTR_ESCAPE_STACK_REF     271   /* compile-time escape (Plan 20-02) */
#define IRON_ERR_PTR_NULL_DEREF           272   /* PTR-13 + runtime panic identifier */

/* Phase 21 — Heap policy + free / leak / defer-free (POL-* / DEFER-02)
 *
 *   273 (Plan 21-01): POL-03 position lock — `heap` keyword used outside
 *                     allocation-expression position.  Single code; the hint
 *                     string distinguishes three call-sites:
 *                     "in type annotation" / "in binding declaration" /
 *                     "in parameter declaration".
 *   274 (Plan 21-01): POL-04 target restriction — `free` target must be a
 *                     bare identifier (binding name), not an expression.
 *   275 (Plan 21-01): POL-05 target restriction — `leak` target must be a
 *                     bare identifier (binding name), not an expression.
 *   276 (Plan 21-01): DEFER-02 structural restriction — only
 *                     `defer free <ident>` is supported in v3.0-alpha.1;
 *                     full `defer` semantics ship in Phase 32. */
#define IRON_ERR_HEAP_BAD_POSITION        273  /* POL-03 (3 positions; hint distinguishes type-annotation/binding/parameter) */
#define IRON_ERR_FREE_NOT_BINDING         274  /* POL-04 (free target must be IRON_NODE_IDENT; emitted Plan 21-01 Task 3) */
#define IRON_ERR_LEAK_NOT_BINDING         275  /* POL-05 (leak target must be IRON_NODE_IDENT; emitted Plan 21-01 Task 3) */
#define IRON_ERR_DEFER_FORM_UNSUPPORTED   276  /* DEFER-02 — RETIRED Phase 32 (defer now accepts any statement; code 276 reserved, no longer emitted) */

/* Phase 22 — readonly Purity Tightening (READ-* / OQ-04 / OQ-05)
 *
 *   277 (Plan 22-01): READ-02 param-mutation — readonly method writes a
 *                     parameter; check fires at typecheck.c IRON_NODE_ASSIGN
 *                     arm; sibling to IRON_ERR_PURE_PARAM_WRITE (243).
 *   278 (Plan 22-01): READ-04 I/O — readonly method calls an I/O builtin
 *                     (free-function site at ~line 2585) or I/O stdlib module
 *                     method (method-call site in IRON_NODE_METHOD_CALL arm).
 *   279 (Plan 22-01): READ-05 heap-escape — readonly method allocates heap
 *                     memory; check fires at check_expr IRON_NODE_HEAP arm.
 *
 * Reserved by Plan 22-02 (do NOT use in Plan 22-01):
 *   280: IRON_ERR_READONLY_RETURN_TYPE      — READ-06 declaration-site whitelist
 *   281: IRON_ERR_READONLY_IFACE_CONFORMANCE — READ-07 interface conformance */
#define IRON_ERR_READONLY_PARAM_MUTATION  277  /* READ-02 (typecheck.c IRON_NODE_ASSIGN; Plan 22-01) */
#define IRON_ERR_READONLY_IO              278  /* READ-04 (typecheck.c free-fn + method-call sites; Plan 22-01) */
#define IRON_ERR_READONLY_HEAP_ESCAPE     279  /* READ-05 (typecheck.c IRON_NODE_HEAP arm; Plan 22-01) */
#define IRON_ERR_READONLY_RETURN_TYPE      280  /* READ-06 declaration-site whitelist (typecheck.c check_func_decl + check_method_decl; Plan 22-02) */
#define IRON_ERR_READONLY_IFACE_CONFORMANCE 281  /* READ-07 interface conformance (typecheck.c check_iface_tier_strengthening; Plan 22-02) */
/* Phase 23 VEC: bounded vector [T; <=N] type-level surface */
#define IRON_ERR_VEC_STRICT_LENGTH_MISMATCH    282  /* VEC-04 (typecheck.c VAL_DECL/VAR_DECL ARRAY_LIT element-count mismatch; Plan 23-01) */
#define IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN 283  /* VEC: [T;<=N]<->[T;N] disjoint cross-assign (typecheck.c types_assignable + VAL_DECL/VAR_DECL specialization; Plan 23-01) */
/* Phase 24 Resource Types — drop / copy / nocopy (DROP-01/06/08 + Area 5) */
#define IRON_ERR_DROP_DUPLICATE              284  /* DROP-01 duplicate drop block (Plan 24-01) */
#define IRON_ERR_COPY_DUPLICATE              285  /* DROP-06 duplicate copy block (Plan 24-01) */
#define IRON_ERR_COPY_OF_NOCOPY_TYPE         286  /* DROP-08 copy/assign/pass-by-value of nocopy type (Plan 24-01) */
#define IRON_ERR_DROP_NOT_READONLY           287  /* drop body marked readonly — incompatible (Plan 24-01) */
#define IRON_ERR_DROP_NO_EARLY_RETURN        288  /* drop body uses `return` early — incompatible with field-destructor sweep (Plan 24-01, CONTEXT Area 5) */

/* Phase 25 — *unchecked T + Box[T] (§4.3-§4.4 + §3.4 + §12 step 11)
 *
 * Code allocation note (RESEARCH Pitfall 1): 289 is the only free slot at
 * the top of the semantic range after Phase 24's E0288. Codes 290-293 are
 * PRE-ALLOCATED to LSP/typecheck-internal codes:
 *   290 = IRON_ERR_CANCELLED             (LSP request-cancellation)
 *   291 = IRON_ERR_COMPTIME_FS_DISABLED_IN_LSP_MODE
 *   292 = IRON_ERR_TYPE_MISMATCH_LITERAL (retyped-literal narrowing)
 *   293 = IRON_ERR_MISSING_RETURN        (missing return path)
 * DO NOT REUSE 290-293. Next free slots are 289, 294, 295, 296+.
 *
 *   289 (Plan 25-01): PTR-02/03 cross-regime assign/call/return. Emitted at
 *                     val/var-decl, call-arg, and return sites when one
 *                     pointer is *T (checked) and the other is *unchecked T.
 *                     Hint cites §4.3-§4.4 and points to Box.unwrap().
 *   294 (Plan 25-01): PTR-05/UNCK-04 — `&` cannot produce *unchecked T.
 *                     Emitted at val/var declaration when lhs is *unchecked T
 *                     and rhs is a unary `&` expression. Hint cites §4.3 and
 *                     points to Box.unwrap() or RawPtr (Phase 33).
 *   295 (Plan 25-01 reserves; Plan 25-02 emits): UNCK-06 — Ptr.offset and
 *                     Ptr.diff require *unchecked T argument. Emitted when
 *                     these compiler builtins receive a checked pointer.
 *                     Hint cites §4.3 and points to Box.unwrap(). */
#define IRON_ERR_PTR_REGIME_MISMATCH     289  /* PTR-02/03 cross-regime assign/call/return (Plan 25-01) */
/* Codes 290-293: DO NOT USE — pre-allocated (see comment above).           */
#define IRON_ERR_PTR_AMP_NOT_UNCHECKED   294  /* PTR-05/UNCK-04 '&' cannot produce *unchecked T (Plan 25-01) */
#define IRON_ERR_PTR_ARITH_CHECKED       295  /* UNCK-06 Ptr.offset/Ptr.diff require *unchecked T (Plan 25-01 reserves; Plan 25-02 emits) */

/* Phase 26 — rc Policy (§4.5 + §12 step 12 — POL-06/07/10/11 + OQ-03)
 *
 * Three new diagnostic codes layered on the Plan 26-01 runtime substrate
 * (Iron_RcHeader + iron_rc_alloc/retain/release). Single-code-with-position-
 * hint discipline mirrors Phase 21 E0273 (heap bad position).
 *
 *   296 (Plan 26-02): POL-07 — `&` on rc value forbidden. Emitted at
 *                     typecheck.c IRON_NODE_UNARY-AMP arm when operand
 *                     resolved_type kind == IRON_TYPE_RC. Hint cites
 *                     `weak rc T` (Phase 27) as the non-owning reference path.
 *   297 (Plan 26-02): POL-11 — `rc` in illegal position. Single code with
 *                     position-distinguishing hint, emitted at FOUR parser.c
 *                     sites (mirrors POL-03 E0273 pattern):
 *                       - type-annotation parser (parser.c:~516)
 *                       - binding-declaration parser (parser.c:~2561)
 *                       - parameter-list parser (parser.c:~936)
 *                       - nullable `?rc T` variant (redirect to `weak rc T?` Phase 27)
 *   298 (Plan 26-02): POL-11 — closed-policy guard. Emitted at parser.c
 *                     allocation-expression dispatch when the token is an
 *                     identifier matching the known-future reserved set
 *                     {"pool", "arena", "weak"}. Hint references the canonical
 *                     closed lifecycle policy set {stack, heap, rc, weak rc}.
 *
 * Companion reuses (no new code):
 *   E0279 IRON_ERR_READONLY_HEAP_ESCAPE — extended in typecheck.c IRON_NODE_RC
 *         arm to also reject `rc T(...)` in readonly methods (parallel to the
 *         existing IRON_NODE_HEAP arm; Phase 22 READ-05 extension).
 *   E0286 IRON_ERR_COPY_OF_NOCOPY_TYPE — `rc Box[T]` rejected naturally via
 *         this Phase 24/25 diagnostic (Box[T] is nocopy; rc requires copy).
 *         See docs/dev/RC-LAYOUT.md §3.1.
 */
#define IRON_ERR_PTR_AMP_ON_RC            296  /* POL-07 (Phase 26-02) — `&` on rc/weak rc value; typecheck.c IRON_NODE_UNARY-AMP arm. Phase 27 GA4: message extended to name both rc and weak rc. */
#define IRON_ERR_RC_BAD_POSITION          297  /* POL-11 (Phase 26-02) — rc in type-anno / binding / parameter / nullable; parser.c 4 sites */
#define IRON_ERR_CLOSED_POLICY_KEYWORD    298  /* POL-11 (Phase 26-02) — unknown lifecycle keyword at allocation-expression; parser.c */

/* Phase 27 POL-08 / POL-09 (Plan 27-02): weak rc compiler-surface diagnostics.
 * Built atop the Plan 27-01 runtime substrate (24B Iron_RcHeader with
 * weak_count@16 + iron_weak_rc_retain/release + iron_rc_downgrade/upgrade).
 *
 *   299 (Plan 27-02): POL-08 — direct dereference of `weak rc T`.  weak rc
 *                     references are non-owning and may point at a destructed
 *                     payload; the type system forbids `w.field`, `*w`, `w[i]`,
 *                     and `w.method()` on weak-rc receivers.  Hint redirects to
 *                     `.upgrade()` which returns the nullable strong reference
 *                     `T?` (atomic against the last drop per POL-09).
 *   300 (Plan 27-02): POL-08 — calling `.downgrade()` on a non-rc receiver.
 *                     `.downgrade()` is only available on `rc T` values; calls
 *                     against primitives, objects, pointers, etc., emit E0300.
 *
 * Companion reuses (no new code):
 *   E0296 IRON_ERR_PTR_AMP_ON_RC — extended (no new code) to also reject `&`
 *         on weak rc receivers; the canonical message now names both rc and
 *         weak rc per CONTEXT.md GA4.
 *   E0217 IRON_ERR_TYPE_MISMATCH — passing `weak rc T` where `rc T` is
 *         expected reuses the existing type-mismatch diagnostic; users are
 *         expected to call `.upgrade()` explicitly.
 *   E0279 IRON_ERR_READONLY_HEAP_ESCAPE — NOT extended.  `.upgrade()` is a
 *         read-only operation (CAS-loop on refcount; no allocation, no I/O)
 *         and is allowed in readonly methods per CONTEXT.md GA2.
 */
#define IRON_ERR_WEAK_RC_DEREF             299  /* POL-08 (Phase 27-02) — direct deref of weak rc T (w.field, w.method(), *w, w[i]); typecheck.c */
#define IRON_ERR_WEAK_RC_DOWNGRADE_NOT_RC  300  /* POL-08 (Phase 27-02) — .downgrade() on non-rc receiver; typecheck.c IRON_NODE_METHOD_CALL arm */

/* Phase 28 ARENA-08 (Plan 28-03): rc / weak rc allocation inside an arena.
 *
 * The closed-policy lifecycle lattice is {stack, heap, rc, weak rc, arena}.
 * `rc` and `weak rc` carry refcount discipline whose per-object drop semantics
 * are fundamentally incompatible with an arena's O(1) batch mass-invalidation
 * on reset()/restore() — the arena never runs per-object destructors, so the
 * refcount would leak and weak observers could never learn the strong count
 * hit zero. The error therefore rejects BOTH `rc T(...)` and `weak rc T(...)`
 * allocation forms that appear (lexically) inside an `in arena { ... }` block.
 *
 * The trigger is the LEXICAL `in arena {}` block depth (typecheck.c
 * in_arena_block_depth context flag), NOT the `heap(in:)` named-option list —
 * `rc`/`heap` are distinct allocation keywords (Pitfall 6); an arena only
 * forbids the *refcounted* policies that appear textually within its block.
 *
 * The canonical message names the offending policy (`rc` or `weak rc`) and the
 * substring `arena`; the weak variant additionally carries `weak rc` so
 * weak_rc_in_arena.expected + test_did_publish_arena_violation.py pin it. */
#define IRON_ERR_RC_IN_ARENA               301  /* ARENA-08 (Phase 28-03) — rc/weak rc allocation inside `in arena {}`; typecheck.c lexical in_arena_block_depth */

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

/* HARD-02 (Plan 05): LSP-mode comptime FS-gating — emitted when `read_file()`
 * (or any future FS-bound builtin) is invoked under IRON_ANALYSIS_MODE_LSP.
 * ERROR-level so the caller can surface a clear message in-editor without
 * actually reading the filesystem.
 * RENUMBERED 234→291 (F3 Phase 8 rebase): 234 now owned by IRON_ERR_MUT_FIELD_IMMUT_RECV
 * (Phase 80 MUT). */
#define IRON_ERR_COMPTIME_FS_DISABLED_IN_LSP_MODE 291

/* Phase 4 Plan 04-01 (D-06) — P1 quickfix set for LSP code actions.
 * RENUMBERED (F3 Phase 8 rebase):
 *   IRON_ERR_TYPE_MISMATCH_LITERAL 235 → 292 (235 owned by IRON_ERR_MUT_CALL_ON_VAL, Phase 80 MUT)
 *   IRON_ERR_MISSING_RETURN        236 → 293 (236 owned by IRON_ERR_MUT_ON_PRIMITIVE, Phase 80 MUT) */
#define IRON_ERR_TYPE_MISMATCH_LITERAL 292   /* narrowing of IRON_ERR_TYPE_MISMATCH for literal RHS */
#define IRON_ERR_MISSING_RETURN        293   /* function body reaches end without returning a value */

/* Cancellation meta-diagnostic — emitted by iron_analyze_buffer on cancel.
 * Level is IRON_DIAG_NOTE so it does NOT bump error_count and does NOT change
 * exit-code semantics for CLI. HARD-05 (Plan 03).
 * RENUMBERED 240→290 (F3 Phase 8 rebase): 240 now owned by IRON_ERR_PURE_IO
 * (Phase 84 MUTTIER). */
#define IRON_ERR_CANCELLED            290

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

/* Phase 93 VIS-03: cross-module visibility. Fired by name resolution when a
 * top-level symbol lookup matches a non-`pub` decl whose declaring file
 * differs from the use-site file. Stdlib carve-out (line-offset based via
 * Iron_Parser.user_source_start_line) makes prepended-stdlib decls
 * implicitly pub; user code is private-by-default. Slotted in the open gap
 * between E0314 (POSSIBLY_UNINITIALIZED) and the 400-range LOWER codes. */
#define IRON_ERR_CROSS_MODULE_PRIVATE   320

/* Phase 98 PATCH-03: the standalone form `func TypeName.method()` is
 * removed in v3.2. The form was deprecated in v3.0 but unenforced; the
 * stdlib-side migration in Plan 98-01 rewrote every standalone decl
 * into the `patch object T { ... }` form, so the parser can now reject
 * the standalone form universally. The diagnostic carries the locked
 * substrings "the standalone form" (in the message) and "use `patch
 * object" (in the suggestion). Stdlib carve-out via
 * Iron_Parser.user_source_start_line mirrors the E0320 carve-out: only
 * user-source lines (line >= user_source_start_line) trigger E0321. */
#define IRON_ERR_STANDALONE_METHOD_FORM 321

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
/* Phase 3 NAV-14 (T-03-01): `///` doc-comment body exceeded the 8 KB per-line
 * cap. Body is truncated; a NOTE-level diagnostic is emitted so the user can
 * see why their `///` text stopped mid-sentence. */
#define IRON_WARN_DOC_COMMENT_TRUNCATED 610

/* Phase 4 Plan 04-01 (D-06) — P1 quickfix warnings. */
#define IRON_WARN_UNUSED_IMPORT        611   /* import referenced zero times in module */
#define IRON_WARN_REDUNDANT_CAST       612   /* `expr as T` where expr is already of type T */

/* Phase 17 VAL-05: var binding never reassigned in its scope. Suggests
 * `val` to keep the modifier system honest. Span anchored on the `var`
 * keyword (3-char width slice from binding span start) so Phase 34
 * LSP-06 quickfix can replace "var" with "val" in a single TextEdit. */
#define IRON_WARN_UNUSED_VAR        613   /* VAL-05 */
/* Phase 17 VAL-06: var parameter never mutated in function body.
 * Suggests dropping the `var` modifier (parameters default to read-only
 * borrow under v4 §5.3). Split from VAL-05 because the quickfix wording
 * differs ("change var → val" vs "drop var modifier") and the warning
 * text also differs ("never reassigned" vs "never mutated"). */
#define IRON_WARN_UNUSED_VAR_PARAM  614   /* VAL-06 */

/* Phase 28 ARENA-09 (Plan 28-03): arena-allocated type with a transitive
 * non-trivial destructor. A type warns if it (or a field whose type
 * transitively does) carries a user `drop` block: the arena bulk-frees its
 * backing memory on reset()/restore() WITHOUT running per-object destructors,
 * so the drop body silently never executes. `allow_drop_skip: true` on the
 * `heap(in: arena, allow_drop_skip: true) T(...)` form acknowledges the
 * skipped destructor explicitly and suppresses this warning.
 *
 * Reserved as W0605 by Plan 28-01 (600-604 + 610-614 were already taken;
 * W0605 is the first free slot in the W06xx block). The message names the
 * type and notes drops are skipped on arena reset. */
#define IRON_WARN_ARENA_NONTRIVIAL_DTOR  605   /* ARENA-09 (Phase 28-03) — arena alloc of type with transitive non-trivial drop; allow_drop_skip:true suppresses */

/* Phase 31 GA2 (Plan 31-02) — debug-allocator compile-time lints (best-effort,
 * WARNING level, never block compilation). Codes 606/607 were the first free
 * slots in the W06xx block (600-605 + 610-614 taken). Both surface identically
 * in `ironc check` and LSP publishDiagnostics via iron_analyze (CORE-22). */
#define IRON_WARN_FORGOTTEN_FREE        606   /* DBG-05: non-escaping heap binding never freed/leaked */
#define IRON_WARN_UNREACHABLE_FREE      607   /* DBG-06: second free of an already-freed binding in the same function */

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

/* ── Stale-pointer panic helper (Phase 19, SAFE-03/04/06) ───────────────────
 * iron_panic_stale_pointer is the canonical abort path for a checked-pointer
 * dereference that observes a generation mismatch: the user is dereferencing
 * a pointer to a heap allocation that has been freed (or whose generation
 * has otherwise been bumped — arena reset in Phase 28, partial-init cleanup
 * in Phase 24, etc.).
 *
 * Multi-line stderr block (text default) or one-line JSON (set
 * IRON_PANIC_FORMAT=json BEFORE iron_runtime_init is called). The env
 * variable is read ONCE at iron_runtime_init time and cached — subsequent
 * setenv() calls are NOT honored (Pitfall 6: per-panic getenv is not
 * async-signal-safe and may itself allocate or take a lock).
 *
 * Allocation-site fields (file, line, size) are emitted in debug builds
 * only (IRON_DEBUG_ALLOCATOR); in release builds the JSON form emits
 * "alloc_site":null and "allocation":null while the text form simply
 * omits those lines.
 *
 * Termination: abort(). Triggers SIGABRT, captures core dump, debugger-
 * friendly. Matches iron_oom_abort precedent (above). Process-mode panic
 * (the entire process dies, not just the offending thread) — pointer
 * safety is global.
 *
 * Definition lives in src/runtime/iron_panic.c (linked into iron_runtime,
 * same convention as iron_oom.c — see iron_oom.c lines 9-22 for the
 * definition-vs-declaration split rationale).
 *
 * IronAllocHdr is forward-declared here to avoid pulling
 * runtime/iron_runtime.h into every diagnostics consumer; the full
 * definition lives in iron_runtime.h.
 */
struct IronAllocHdr;  /* forward declaration; full def in runtime/iron_runtime.h */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_stale_pointer(const char *deref_file,
                              int deref_line,
                              const struct IronAllocHdr *hdr);

/* Phase 20 PTR-10 (OQ-B Option C): stack-pointer panic helper.
 *
 * Same emission channels as iron_panic_stale_pointer (text + JSON);
 * different header substring ("dangling stack pointer to frame") and
 * JSON "panic":"stack_pointer". Stack pointers carry no IronAllocHdr —
 * captured_frame_gen is the gen value the pointer holds at the &-site,
 * compared against current iron_stack_gen at deref-check failure.
 *
 * Definition in src/runtime/iron_panic.c; the static-inline
 * iron_check_stack_pointer_gen in src/runtime/iron_runtime.h is the only
 * call site in release builds (panic-on-mismatch); generated user code
 * never calls this directly. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_stale_stack_pointer(const char *deref_file,
                                    int deref_line,
                                    uint64_t captured_frame_gen);

/* Phase 28 GA1 (Plan 28-02): arena-stale-pointer panic helper.
 *
 * Same emission channels as iron_panic_stale_pointer (text + JSON); distinct
 * header substring ("stale arena pointer dereference") and JSON
 * "panic":"arena_pointer". Fired by iron_check_arena_pointer_gen when a fat
 * pointer's generation snapshot no longer matches the owning arena's live
 * generation (reset()/restore() bumped it). IronArenaAllocHdr is forward-
 * declared (full def + ABI lock in runtime/iron_arena_rt.h). Definition in
 * src/runtime/iron_panic.c. */
struct IronArenaAllocHdr;  /* forward declaration; full def in runtime/iron_arena_rt.h */

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_arena_stale(const char *deref_file,
                            int deref_line,
                            const struct IronArenaAllocHdr *hdr);

/* Phase 28 ARENA-10 (Plan 28-02): arena out-of-memory panic helper.
 *
 * Fired by iron_arena_alloc / iron_arena_new when an allocation would exceed
 * the arena's fixed capacity. The bump-pointer contract never returns null —
 * this is the deterministic abort path. Message carries the arena name, the
 * requested size, and the arena capacity (ARENA-10). Definition in
 * src/runtime/iron_panic.c. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_arena_oom(const char *arena_name,
                          uint64_t requested_size,
                          uint64_t capacity);

#endif /* IRON_DIAGNOSTICS_H */
