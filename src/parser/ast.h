#ifndef IRON_AST_H
#define IRON_AST_H

#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations for semantic annotation fields.
 * Do NOT include analyzer headers from ast.h — the analyzer includes ast.h. */
struct Iron_Symbol;
struct Iron_Type;

/* Capture analysis annotation — set by capture.c pass */
typedef struct {
    const char      *name;        /* original Iron variable name */
    struct Iron_Type *type;       /* resolved type from symbol table */
    bool             is_mutable;  /* true = var capture (by pointer), false = val (by value) */
} Iron_CaptureEntry;

/* ── Node kinds ──────────────────────────────────────────────────────────── */

typedef enum {
    /* Program root */
    IRON_NODE_PROGRAM,

    /* Top-level declarations */
    IRON_NODE_IMPORT_DECL,
    IRON_NODE_OBJECT_DECL,
    IRON_NODE_INTERFACE_DECL,
    IRON_NODE_ENUM_DECL,
    IRON_NODE_FUNC_DECL,
    IRON_NODE_METHOD_DECL,

    /* Statements */
    IRON_NODE_VAL_DECL,
    IRON_NODE_VAR_DECL,
    IRON_NODE_ASSIGN,
    IRON_NODE_RETURN,
    IRON_NODE_IF,
    IRON_NODE_WHILE,
    IRON_NODE_FOR,
    IRON_NODE_MATCH,
    IRON_NODE_DEFER,
    IRON_NODE_FREE,
    IRON_NODE_LEAK,
    IRON_NODE_SPAWN,
    IRON_NODE_BLOCK,

    /* Expressions */
    IRON_NODE_INT_LIT,
    IRON_NODE_FLOAT_LIT,
    IRON_NODE_STRING_LIT,
    IRON_NODE_INTERP_STRING,
    IRON_NODE_BOOL_LIT,
    IRON_NODE_NULL_LIT,
    IRON_NODE_IDENT,
    IRON_NODE_BINARY,
    IRON_NODE_UNARY,
    IRON_NODE_CALL,
    IRON_NODE_METHOD_CALL,
    IRON_NODE_FIELD_ACCESS,
    IRON_NODE_INDEX,
    IRON_NODE_SLICE,
    IRON_NODE_LAMBDA,
    IRON_NODE_HEAP,
    IRON_NODE_RC,
    IRON_NODE_COMPTIME,
    IRON_NODE_IS,
    IRON_NODE_CONSTRUCT,
    IRON_NODE_ARRAY_LIT,
    IRON_NODE_AWAIT,

    /* Error recovery */
    IRON_NODE_ERROR,

    /* Structural helpers */
    IRON_NODE_PARAM,
    IRON_NODE_FIELD,
    IRON_NODE_MATCH_CASE,
    IRON_NODE_ENUM_VARIANT,
    IRON_NODE_TYPE_ANNOTATION,
    IRON_NODE_PATTERN,
    IRON_NODE_ENUM_CONSTRUCT,

    IRON_NODE_COUNT
} Iron_NodeKind;

/* ── Base node ───────────────────────────────────────────────────────────── */

/* All AST nodes embed Iron_Span span + Iron_NodeKind kind as their first two
 * fields so a pointer to any node can be safely cast to Iron_Node *. */
typedef struct Iron_Node {
    Iron_Span     span;
    Iron_NodeKind kind;
} Iron_Node;

/* ── Expression node prefix (PROT-01) ────────────────────────────────────────
 * Every expression AST type (Iron_IntLit, Iron_Ident, Iron_BinaryExpr, ...)
 * begins with the same three-field prefix. This typedef lets a caller access
 * `resolved_type` on any expression node via a single generic path instead of
 * branching on kind first. The _Static_assert block at the bottom of this
 * header enforces the layout on every expression type at compile time — adding
 * a new expression type with a wrong field order fails the build immediately.
 *
 * Moved to ast.h in Phase 66 (PROT-01). Previous home: src/hir/hir_lower.c.
 */
typedef struct Iron_ExprNode {
    Iron_Span         span;
    Iron_NodeKind     kind;
    struct Iron_Type *resolved_type;
} Iron_ExprNode;

/* ── AST node kind assertion (PROT-03) ───────────────────────────────────────
 * Debug-build runtime check that an AST node has the expected kind before a
 * concrete cast. Call immediately before casting Iron_Node* to any concrete
 * expression/statement/decl type:
 *
 *   IRON_NODE_ASSERT_KIND(sym->decl_node, IRON_NODE_OBJECT_DECL);
 *   Iron_ObjectDecl *od = (Iron_ObjectDecl *)sym->decl_node;
 *
 * In Release builds (NDEBUG defined) the macro expands to ((void)0) so there
 * is zero runtime cost. In Debug builds a wrong-kind cast aborts via iron_ice
 * with file:line:func diagnostic — the wrong cast shows up as an internal
 * compiler error in CI rather than silent memory corruption.
 *
 * The impl function lives in src/diagnostics/diagnostics.c; it's declared here
 * because ast.h is the single include point for compiler-side AST consumers.
 */
void iron_node_assert_kind_impl(const Iron_Node *node,
                                Iron_NodeKind expected,
                                const char *file,
                                int line,
                                const char *func);

#ifndef NDEBUG
#  define IRON_NODE_ASSERT_KIND(node, expected_kind)                    \
       iron_node_assert_kind_impl((const Iron_Node *)(node),            \
                                  (expected_kind),                      \
                                  __FILE__, __LINE__, __func__)
#else
#  define IRON_NODE_ASSERT_KIND(node, expected_kind) ((void)0)
#endif

/* ── Forward-declare token kind for operators stored in nodes ────────────── */
/* We need Iron_TokenKind from lexer.h but avoid a circular include.
 * The token kind values fit in an int; use int here and cast at use sites. */
typedef int Iron_OpKind;  /* stores Iron_TokenKind values for operators */

/* ── Top-level declarations ──────────────────────────────────────────────── */

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_PROGRAM */
    Iron_Node   **decls;
    int           decl_count;
    /* NAV-15: set to true by iron_analyze_buffer on the single
     * successful-return path. Consumers outside src/analyzer/ and
     * src/parser/ must never mutate a sealed Iron_Program -- the
     * IRON_AST_ASSERT_UNSEALED macro below traps rogue writes in
     * debug builds. See docs/dev/AST_CONTRACT.md. */
    bool          sealed;
    /* Stdlib carve-out: copied from Iron_Parser.user_source_start_line at
     * iron_parse exit. The resolver consults this when classifying decls
     * as stdlib for cross-module visibility purposes. 0 means no
     * carve-out is active (single-file user code with no prepended
     * stdlib). */
    int           user_source_start_line;
} Iron_Program;

/* ── Phase 3 NAV-15: sealed-AST contract debug enforcement ───────────────────
 * Expands to a call into iron_ice (which is noreturn) when a NAV/LSP handler
 * attempts to write to an Iron_Program after iron_analyze_buffer has sealed
 * it. Release builds compile it to a no-op so production is zero-cost.
 *
 * Usage: inside any `src/lsp/facade` write path that touches Iron_Program
 * fields, call IRON_AST_ASSERT_UNSEALED(program) before the write. The
 * full contract lives in docs/dev/AST_CONTRACT.md. */
#ifndef NDEBUG
#  define IRON_AST_ASSERT_UNSEALED(program)                                  \
       do { if ((program) && ((const Iron_Program *)(program))->sealed)     \
            iron_ice("AST_CONTRACT breach: write after analyze at %s:%d",   \
                     __FILE__, __LINE__); } while (0)
#else
#  define IRON_AST_ASSERT_UNSEALED(program) ((void)0)
#endif

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_IMPORT_DECL */
    const char   *path;
    const char   *alias;  /* NULL if no alias */
    /* Phase 3 NAV-14: arena-interned `///` run (lines joined by '\n'); NULL if none. */
    const char   *doc_comment;
} Iron_ImportDecl;

typedef struct Iron_ObjectDecl {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_OBJECT_DECL */
    const char   *name;
    Iron_Node   **fields;
    int           field_count;
    const char   *extends_name;      /* NULL if none */
    const char  **implements_names;  /* array of name strings */
    int           implements_count;
    Iron_Node   **generic_params;
    int           generic_param_count;
    /* Phase 86 PATCH-01 additions (trailing fields; preserves ABI layout for
     * every existing reader that stops at generic_param_count).
     *
     * is_patch          false for classic `object T {}`; true for
     *                   `patch object T {}`. Plan 86-02 resolver walks all
     *                   program decls keying on this bit.
     * target_type_name  NULL when is_patch=false; identifier after
     *                   `patch object` when true. For patches
     *                   target_type_name == name (both point to the same
     *                   arena-strdup) so the resolver can key the patch
     *                   registry without an is_patch branch at every
     *                   lookup site. */
    bool          is_patch;           /* PATCH-01 */
    const char   *target_type_name;   /* PATCH-02 */
    /* NAV-14: arena-interned `///` run; NULL if none. */
    const char   *doc_comment;
    /* True when the object was declared `pub object T {...}` or
     * `pub patch object T {...}`. Default false. Does NOT propagate
     * visibility to members. Resolver reads this onto Iron_Symbol.is_pub
     * for cross-module visibility. */
    bool          is_pub;
} Iron_ObjectDecl;

typedef struct Iron_InterfaceDecl {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_INTERFACE_DECL */
    const char   *name;
    Iron_Node   **method_sigs;
    int           method_count;
    /* Phase 3 NAV-14: arena-interned `///` run; NULL if none. */
    const char   *doc_comment;
} Iron_InterfaceDecl;

typedef struct Iron_EnumDecl {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_ENUM_DECL */
    const char   *name;
    Iron_Node   **variants;
    int           variant_count;
    bool          has_payloads;       /* true if any variant has payload_count > 0 */
    Iron_Node   **generic_params;     /* NULL for non-generic enums */
    int           generic_param_count; /* 0 for non-generic enums */
    /* NAV-14: arena-interned `///` run; NULL if none. */
    const char   *doc_comment;
    /* True when the enum was declared `pub enum E {...}`. Default false.
     * Resolver propagates onto Iron_Symbol.is_pub for both the enum and
     * its variants. */
    bool          is_pub;
} Iron_EnumDecl;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;  /* IRON_NODE_FUNC_DECL */
    const char        *name;
    Iron_Node        **params;
    int                param_count;
    Iron_Node         *return_type;  /* NULL if void */
    Iron_Node         *body;         /* Iron_Block; NULL for extern funcs */
    bool               is_private;
    /* Phase 93 VIS-01/02: true when the function was declared with the
     * top-level `pub` modifier. Default false (top-level decls are private
     * by default in v3.2). Resolver propagates onto Iron_Symbol.is_pub for
     * the cross-module visibility check (E0320). Plan 93-01 introduces
     * this field; Plan 93-03 reads it at the resolver. */
    bool               is_pub;
    bool               is_extern;      /* true for extern func declarations */
    const char        *extern_c_name;  /* C function name, e.g. "InitWindow"; NULL for non-extern */
    Iron_Node        **generic_params;
    int                generic_param_count;
    struct Iron_Type  *resolved_return_type;  /* set by type checker */
    struct Iron_Type **resolved_param_types;  /* set by type checker (extern func FFI) */
    bool               is_fusible;            /* Phase 49: @fusible annotation */
    /* Phase 87 IFACE-01: tier modifiers on interface method signatures. These
     * fields are populated ONLY for Iron_FuncDecl nodes stored inside an
     * Iron_InterfaceDecl.method_sigs array (i.e. interface sigs). Top-level
     * function declarations keep both bits false (Phase 84 already rejects
     * readonly/pure on top-level funcs via E0245). The has_default_body
     * invariant is encoded by body != NULL on an interface sig — no separate
     * flag. Defaults false at every allocation site. */
    bool               is_readonly;
    bool               is_pure;
    /* Phase 3 NAV-14: arena-interned `///` run; NULL if none. */
    const char        *doc_comment;
} Iron_FuncDecl;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;  /* IRON_NODE_METHOD_DECL */
    const char        *type_name;
    const char        *method_name;
    Iron_Node        **params;
    int                param_count;
    Iron_Node         *return_type;  /* NULL if void */
    Iron_Node         *body;
    bool               is_private;
    /* Phase 93 VIS-01/04: true when this method was declared with the
     * `pub` modifier. For in-block members, only valid when the enclosing
     * object is `pub` (Plan 93-02 enforces this). Default false. */
    bool               is_pub;
    Iron_Node        **generic_params;
    int                generic_param_count;
    struct Iron_Type  *resolved_return_type;  /* set by type checker */
    struct Iron_Symbol *owner_sym;             /* set by resolver: the owning type */
    bool               is_array_extension;    /* true for func [T].method(...) */
    const char        *elem_type_name;        /* generic element type param name, e.g. "T" */
    bool               is_fusible;            /* Phase 49: @fusible annotation */
    /* v2.1: true when the method was declared with the new receiver syntax
     * `func (r: Type) method(...)`. In that case, `params[0]` is the
     * receiver (parser desugaring), `self` is NOT auto-prepended by
     * hir_lower, and the receiver is visible in the body under the
     * declared name. False for the classic `func Type.method(...)`
     * form where hir_lower prepends an implicit self. */
    bool               is_receiver_form;
    /* Phase 83 ACCESS-03/04: true when this MethodDecl was synthesized by
     * the parser as an accessor (getter or setter) for a `pub val`/`pub var`
     * field. Plan 83-02 flips it on while synthesizing accessor methods;
     * Phase 84 MUTTIER reads it to retrofit the getter as `readonly`.
     * Default false on every construction site; Plan 83-01 only threads
     * the default, no writer yet. */
    bool               is_synth_accessor;
    /* Phase 84 MUTTIER-01/02: true when this method was declared with the
     * `readonly` modifier in an object block (or retrofitted onto a synth
     * accessor getter). Defaults false. Plan 84-02 reads this bit to
     * reject self-field writes (E0238) and mutating-method calls (E0239)
     * from readonly contexts. Mutual exclusion with is_pure is enforced
     * at parse time via IRON_ERR_TIER_MODIFIER_PLACEMENT. */
    bool               is_readonly;
    /* Phase 84 MUTTIER-03: true when declared with `pure`. Defaults false.
     * Plan 84-02 reads it to reject I/O (E0240), mutable globals (E0241),
     * non-pure calls (E0242), param writes (E0243), and self-writes (E0244).
     * Synth getters are retrofitted is_readonly=true AND is_pure=true in
     * iron_parse_object_decl. Synth setters keep both false. */
    bool               is_pure;
    /* Phase 85 INIT-03/07: true when this MethodDecl is an init declaration
     * (anonymous or named). Plan 85-02 reads it to gate definite-assignment
     * analysis, reject `self.init(...)` delegation (INIT-14), reject return
     * values (INIT-11), and enforce the "no methods before full assignment"
     * rule (INIT-09). Defaults false at every construction site. */
    bool               is_init;
    /* Phase 85 INIT-08: named-init identifier for call-site dispatch. NULL
     * for non-init methods AND for anonymous init. Anonymous init dispatch
     * lives on Type(args); named dispatch on Type.<init_name>(args). When
     * is_init is true and init_name is NULL, method_name == "init"; when
     * is_init is true and init_name != NULL, method_name == init_name so
     * the symbol-table lookup for `Type.<init_name>` hits naturally. */
    const char        *init_name;
    /* NAV-14: arena-interned `///` run; NULL if none. */
    const char        *doc_comment;
    /* True when this method was produced by flattening an in-patch
     * method (`pub patch object T { pub func ... }`). Set in
     * iron_parse_patch_decl; defaults false everywhere else (in-block on
     * a regular object, receiver-form, array-extension). The .iron-stub
     * generator reads this bit to suppress patch-sourced methods even
     * when a non-patch `pub object T` exists in the same source file
     * (regular object's methods survive, patch's don't). */
    bool               is_patch_member;
} Iron_MethodDecl;

/* ── Helper node types ───────────────────────────────────────────────────── */

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_PARAM */
    const char   *name;
    Iron_Node    *type_ann;  /* NULL if inferred */
    bool          is_var;
    /* Phase 79 MUT-01: true when this param is a receiver-binding declared
     * with the `mut` prefix (`func (mut t: Timer) update(...)`). Always
     * false for non-receiver params — the parser rejects `mut` on regular
     * parameters per REQUIREMENTS.md Out-of-Scope table. Phase 79 stops at
     * the AST level (this field's presence + correct population); Phase 80
     * adds resolver/typechecker enforcement.
     *
     * Field name locked by CONTEXT.md Decisions section; mirrors the
     * existing `bool is_mutable` pattern on capture entries at line 19. */
    bool          is_mut_receiver;
} Iron_Param;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_FIELD */
    const char   *name;
    Iron_Node    *type_ann;
    bool          is_var;
    /* Phase 83 ACCESS-02: true when the field was declared with the `pub`
     * modifier inside an `object X { ... }` body. Plan 83-02 reads this bit
     * to decide whether to synthesize accessor methods. Default false;
     * Phase 88 BREAK may flip the default to public-by-default. */
    bool          is_pub;
    /* Phase 3 NAV-14: arena-interned `///` run; NULL if none. */
    const char   *doc_comment;
} Iron_Field;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_ENUM_VARIANT */
    const char   *name;
    bool          has_explicit_value;  /* true when variant has = N */
    int           explicit_value;      /* only valid when has_explicit_value */
    Iron_Node   **payload_type_anns;   /* array of IRON_NODE_TYPE_ANNOTATION nodes; NULL if plain */
    int           payload_count;       /* 0 for plain variants */
    bool         *payload_is_boxed;    /* [payload_count]; true if field is recursive (auto-boxed) */
    /* Phase 3 NAV-14: arena-interned `///` run; NULL if none. */
    const char   *doc_comment;
} Iron_EnumVariant;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_TYPE_ANNOTATION */
    const char   *name;
    bool          is_nullable;
    Iron_Node   **generic_args;
    int           generic_arg_count;
    bool          is_array;
    Iron_Node    *array_size;  /* NULL if dynamic/no size */
    /* Phase 33: function type annotations — func(T, U) -> R */
    bool          is_func;
    Iron_Node   **func_params;      /* array of Iron_TypeAnnotation* for param types */
    int           func_param_count;
    Iron_Node    *func_return;      /* return type annotation, NULL means void */
    /* Phase 48: layout annotations for array types [T, layout: soa/aos] [T, unordered] */
    int           layout_hint;      /* 0 = none, 1 = soa, 2 = aos */
    bool          is_unordered;     /* true if [T, unordered] */
    /* Phase 59 01d: tuple type annotation — (T0, T1, ...) */
    bool          is_tuple;
    Iron_Node   **tuple_elems;      /* array of Iron_TypeAnnotation* for element types */
    int           tuple_elem_count;
    /* Phase 87-02 SELF-01/02: true when the type annotation is literally the
     * identifier "Self" in return-type position of a method or interface sig.
     * Set by iron_parse_type_annotation when ann->name == "Self". The
     * typechecker resolves is_self_type to the enclosing ObjectDecl type via
     * TypeCtx.enclosing_type_name, or emits E0259 if used outside a
     * method/interface context. Defaults false at every allocation site. */
    bool          is_self_type;
} Iron_TypeAnnotation;

#define IRON_LAYOUT_HINT_NONE 0
#define IRON_LAYOUT_HINT_SOA  1
#define IRON_LAYOUT_HINT_AOS  2

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;            /* IRON_NODE_PATTERN */
    const char   *enum_name;       /* "Shape" in Shape.Circle(r) */
    const char   *variant_name;    /* "Circle" */
    const char  **binding_names;   /* stb_ds array; NULL entries = wildcard _ */
    Iron_Node   **nested_patterns; /* stb_ds array; NULL entries = simple binding */
    int           binding_count;
} Iron_Pattern;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;           /* IRON_NODE_ENUM_CONSTRUCT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    const char        *enum_name;      /* "Shape" */
    const char        *variant_name;   /* "Circle" */
    Iron_Node        **args;           /* argument expressions; NULL for plain variants */
    int                arg_count;
} Iron_EnumConstruct;

/* ── Statements ──────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;  /* IRON_NODE_VAL_DECL */
    const char        *name;       /* single-name path; ignored when binding_count > 0 */
    Iron_Node         *type_ann;   /* NULL if inferred */
    Iron_Node         *init;
    struct Iron_Type  *declared_type;  /* set by type checker */
    /* Phase 59 01d: tuple destructure bindings — val (a, b, ...) = init.
     * binding_count == 0 means the single-name `name` field is authoritative.
     * NULL entries in binding_names[] mean wildcard (`_`). */
    const char       **binding_names;
    int                binding_count;
} Iron_ValDecl;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;  /* IRON_NODE_VAR_DECL */
    const char        *name;
    Iron_Node         *type_ann;  /* NULL if inferred */
    Iron_Node         *init;
    struct Iron_Type  *declared_type;  /* set by type checker */
} Iron_VarDecl;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_ASSIGN */
    Iron_Node    *target;
    Iron_Node    *value;
    Iron_OpKind   op;  /* ASSIGN, PLUS_ASSIGN, etc. */
    /* Phase 83-02 ACCESS-05: set to true by the typechecker when the LHS is
     * a field access on a `pub var` field. HIR reads this bit to lower the
     * assign as a `set_<field>(value)` method call against the synthesized
     * setter instead of emitting a direct field store. Default false at
     * every construction site; arena zero-init covers non-pub assigns. */
    bool          is_pub_setter;
} Iron_AssignStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_RETURN */
    Iron_Node    *value;  /* NULL if bare return */
} Iron_ReturnStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;        /* IRON_NODE_IF */
    Iron_Node    *condition;
    Iron_Node    *body;
    Iron_Node   **elif_conds;  /* stb_ds array */
    Iron_Node   **elif_bodies; /* stb_ds array */
    int           elif_count;
    Iron_Node    *else_body;   /* NULL if no else */
} Iron_IfStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;      /* IRON_NODE_WHILE */
    Iron_Node    *condition;
    Iron_Node    *body;
} Iron_WhileStmt;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;        /* IRON_NODE_FOR */
    const char        *var_name;
    Iron_Node         *iterable;
    Iron_Node         *body;
    bool               is_parallel;
    Iron_Node         *pool_expr;   /* NULL if default pool */
    Iron_CaptureEntry *pfor_captures;      /* set by capture analysis for parallel-for; NULL otherwise */
    int                pfor_capture_count; /* 0 for non-capturing pfor bodies */
} Iron_ForStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;       /* IRON_NODE_MATCH */
    Iron_Node    *subject;
    Iron_Node   **cases;
    int           case_count;
    Iron_Node    *else_body;  /* NULL if no else */
} Iron_MatchStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;     /* IRON_NODE_MATCH_CASE */
    Iron_Node    *pattern;
    Iron_Node    *body;
} Iron_MatchCase;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_DEFER */
    Iron_Node    *expr;
} Iron_DeferStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_FREE */
    Iron_Node    *expr;
} Iron_FreeStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_LEAK */
    Iron_Node    *expr;
} Iron_LeakStmt;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;          /* IRON_NODE_SPAWN */
    const char        *name;
    Iron_Node         *pool_expr;     /* NULL if default */
    Iron_Node         *body;
    const char        *handle_name;  /* NULL if no handle */
    Iron_CaptureEntry *captures;      /* set by capture analysis; NULL before analysis */
    int                capture_count; /* 0 for non-capturing spawn blocks */
} Iron_SpawnStmt;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_BLOCK */
    Iron_Node   **stmts;
    int           stmt_count;
} Iron_Block;

/* ── Expressions ─────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_INT_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    const char        *value;
} Iron_IntLit;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_FLOAT_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    const char        *value;
} Iron_FloatLit;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_STRING_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    const char        *value;
} Iron_StringLit;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;       /* IRON_NODE_INTERP_STRING */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node        **parts;      /* alternating string lit and expr */
    int                part_count;
} Iron_InterpString;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_BOOL_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    bool               value;
} Iron_BoolLit;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_NULL_LIT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
} Iron_NullLit;

typedef struct {
    Iron_Span           span;
    Iron_NodeKind       kind;  /* IRON_NODE_IDENT */
    struct Iron_Type   *resolved_type;  /* set by type checker */
    const char         *name;
    struct Iron_Symbol *resolved_sym;   /* set by resolver; NULL = unresolved */
    const char         *constraint_name;  /* generic param constraint; NULL if unconstrained */
} Iron_Ident;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_BINARY */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *left;
    Iron_OpKind        op;
    Iron_Node         *right;
    /* Phase 96 STR-01: set true by typecheck when op == IRON_TOK_PLUS AND
     * both operands resolve to IRON_TYPE_STRING. hir_lower reads this bit
     * to lower the binop as a runtime call to iron_string_concat instead
     * of an HIR ADD opcode. Default false at every parser allocation site
     * (parser.c:1357, where bin->kind = IRON_NODE_BINARY); explicit
     * assignment mirrors the Phase 93 is_pub explicit-assignment audit
     * convention so future maintainers can grep all init sites at once. */
    bool               is_string_concat;
} Iron_BinaryExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;     /* IRON_NODE_UNARY */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_OpKind        op;
    Iron_Node         *operand;
} Iron_UnaryExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;     /* IRON_NODE_CALL */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *callee;
    Iron_Node        **args;
    int                arg_count;
    bool               is_primitive_cast; /* true when Float(x), Int(x), etc. */
} Iron_CallExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;        /* IRON_NODE_METHOD_CALL */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *object;
    const char        *method;
    Iron_Node        **args;
    int                arg_count;
} Iron_MethodCallExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_FIELD_ACCESS */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *object;
    const char        *field;
    /* Phase 83-02 ACCESS-05: set to true by the typechecker when the matched
     * field on the object has is_pub=true. HIR reads this bit to lower the
     * access as a zero-arg method call on the synthesized getter instead of
     * emitting a direct field load. Default false at every construction
     * site; arena zero-init covers non-pub field accesses. */
    bool               is_pub_access;
} Iron_FieldAccess;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_INDEX */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *object;
    Iron_Node         *index;
} Iron_IndexExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_SLICE */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *object;
    Iron_Node         *start;  /* NULL if omitted */
    Iron_Node         *end;    /* NULL if omitted */
} Iron_SliceExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;         /* IRON_NODE_LAMBDA */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node        **params;
    int                param_count;
    Iron_Node         *return_type;  /* NULL if inferred */
    Iron_Node         *body;
    Iron_CaptureEntry *captures;      /* set by capture analysis; NULL before analysis */
    int                capture_count; /* 0 for non-capturing lambdas */
} Iron_LambdaExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_HEAP */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *inner;
    bool               auto_free;  /* set by escape analyzer */
    bool               escapes;    /* set by escape analyzer */
} Iron_HeapExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_RC */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *inner;
} Iron_RcExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;   /* IRON_NODE_COMPTIME */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *inner;
} Iron_ComptimeExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;       /* IRON_NODE_IS */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    Iron_Node         *expr;
    const char        *type_name;
} Iron_IsExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;            /* IRON_NODE_AWAIT */
    struct Iron_Type  *resolved_type;   /* set by type checker */
    Iron_Node         *handle;
} Iron_AwaitExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;           /* IRON_NODE_CONSTRUCT */
    struct Iron_Type  *resolved_type;  /* set by type checker */
    const char        *type_name;
    Iron_Node        **args;
    int                arg_count;
    Iron_Node        **generic_args;
    int                generic_arg_count;
} Iron_ConstructExpr;

typedef struct {
    Iron_Span          span;
    Iron_NodeKind      kind;          /* IRON_NODE_ARRAY_LIT */
    struct Iron_Type  *resolved_type; /* set by type checker */
    Iron_Node         *type_ann;      /* NULL if inferred */
    Iron_Node         *size;          /* NULL if dynamic */
    Iron_Node        **elements;
    int                element_count;
} Iron_ArrayLit;

typedef struct {
    Iron_Span     span;
    Iron_NodeKind kind;  /* IRON_NODE_ERROR */
} Iron_ErrorNode;

/* ── Visitor pattern ─────────────────────────────────────────────────────── */

typedef struct Iron_Visitor {
    void *ctx;
    /* Called before visiting children. Return true to recurse into children. */
    bool (*visit_node)(struct Iron_Visitor *v, Iron_Node *node);
    /* Called after visiting children. May be NULL. */
    void (*post_visit)(struct Iron_Visitor *v, Iron_Node *node);
} Iron_Visitor;

/* Walk the AST rooted at `root`, calling v->visit_node on each node.
 * If visit_node returns true, children are visited recursively.
 * After children, v->post_visit is called (if non-NULL). */
void iron_ast_walk(Iron_Node *root, Iron_Visitor *v);

/* Return a human-readable name for the node kind. */
const char *iron_node_kind_str(Iron_NodeKind kind);

/* ── Expression prefix layout enforcement (PROT-01) ──────────────────────────
 * Each expression AST type must begin with {Iron_Span span; Iron_NodeKind kind;
 * struct Iron_Type *resolved_type;} in that exact order. The compile-time
 * asserts below lock this layout so that `Iron_ExprNode *` reads correctly
 * regardless of the concrete expression type, and so that any future expression
 * type is forced to adopt the prefix.
 *
 * If one of these asserts fires: either the new type is not an expression
 * (remove it from the list) or its fields need reordering to match the prefix.
 */
#define IRON_ASSERT_EXPR_PREFIX(T)                                              \
    _Static_assert(offsetof(T, span) == offsetof(Iron_ExprNode, span),          \
                   #T " must begin with Iron_Span span");                       \
    _Static_assert(offsetof(T, kind) == offsetof(Iron_ExprNode, kind),          \
                   #T " must have Iron_NodeKind kind after span");              \
    _Static_assert(offsetof(T, resolved_type) ==                                \
                       offsetof(Iron_ExprNode, resolved_type),                  \
                   #T " must have resolved_type after kind");                   \
    _Static_assert(sizeof(((T*)0)->span) == sizeof(Iron_Span),                  \
                   #T " span field size mismatch");                             \
    _Static_assert(sizeof(((T*)0)->kind) == sizeof(Iron_NodeKind),              \
                   #T " kind field size mismatch")

IRON_ASSERT_EXPR_PREFIX(Iron_IntLit);
IRON_ASSERT_EXPR_PREFIX(Iron_FloatLit);
IRON_ASSERT_EXPR_PREFIX(Iron_StringLit);
IRON_ASSERT_EXPR_PREFIX(Iron_InterpString);
IRON_ASSERT_EXPR_PREFIX(Iron_BoolLit);
IRON_ASSERT_EXPR_PREFIX(Iron_NullLit);
IRON_ASSERT_EXPR_PREFIX(Iron_Ident);
IRON_ASSERT_EXPR_PREFIX(Iron_BinaryExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_UnaryExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_CallExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_MethodCallExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_FieldAccess);
IRON_ASSERT_EXPR_PREFIX(Iron_IndexExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_SliceExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_LambdaExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_HeapExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_RcExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_ComptimeExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_IsExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_AwaitExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_ConstructExpr);
IRON_ASSERT_EXPR_PREFIX(Iron_ArrayLit);
IRON_ASSERT_EXPR_PREFIX(Iron_EnumConstruct);

#undef IRON_ASSERT_EXPR_PREFIX

#endif /* IRON_AST_H */
