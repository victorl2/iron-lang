/* typecheck.c — Type checking pass for Iron.
 *
 * Walks the name-resolved AST and:
 *   1. Annotates every expression node with resolved_type.
 *   2. Annotates val/var decls with declared_type.
 *   3. Annotates func/method decls with resolved_return_type.
 *   4. Checks: type assignments, return types, val immutability, nullable
 *      access, flow-sensitive narrowing, interface completeness,
 *      ConstructExpr disambiguation.
 *
 * Scope strategy:
 *   The type checker mirrors the resolver's scope structure, pushing/popping
 *   scopes as it enters functions and blocks, and defining symbols as it
 *   encounters val/var/param declarations.
 *
 *   For IDENT lookup, the type checker first checks the narrowing map, then
 *   looks up in the type-checker scope chain.  This guarantees param types
 *   (set at function entry) are visible to the body.
 *
 *   For ASSIGN mutability: uses resolved_sym->is_mutable directly (set by
 *   the resolver) since that's the authoritative source of mutability.
 *
 * No implicit numeric conversions — Int and Float are distinct types.
 * Narrowing map: stb_ds hash map from symbol name to narrowed type.
 */

#include "analyzer/typecheck.h"
#include "analyzer/resolve.h"
#include "lexer/lexer.h"
#include "util/strbuf.h"
#include "vendor/stb_ds.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>

/* ── Type checker context ────────────────────────────────────────────────── */

typedef struct {
    char        *key;   /* stb_ds strdup key */
    Iron_Type   *value;
} NarrowEntry;

typedef struct {
    char        *key;   /* stb_ds strdup key -- handle name */
    Iron_Type   *value; /* spawn body return type */
} SpawnResultEntry;

typedef struct {
    char        *key;   /* stb_ds strdup key -- mangled name */
    Iron_Type   *value; /* the in-progress or completed mono type */
} MonoRegistryEntry;

/* Phase 85 INIT: branch-local definite-assignment set entry. stb_ds
 * string-set where presence of a key means the named field is still
 * unassigned on the current control-flow path. value is a sentinel (always
 * 1 when present). Use sh_new_strdup(...) to allocate, shput/shget/shdel
 * to mutate, shlen to query size, shfree to release. */
typedef struct {
    const char  *key;
    int          value;
} InitUnassignedEntry;

typedef struct {
    Iron_Arena        *arena;
    Iron_DiagList     *diags;
    Iron_Scope        *global_scope;
    Iron_Scope        *current_scope;   /* type-checker's own scope chain */
    Iron_Type         *current_return_type;  /* expected return type; NULL outside funcs */
    const char        *current_method_type;  /* owning type name if in method */
    /* Phase 83-02 ACCESS-05: true when the currently-checked method is a
     * parser-synthesized accessor (getter or setter for a pub field). The
     * typechecker uses this to suppress its own is_pub_access/is_pub_setter
     * rewrites inside accessor bodies — the synth getter's `self.field` read
     * and synth setter's `self.field = _v` write are supposed to be direct
     * field accesses, otherwise HIR would infinitely re-dispatch through the
     * same accessor. */
    bool               in_synth_accessor;
    /* Phase 84 MUTTIER-02/03: tracks whether the enclosing method is readonly
     * or pure. Both bits saved/restored around every method body in
     * check_method_decl (mirrors in_synth_accessor pattern). `in_readonly_method`
     * is set when the method is readonly OR pure — pure implies readonly for
     * self-writes, so the self-write check branches on `in_readonly_method`
     * and picks the tier-specific error code based on `in_pure_method`. */
    bool               in_readonly_method;
    bool               in_pure_method;
    /* Phase 85 INIT-04/05/06/09/10/11/12/14: true when the enclosing method
     * is an is_init=true MethodDecl. Plan 85-02 reads this bit to drive the
     * definite-assignment analysis (unassigned_fields below) and to gate
     * the seven INIT error codes (E0246..E0252). Saved/restored around every
     * method body in check_method_decl (mirrors in_synth_accessor). */
    bool               in_init_method;
    /* Phase 24 DROP-01/06 (Plan 24-02): true when the enclosing method is a
     * drop or copy block respectively. Saved/restored around every method body
     * in check_method_decl (mirrors in_init_method pattern). Used to gate
     * E0288 (drop early-return) and to enable copy-site context. */
    bool               in_drop_method;
    bool               in_copy_method;
    /* Phase 85 INIT-05: the Iron_Node* currently being checked as an
     * assignment's LHS. The IRON_NODE_ASSIGN handler sets this to `as->target`
     * around its check_expr call; the IRON_NODE_FIELD_ACCESS handler
     * compares pointer equality to suppress the E0246 read-before-assign
     * check on the write path (`self.x = ...` must not count as a read of
     * self.x). Set back to NULL after the target is checked. Only the
     * OUTERMOST field-access node in the chain matches this pointer — a
     * nested `self.inner` read inside `self.inner.field = v` still surfaces
     * E0246 if `inner` is unassigned. */
    Iron_Node         *cur_assign_target;
    /* Phase 85 INIT-04/05/06/12: branch-local definite-assignment set.
     * stb_ds string-set — presence of a field name => the field is still
     * unassigned on the current control-flow path. Populated at init body
     * entry with every field on the enclosing ObjectDecl; removed on
     * self.<field> writes; union-merged across then/else branches; saved
     * and restored around loop bodies (possibly-zero-iteration correctness).
     * Freed and the previous pointer restored at init body exit. */
    InitUnassignedEntry *unassigned_fields;
    NarrowEntry       *narrowed;             /* stb_ds map: sym name -> narrowed type */
    Iron_Program      *program;              /* for method return type lookup */
    SpawnResultEntry  *spawn_result_types;   /* stb_ds map: handle_name -> body return type */
    MonoRegistryEntry *mono_registry;        /* stb_ds map: mangled_name -> mono Iron_Type* (cycle detection + caching) */
    /* Phase 87-02 SELF-01/02/03: name of the enclosing ObjectDecl when
     * checking a method body or resolving a method's return type. Set at
     * check_method_decl entry; restored at exit. NULL at top-level scope.
     * Consulted by resolve_type_annotation when ann->is_self_type is true
     * to substitute the concrete enclosing type instead of Self. */
    const char        *enclosing_type_name;
    const _Atomic bool *cancel_flag;         /* HARD-05: NULL means never cancel */
} TypeCtx;

/* ── Cancellation helper (HARD-05) ─────────────────────────────────────────── */
static inline bool iron_cancel_requested(const _Atomic bool *flag) {
    return flag != NULL && atomic_load_explicit(flag, memory_order_relaxed);
}

/* ── Forward declarations ────────────────────────────────────────────────── */

static Iron_Type *check_expr(TypeCtx *ctx, Iron_Node *node);
static Iron_Type *check_expr_with_expected(TypeCtx *ctx, Iron_Node *node,
                                            Iron_Type *expected);
static void check_stmt(TypeCtx *ctx, Iron_Node *node);
static void check_block_stmts(TypeCtx *ctx, Iron_Node **stmts, int count);
static Iron_Type *resolve_type_annotation(TypeCtx *ctx, Iron_Node *ann_node);

/* ── Mangling helpers ────────────────────────────────────────────────────── */

/* Return the C-identifier-safe name component for a type when building a
 * monomorphized enum's mangled name.
 *
 * For primitives, return the plain name (e.g. "Int", "String").
 * For a monomorphized generic enum, strip the "Iron_" prefix from the
 * mangled_name (e.g. "Iron_Option_Int" -> "Option_Int").
 * For a non-generic enum, return the enum decl name.
 * This ensures nested generics like Result[Option[Int], String] produce
 * "Iron_Result_Option_Int_String" (valid C identifier). */
static const char *type_mangle_component(const Iron_Type *t, Iron_Arena *arena) {
    if (!t) return "unknown";
    switch ((int)(t->kind)) {
        case IRON_TYPE_INT:    return "Int";
        case IRON_TYPE_INT8:   return "Int8";
        case IRON_TYPE_INT16:  return "Int16";
        case IRON_TYPE_INT32:  return "Int32";
        case IRON_TYPE_INT64:  return "Int64";
        case IRON_TYPE_UINT:   return "UInt";
        case IRON_TYPE_UINT8:  return "UInt8";
        case IRON_TYPE_UINT16: return "UInt16";
        case IRON_TYPE_UINT32: return "UInt32";
        case IRON_TYPE_UINT64: return "UInt64";
        case IRON_TYPE_FLOAT:  return "Float";
        case IRON_TYPE_FLOAT32: return "Float32";
        case IRON_TYPE_FLOAT64: return "Float64";
        case IRON_TYPE_BOOL:   return "Bool";
        case IRON_TYPE_STRING: return "String";
        case IRON_TYPE_VOID:   return "void";
        case IRON_TYPE_ENUM:
            if (t->enu.mangled_name) {
                /* Strip "Iron_" prefix: "Iron_Option_Int" -> "Option_Int" */
                const char *mn = t->enu.mangled_name;
                if (strncmp(mn, "Iron_", 5) == 0) return mn + 5;
                return mn;
            }
            if (t->enu.decl) return t->enu.decl->name;
            return "Enum";
        /* -Wswitch-enum opt-out: composite kinds (OBJECT, INTERFACE, ARRAY,
         * NULLABLE, FUNC, TUPLE, POINTER, ERROR, NULL) flow through the
         * iron_type_to_string fallback which already handles each variant. */
        default:
            /* Fallback: use iron_type_to_string but replace brackets with underscores */
            return iron_type_to_string(t, arena);
    }
}

/* ── ADT helpers ─────────────────────────────────────────────────────────── */

/* Find variant index by name in an enum declaration. Returns -1 if not found. */
static int find_variant_index(Iron_EnumDecl *ed, const char *name) {
    for (int i = 0; i < ed->variant_count; i++) {
        Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[i];
        if (strcmp(ev->name, name) == 0) return i;
    }
    return -1;
}

/* ── Scope helpers ───────────────────────────────────────────────────────── */

static void tc_push_scope(TypeCtx *ctx, Iron_ScopeKind kind) {
    ctx->current_scope = iron_scope_create(ctx->arena, ctx->current_scope, kind);
}

static void tc_pop_scope(TypeCtx *ctx) {
    if (ctx->current_scope && ctx->current_scope->parent) {
        ctx->current_scope = ctx->current_scope->parent;
    }
}

/* Define a symbol in the type-checker's current scope.
 * Silently ignores duplicates (resolver already reported those). */
static Iron_Symbol *tc_define(TypeCtx *ctx, const char *name, Iron_SymbolKind kind,
                               Iron_Node *decl, Iron_Span span,
                               bool is_mutable, Iron_Type *type) {
    Iron_Symbol *sym = iron_symbol_create(ctx->arena, name, kind, decl, span);
    sym->is_mutable = is_mutable;
    sym->type = type;
    iron_scope_define(ctx->current_scope, ctx->arena, sym);
    return sym;
}

/* Look up a symbol in the type-checker's scope chain. */
static Iron_Symbol *tc_lookup(TypeCtx *ctx, const char *name) {
    return iron_scope_lookup(ctx->current_scope, name);
}

/* Phase 18 PARM-03: derive whether an argument expression yields a
 * mutable source.
 *
 * Walks IRON_NODE_IDENT and IRON_NODE_FIELD_ACCESS chains; every other
 * expression kind is conservatively NOT mutable (literals, calls, binops,
 * unary, casts, lambdas, struct/list/map literals are all rvalues).
 *
 * For a FIELD_ACCESS, both (a) the named field's is_var bit AND (b) the
 * root binding's is_mutable must be true — a `var` field on a `val`-rooted
 * object is still effectively immutable; a `val` field on a `var`-rooted
 * object is also immutable (the field's storage class wins).
 *
 * Mirrors the chain walk pattern at typecheck.c:4075-4095 (Phase 80
 * MUT-03 / Phase 17 VAL-03). IRON_TYPE_RC unwrapping handles `rc Box`
 * receivers transparently. */
static bool arg_source_is_mutable(TypeCtx *ctx, Iron_Node *arg) {
    if (!arg) return false;
    switch ((int)arg->kind) {
        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)arg;
            if (id->resolved_sym) return id->resolved_sym->is_mutable;
            Iron_Symbol *s = id->name ? tc_lookup(ctx, id->name) : NULL;
            return s ? s->is_mutable : false;
        }
        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)arg;
            /* Field-level mutability: walk obj's resolved_type → ObjectDecl,
             * find the field, check is_var. Combined with root-binding
             * mutability (recursive). Both must be mutable for the source
             * to count as mutable. */
            Iron_Type *obj_ty = fa->object
                ? ((Iron_ExprNode *)fa->object)->resolved_type : NULL;
            if (obj_ty && obj_ty->kind == IRON_TYPE_RC) obj_ty = obj_ty->rc.inner;
            bool field_mut = false;
            if (obj_ty && obj_ty->kind == IRON_TYPE_OBJECT && obj_ty->object.decl) {
                Iron_ObjectDecl *od = obj_ty->object.decl;
                for (int fi = 0; fi < od->field_count; fi++) {
                    Iron_Field *f = (Iron_Field *)od->fields[fi];
                    if (f && f->name && fa->field &&
                        strcmp(f->name, fa->field) == 0) {
                        field_mut = f->is_var;
                        break;
                    }
                }
            }
            return field_mut && arg_source_is_mutable(ctx, fa->object);
        }
        /* Calls, literals, binops, unary, struct-literal, list-literal,
         * map-literal, casts, lambdas — all rvalues, not mutable sources. */
        default:
            return false;
    }
}

/* Phase 20 PTR-07 (Plan 20-02a): true when `expr` is a syntactic lvalue
 * that auto-address can target (named binding, field, or array element).
 * Function-call results, literals, binops, and explicit `&` are all
 * rvalues for the purposes of auto-address insertion. */
static bool is_lvalue_expression(const Iron_Node *expr) {
    if (!expr) return false;
    switch ((int)expr->kind) {
        case IRON_NODE_IDENT:        return true;
        case IRON_NODE_FIELD_ACCESS: return true;
        case IRON_NODE_INDEX:        return true;
        default:                     return false;
    }
}

/* Phase 20 PTR-07 (Plan 20-02a): set is_auto_address_target=true on the
 * given expression when its kind is one of the supported lvalue shapes
 * (IDENT, FIELD_ACCESS, INDEX). Pitfall 4 lock: flag-on-existing-node
 * pattern, NEVER synthesize a new Iron_UnaryExpr wrapper — formatter must
 * stay unaware so the parity-fmt gate keeps green. */
static void set_auto_address_target_flag(Iron_Node *expr) {
    if (!expr) return;
    switch ((int)expr->kind) {
        case IRON_NODE_IDENT:
            ((Iron_Ident *)expr)->is_auto_address_target = true;
            break;
        case IRON_NODE_FIELD_ACCESS:
            ((Iron_FieldAccess *)expr)->is_auto_address_target = true;
            break;
        case IRON_NODE_INDEX:
            ((Iron_IndexExpr *)expr)->is_auto_address_target = true;
            break;
        default:
            break;
    }
}

/* Phase 20 PTR-10 (Plan 20-02a): walk a chain of FIELD_ACCESS / INDEX
 * back to the rooted IRON_NODE_IDENT and return its resolved Iron_Symbol.
 * Used by (a) PTR-10 stack-escape detection at IRON_NODE_RETURN to ask
 * "is this an address of a stack-local?", and (b) mark_takes_local_addr
 * walker (analyzer.c) to flag functions whose body addresses any local.
 * Returns NULL if the chain doesn't terminate at an Iron_Ident or if the
 * ident has no resolved_sym (resolver error path). */
Iron_Symbol *iron_walk_to_root_binding(Iron_Node *expr) {
    while (expr) {
        switch ((int)expr->kind) {
            case IRON_NODE_IDENT: {
                Iron_Ident *id = (Iron_Ident *)expr;
                return id->resolved_sym;
            }
            case IRON_NODE_FIELD_ACCESS:
                expr = ((Iron_FieldAccess *)expr)->object;
                break;
            case IRON_NODE_INDEX:
                expr = ((Iron_IndexExpr *)expr)->object;
                break;
            default:
                return NULL;
        }
    }
    return NULL;
}

/* Phase 20 OQ-D (Plan 20-02a): pointee-size estimate for Ptr.cast[T]
 * compile-time check. Returns a coarse byte estimate sufficient to
 * distinguish "same-size" from "different-size" pointee types. Mirrors
 * the existing emit_estimate_type_size logic in src/lir/emit_structs.c
 * (8B fixed-width primitives, 16B String/Closure, 24B array, sum of
 * field sizes for objects). Returns -1 for IRON_TYPE_ERROR / NULL so
 * the caller can short-circuit. */
static int iron_type_pointee_size(const Iron_Type *t) {
    if (!t) return -1;
    switch ((int)t->kind) {
        case IRON_TYPE_INT8:
        case IRON_TYPE_UINT8:
        case IRON_TYPE_BOOL:
            return 1;
        case IRON_TYPE_INT16:
        case IRON_TYPE_UINT16:
            return 2;
        case IRON_TYPE_INT32:
        case IRON_TYPE_UINT32:
        case IRON_TYPE_FLOAT32:
            return 4;
        case IRON_TYPE_INT:
        case IRON_TYPE_INT64:
        case IRON_TYPE_UINT:
        case IRON_TYPE_UINT64:
        case IRON_TYPE_FLOAT:
        case IRON_TYPE_FLOAT64:
            return 8;
        case IRON_TYPE_STRING:
            return 16;
        case IRON_TYPE_FUNC:
            return 16;
        case IRON_TYPE_PTR:
            return 16;
        case IRON_TYPE_ARRAY:
            return 24;
        case IRON_TYPE_OBJECT: {
            if (!t->object.decl) return 8;
            Iron_ObjectDecl *od = t->object.decl;
            int total = 0;
            for (int i = 0; i < od->field_count; i++) {
                if (!od->fields[i] ||
                    od->fields[i]->kind != IRON_NODE_FIELD) continue;
                Iron_Field *f = (Iron_Field *)od->fields[i];
                if (!f->type_ann ||
                    f->type_ann->kind != IRON_NODE_TYPE_ANNOTATION) {
                    total += 8;
                    continue;
                }
                Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                if (ta->is_pointer)     total += 16;
                else if (ta->is_array)  total += 24;
                else if (ta->is_func)   total += 16;
                else if (ta->name && strcmp(ta->name, "String") == 0)
                                        total += 16;
                else                    total += 8;
            }
            return total > 0 ? total : 8;
        }
        case IRON_TYPE_NULLABLE:
            return iron_type_pointee_size(t->nullable.inner);
        default:
            return 8;
    }
}

/* Phase 20 PTR-10: returns true when `sym` represents a stack-local
 * variable (val/var binding inside a function body). Resolver tags such
 * bindings with sym_kind == IRON_SYM_VARIABLE and sets the symbol on the
 * function's local scope; top-level globals share IRON_SYM_VARIABLE but
 * live on the global scope. We discriminate via decl_node->kind: stack
 * locals come from IRON_NODE_VAL_DECL / IRON_NODE_VAR_DECL inside a
 * function, AND parameters (IRON_SYM_PARAM) are also stack-resident.
 *
 * For the conservative whole-function pessimistic detection (Pitfall 6),
 * any IRON_SYM_VARIABLE or IRON_SYM_PARAM source counts. Top-level
 * globals are excluded because their addresses (when supported in a
 * later phase) carry static-storage-duration generation, not stack. */
static bool sym_is_stack_local(const Iron_Symbol *sym) {
    if (!sym) return false;
    if (sym->sym_kind == IRON_SYM_PARAM) return true;
    if (sym->sym_kind != IRON_SYM_VARIABLE) return false;
    /* IRON_SYM_VARIABLE: differentiate top-level globals (decl from
     * top-level VAL_DECL / VAR_DECL but resolver registers in global
     * scope) vs locals (registered in function scope). The resolver
     * does not currently expose `scope_kind` on the symbol; use
     * decl_node->kind as a proxy — IRON_NODE_VAL_DECL / VAR_DECL are
     * the only decl kinds that produce IRON_SYM_VARIABLE. Conservative
     * choice: treat ALL IRON_SYM_VARIABLE as stack locals for PTR-10
     * (matches CONTEXT.md "whole-function pessimistic" lock). Top-level
     * globals are not common enough to matter, and the runtime panic
     * path (Plan 20-02b) is the safety net regardless. */
    return true;
}

/* Recursively define binding variables from a pattern into the current scope.
 * enum_type: the Iron_Type of the enum being matched by this pattern.
 * pattern_node: the Iron_Pattern AST node. */
static void tc_define_pattern_bindings(TypeCtx *ctx,
                                        Iron_Type *enum_type,
                                        Iron_Node *pattern_node) {
    if (!pattern_node || pattern_node->kind != IRON_NODE_PATTERN) return;
    Iron_Pattern *pat = (Iron_Pattern *)pattern_node;

    Iron_EnumDecl *ed = NULL;
    Iron_Type     *pat_enum_type = enum_type;

    /* If the pattern names its own enum (e.g. Inner.Val(n)), resolve by name */
    if (pat->enum_name) {
        Iron_Symbol *esym = iron_scope_lookup(ctx->global_scope, pat->enum_name);
        if (esym && esym->type && esym->type->kind == IRON_TYPE_ENUM) {
            pat_enum_type = esym->type;
        }
    }
    if (pat_enum_type && pat_enum_type->kind == IRON_TYPE_ENUM) {
        ed = pat_enum_type->enu.decl;
    }
    if (!ed || !pat_enum_type || !pat_enum_type->enu.variant_payload_types) return;

    int vi = find_variant_index(ed, pat->variant_name);
    if (vi < 0) return;

    Iron_Type **ptypes = pat_enum_type->enu.variant_payload_types[vi];
    Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[vi];
    for (int j = 0; j < pat->binding_count && j < ev->payload_count; j++) {
        const char *bname = pat->binding_names ? pat->binding_names[j] : NULL;
        Iron_Node  *nested = (pat->nested_patterns && pat->nested_patterns[j])
                              ? pat->nested_patterns[j] : NULL;
        if (bname) {
            /* Simple binding: define variable with payload type */
            Iron_Type *btype = (ptypes && ptypes[j]) ? ptypes[j]
                               : iron_type_make_primitive(IRON_TYPE_ERROR);
            tc_define(ctx, bname, IRON_SYM_VARIABLE, pattern_node, pat->span,
                      /*is_mutable=*/false, btype);
        } else if (nested) {
            /* Nested pattern: recurse with the payload type as the context enum type */
            Iron_Type *payload_type = (ptypes && ptypes[j]) ? ptypes[j] : NULL;
            tc_define_pattern_bindings(ctx, payload_type, nested);
        }
        /* else: wildcard _ — no binding */
    }
}

/* ── Diagnostic helpers ──────────────────────────────────────────────────── */

static void emit_error(TypeCtx *ctx, int code, Iron_Span span,
                       const char *msg, const char *suggestion) {
    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
    if (!msg_copy) { /* HARD-09 REPLACE (typecheck.c:emit_error msg) */ msg_copy = "analyzer error"; }
    const char *sug_copy = NULL;
    if (suggestion) {
        sug_copy = iron_arena_strdup(ctx->arena, suggestion, strlen(suggestion));
        if (!sug_copy) { /* HARD-09 REPLACE (typecheck.c:emit_error suggestion) */ sug_copy = NULL; }
    }
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, code, span,
                   msg_copy, sug_copy);
}

static void emit_warning(TypeCtx *ctx, int code, Iron_Span span,
                         const char *msg, const char *suggestion) {
    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
    if (!msg_copy) { /* HARD-09 REPLACE (typecheck.c:emit_warning msg) */ msg_copy = "analyzer error"; }
    const char *sug_copy = NULL;
    if (suggestion) {
        sug_copy = iron_arena_strdup(ctx->arena, suggestion, strlen(suggestion));
        if (!sug_copy) { /* HARD-09 REPLACE (typecheck.c:emit_warning suggestion) */ sug_copy = NULL; }
    }
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_WARNING, code, span,
                   msg_copy, sug_copy);
}

/* ── Phase 85 INIT helpers ──────────────────────────────────────────────────
 *
 * iron_find_init_by_name walks program->decls looking for a MethodDecl that
 * is an init (is_init=true) on the given type. init_name==NULL finds the
 * anonymous init (init_name==NULL on MethodDecl); a non-NULL init_name
 * finds a named init whose init_name matches. Returns NULL if no match.
 *
 * Callers:
 *   - IRON_NODE_CONSTRUCT handler: dispatch `Type(args)` to the anonymous
 *     init when one exists (Plan 85-02 Task 2 PART A).
 *   - IRON_NODE_METHOD_CALL handler: dispatch `Type.name(args)` to the named
 *     init (Plan 85-02 Task 2 PART B) AND detect `self.<named>` delegation
 *     inside init bodies (Plan 85-02 Task 1 E0251).
 */
static Iron_MethodDecl *iron_find_init_by_name(Iron_Program *program,
                                                const char *type_name,
                                                const char *init_name) {
    if (!program || !type_name) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->is_init) continue;
        if (!md->type_name || strcmp(md->type_name, type_name) != 0) continue;
        if (init_name == NULL) {
            if (md->init_name == NULL) return md;
        } else {
            if (md->init_name && strcmp(md->init_name, init_name) == 0) return md;
        }
    }
    return NULL;
}

/* Clone an InitUnassignedEntry stb_ds string-set. Allocates a fresh map,
 * copies every key from src (value sentinel = 1). Caller owns the result
 * and must shfree it. */
static void init_unassigned_clone(InitUnassignedEntry **out,
                                  InitUnassignedEntry  *src) {
    *out = NULL;
    sh_new_strdup(*out);
    if (!src) return;
    for (ptrdiff_t i = 0; i < shlen(src); i++) {
        shput(*out, src[i].key, 1);
    }
}

static int type_bit_width(const Iron_Type *t) {
    if (!t) return 0;
    switch ((int)(t->kind)) {
        case IRON_TYPE_INT8:   case IRON_TYPE_UINT8:   return 8;
        case IRON_TYPE_INT16:  case IRON_TYPE_UINT16:  return 16;
        case IRON_TYPE_INT32:  case IRON_TYPE_UINT32:  return 32;
        case IRON_TYPE_INT64:  case IRON_TYPE_UINT64:  return 64;
        case IRON_TYPE_INT:    case IRON_TYPE_UINT:    return 64;
        case IRON_TYPE_FLOAT32:                        return 32;
        case IRON_TYPE_FLOAT64: case IRON_TYPE_FLOAT:  return 64;
        case IRON_TYPE_BOOL:                           return 1;
        /* -Wswitch-enum opt-out: predicate is numeric-only; non-numeric kinds
         * return 0 meaning "no defined bit width". */
        default:                                       return 0;
    }
}

static bool value_fits_type(int64_t val, const Iron_Type *t) {
    if (!t) return false;
    switch ((int)(t->kind)) {
        case IRON_TYPE_INT8:   return val >= -128 && val <= 127;
        case IRON_TYPE_INT16:  return val >= -32768 && val <= 32767;
        case IRON_TYPE_INT32:  return val >= INT32_MIN && val <= INT32_MAX;
        case IRON_TYPE_INT64:  return true;
        case IRON_TYPE_INT:    return true;
        case IRON_TYPE_UINT8:  return val >= 0 && val <= 255;
        case IRON_TYPE_UINT16: return val >= 0 && val <= 65535;
        case IRON_TYPE_UINT32: return val >= 0 && (uint64_t)val <= UINT32_MAX;
        case IRON_TYPE_UINT64: return val >= 0;
        case IRON_TYPE_UINT:   return val >= 0;
        /* -Wswitch-enum opt-out: non-integer kinds are never passed an int
         * literal to check; the predicate defaults to "fits" so non-int
         * contexts don't emit spurious overflow diagnostics. */
        default:               return true;
    }
}

static bool is_narrow_integer(const Iron_Type *t) {
    if (!t) return false;
    switch ((int)(t->kind)) {
        case IRON_TYPE_INT8:  case IRON_TYPE_INT16:  case IRON_TYPE_INT32:
        case IRON_TYPE_UINT8: case IRON_TYPE_UINT16: case IRON_TYPE_UINT32:
            return true;
        /* -Wswitch-enum opt-out: predicate is strictly for sub-word integer
         * kinds; every other Iron_TypeKind is non-narrow. */
        default:
            return false;
    }
}

static bool is_compound_assign_op(Iron_OpKind op) {
    return op == IRON_TOK_PLUS_ASSIGN  ||
           op == IRON_TOK_MINUS_ASSIGN ||
           op == IRON_TOK_STAR_ASSIGN  ||
           op == IRON_TOK_SLASH_ASSIGN ||
           op == IRON_TOK_SHL_ASSIGN   ||
           op == IRON_TOK_SHR_ASSIGN   ||
           op == IRON_TOK_AMP_ASSIGN   ||
           op == IRON_TOK_PIPE_ASSIGN  ||
           op == IRON_TOK_CARET_ASSIGN;
}

static bool is_stringifiable(TypeCtx *ctx, const Iron_Type *t) {
    if (!t) return false;
    if (iron_type_is_numeric(t)) return true;
    if (t->kind == IRON_TYPE_BOOL) return true;
    if (t->kind == IRON_TYPE_STRING) return true;
    if (t->kind == IRON_TYPE_ENUM) return true;
    if (t->kind == IRON_TYPE_OBJECT && t->object.decl && ctx->program) {
        const char *tname = t->object.decl->name;
        for (int i = 0; i < ctx->program->decl_count; i++) {
            Iron_Node *d = ctx->program->decls[i];
            if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
            Iron_MethodDecl *md = (Iron_MethodDecl *)d;
            if (strcmp(md->type_name, tname) == 0 &&
                strcmp(md->method_name, "to_string") == 0) {
                return true;
            }
        }
    }
    return false;
}

static void emit_type_mismatch(TypeCtx *ctx, Iron_Span span,
                                Iron_Type *expected, Iron_Type *got) {
    char msg[512];
    const char *exp_s = expected ? iron_type_to_string(expected, ctx->arena) : "unknown";
    const char *got_s = got      ? iron_type_to_string(got, ctx->arena)      : "unknown";
    snprintf(msg, sizeof(msg),
             "type mismatch: expected '%s', got '%s'", exp_s, got_s);
    emit_error(ctx, IRON_ERR_TYPE_MISMATCH, span, msg, NULL);
}

/* Phase 4 Plan 04-01 (EDIT-07): narrow emit_type_mismatch for a literal RHS.
 *
 * When the init_node is an Iron_IntLit / Iron_FloatLit / Iron_BoolLit /
 * Iron_StringLit AND the expected type admits a simple retyping of that
 * literal, emit IRON_ERR_TYPE_MISMATCH_LITERAL (code 235) with a
 * retyped-literal .suggestion. Otherwise, fall back to the general
 * type-mismatch form. */
static void emit_type_mismatch_maybe_literal(TypeCtx *ctx, Iron_Span span,
                                              Iron_Type *expected,
                                              Iron_Type *got,
                                              Iron_Node *init_node) {
    const char *suggestion = NULL;
    bool is_literal_rhs = false;

    if (init_node && expected) {
        if (init_node->kind == IRON_NODE_INT_LIT) {
            Iron_IntLit *il = (Iron_IntLit *)init_node;
            is_literal_rhs = true;
            /* Int literal expected to be Float: "N" -> "N.0". */
            if (expected->kind == IRON_TYPE_FLOAT ||
                expected->kind == IRON_TYPE_FLOAT32 ||
                expected->kind == IRON_TYPE_FLOAT64) {
                if (il->value) {
                    size_t n = strlen(il->value) + 3; /* ".0" + NUL */
                    char *buf = (char *)iron_arena_alloc(ctx->arena, n, 1);
                    if (buf) {
                        snprintf(buf, n, "%s.0", il->value);
                        suggestion = buf;
                    }
                }
            }
        } else if (init_node->kind == IRON_NODE_FLOAT_LIT) {
            is_literal_rhs = true;
            Iron_FloatLit *fl = (Iron_FloatLit *)init_node;
            /* Float literal expected to be Int: truncate at '.'. */
            if (expected->kind == IRON_TYPE_INT ||
                expected->kind == IRON_TYPE_INT32 ||
                expected->kind == IRON_TYPE_INT64) {
                if (fl->value) {
                    const char *dot = strchr(fl->value, '.');
                    size_t head = dot ? (size_t)(dot - fl->value) : strlen(fl->value);
                    if (head == 0) head = 1; /* "0" as a fallback */
                    char *buf = (char *)iron_arena_alloc(ctx->arena, head + 1, 1);
                    if (buf) {
                        memcpy(buf, fl->value, head);
                        buf[head] = '\0';
                        suggestion = buf;
                    }
                }
            }
        } else if (init_node->kind == IRON_NODE_BOOL_LIT ||
                   init_node->kind == IRON_NODE_STRING_LIT) {
            is_literal_rhs = true;
        }
    }

    if (!is_literal_rhs) {
        /* Not a literal — use the existing general form (code 202). */
        emit_type_mismatch(ctx, span, expected, got);
        return;
    }

    /* Literal RHS: emit code 235 with message that still shows the general
     * "type mismatch: expected 'X', got 'Y'" phrasing so message parity
     * holds. If suggestion remains NULL, fall back to the bare expected-
     * type printed form so .suggestion is never NULL for a 235 emit. */
    char msg[512];
    const char *exp_s = expected ? iron_type_to_string(expected, ctx->arena) : "unknown";
    const char *got_s = got      ? iron_type_to_string(got, ctx->arena)      : "unknown";
    snprintf(msg, sizeof(msg),
             "type mismatch: expected '%s', got '%s'", exp_s, got_s);
    if (!suggestion) {
        /* Synthetic fallback suggestion: expected-type name. Keeps
         * .suggestion non-NULL without inventing arbitrary literal text. */
        suggestion = iron_arena_strdup(ctx->arena, exp_s, strlen(exp_s));
    }
    emit_error(ctx, IRON_ERR_TYPE_MISMATCH_LITERAL, span, msg, suggestion);
}

/* Implicit coercion rules for assignment compatibility.
 *
 * Currently only widening is implicit:
 * - Int32 -> Int: always safe, no data loss.
 *
 * Narrowing (Int -> Int32) is NOT implicit -- it requires either:
 *   (a) An integer literal (checked separately at each assignment site), or
 *   (b) An explicit Int32() cast expression.
 * Per user decision: "Narrowing requires explicit cast."
 */
static bool types_assignable(const Iron_Type *decl_t, const Iron_Type *init_t) {
    if (!decl_t || !init_t) return true;
    /* Phase 23 VEC: [T;<=N] and [T;N] are disjoint types — reject cross-assignment.
     * Caller specializes the diagnostic to E0283 when this returns false on
     * matching elem + matching size + differing is_bounded. */
    if (decl_t->kind == IRON_TYPE_ARRAY && init_t->kind == IRON_TYPE_ARRAY &&
        decl_t->array.size >= 0 && init_t->array.size >= 0 &&
        decl_t->array.is_bounded != init_t->array.is_bounded &&
        iron_type_equals(decl_t->array.elem, init_t->array.elem)) {
        return false;
    }
    if (iron_type_equals(decl_t, init_t)) return true;
    /* Int32 -> Int: implicit widening (always safe) */
    if (decl_t->kind == IRON_TYPE_INT && init_t->kind == IRON_TYPE_INT32) return true;

    /* Phase 20 PTR-13 (nullable accept): a `null` literal is assignable to
     * any nullable type `T?`. Required so `val p: ?*Point = null` analyzes
     * cleanly while `val p: *Point = null` still triggers the dedicated
     * IRON_ERR_PTR_NULL_DEREF path at the val/var binding site. This rule
     * also makes `val q: Int? = null` work at last; pre-Phase-20 the path
     * silently fell through to E0202 because no NULL -> NULLABLE rule
     * existed. */
    if (decl_t->kind == IRON_TYPE_NULLABLE && init_t->kind == IRON_TYPE_NULL) {
        return true;
    }

    /* Phase 25 PTR-02/03 (Plan 25-01): unchecked regime is DISJOINT from
     * the checked regime. (*T <-> *unchecked T) and (*var T <-> *var unchecked T)
     * are never assignable in either direction.
     * The caller emits IRON_ERR_PTR_REGIME_MISMATCH (289) when this returns
     * false and both kinds are IRON_TYPE_PTR (regime mismatch, not T mismatch).
     * RESEARCH Pattern 2 verbatim. Inserted BEFORE the PTR-12 covariance block
     * so regime isolation takes precedence over covariance. */
    if (decl_t->kind == IRON_TYPE_PTR && init_t->kind == IRON_TYPE_PTR) {
        if (decl_t->ptr.is_unchecked != init_t->ptr.is_unchecked) return false;
    }

    /* Phase 20 PTR-12: pointer covariance.
     *   *var T -> *T  : ALLOWED  (drop mutability is safe).
     *   *T     -> *var T : REJECTED (var-invariance).
     *   *T1    -> *T2 : REJECTED unless pointees structurally equal.
     *   T      <-> *T  : REJECTED (cross-kind).
     */
    if (decl_t->kind == IRON_TYPE_PTR && init_t->kind == IRON_TYPE_PTR) {
        if (!iron_type_equals(decl_t->ptr.pointee, init_t->ptr.pointee)) return false;
        if (decl_t->ptr.is_var && !init_t->ptr.is_var) return false;
        return true;
    }
    /* Cross-kind PTR <-> non-PTR is never assignable. NULL-literal
     * compatibility is handled separately (decl_t may be IRON_TYPE_NULLABLE
     * wrapping IRON_TYPE_PTR for `?*T`). */
    if (decl_t->kind == IRON_TYPE_PTR && init_t->kind != IRON_TYPE_PTR) return false;
    if (decl_t->kind != IRON_TYPE_PTR && init_t->kind == IRON_TYPE_PTR) return false;
    /* func-type compatibility: two func types with equal param counts are compatible
     * when their return types are both "void-like" (either IRON_TYPE_VOID or NULL).
     * This allows lambdas with unresolved return type (NULL) to be passed to
     * parameters typed as func() -> Void. */
    if (decl_t->kind == IRON_TYPE_FUNC && init_t->kind == IRON_TYPE_FUNC) {
        if (decl_t->func.param_count == init_t->func.param_count) {
            bool decl_void = (!decl_t->func.return_type ||
                              decl_t->func.return_type->kind == IRON_TYPE_VOID);
            bool init_void = (!init_t->func.return_type ||
                              init_t->func.return_type->kind == IRON_TYPE_VOID);
            if (decl_void && init_void) return true;
        }
    }
    /* Interface assignment: a concrete object type is assignable to an interface
     * type when the object declares `impl` for that interface. */
    if (decl_t->kind == IRON_TYPE_INTERFACE && init_t->kind == IRON_TYPE_OBJECT) {
        Iron_ObjectDecl *obj = init_t->object.decl;
        Iron_InterfaceDecl *iface = decl_t->interface.decl;
        if (obj && iface) {
            for (int i = 0; i < obj->implements_count; i++) {
                if (strcmp(obj->implements_names[i], iface->name) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Context-directed generic enum completion:
 * When `val r: Result[Int, String] = Result.Ok(100)`, only T=Int can be
 * inferred from the construct; E cannot be inferred from Ok's payload.
 * If the declared type is a fully-instantiated monomorphized enum with the
 * same base decl, copy its type_args into the construct's resolved_type so
 * the types match without a spurious type-mismatch error.
 * This does NOT validate that the inferred args are compatible — that is
 * enforced by the construct's own argument type-check above. */
static void maybe_fill_missing_generic_args(Iron_Node *init_node,
                                             Iron_Type *decl_type) {
    if (!init_node || !decl_type) return;
    if (decl_type->kind != IRON_TYPE_ENUM) return;
    if (!decl_type->enu.mangled_name) return;  /* decl_type not a monomorphized generic */
    if (init_node->kind != IRON_NODE_ENUM_CONSTRUCT) return;
    Iron_EnumConstruct *ec = (Iron_EnumConstruct *)init_node;
    if (!ec->resolved_type) return;
    if (ec->resolved_type->kind != IRON_TYPE_ENUM) return;
    if (ec->resolved_type->enu.decl != decl_type->enu.decl) return;  /* different base enum */
    /* Both are instantiations of the same generic enum.
     * Replace ec->resolved_type with decl_type to fill in any missing args. */
    ec->resolved_type = decl_type;
}

static bool try_get_constant_int(Iron_Node *node, long long *out);

/* Allow integer literals to implicitly narrow to any narrow integer type
 * when the literal value fits in the target range.
 * `val x: UInt8 = 255` is safe because 255 fits in UInt8.
 * `val x: UInt8 = someIntVar` is NOT allowed -- use UInt8(someIntVar).
 * A bare INT_LIT or `-INT_LIT` counts as a literal for this check.
 */
static bool is_int_literal_narrowing(const Iron_Type *decl_t, const Iron_Type *init_t,
                                     const Iron_Node *init_node) {
    if (!decl_t || !init_t || !init_node) return false;
    if (init_t->kind != IRON_TYPE_INT) return false;
    if (!is_narrow_integer(decl_t)) return false;
    long long val;
    if (!try_get_constant_int((Iron_Node *)init_node, &val)) return false;
    return value_fits_type(val, decl_t);
}

/* Try to extract a compile-time constant integer from an AST node.
 * Returns true if the node is a constant integer (INT_LIT or -INT_LIT),
 * and writes the value to *out. Returns false otherwise. */
static bool try_get_constant_int(Iron_Node *node, long long *out) {
    if (!node) return false;
    if (node->kind == IRON_NODE_INT_LIT) {
        Iron_IntLit *lit = (Iron_IntLit *)node;
        if (!lit->value) return false;
        errno = 0;
        long long v = strtoll(lit->value, NULL, 10);
        if (errno) return false;
        *out = v;
        return true;
    }
    /* Handle unary minus: -42 is UNARY(-, INT_LIT(42)) */
    if (node->kind == IRON_NODE_UNARY) {
        Iron_UnaryExpr *ue = (Iron_UnaryExpr *)node;
        if (ue->op == IRON_TOK_MINUS && ue->operand &&
            ue->operand->kind == IRON_NODE_INT_LIT) {
            Iron_IntLit *lit = (Iron_IntLit *)ue->operand;
            if (!lit->value) return false;
            errno = 0;
            long long v = strtoll(lit->value, NULL, 10);
            if (errno) return false;
            *out = -v;
            return true;
        }
    }
    return false;
}

/* ── Generic constraint helpers ──────────────────────────────────────────── */

/* Check if concrete_type satisfies the named constraint.
 * A constraint is satisfied if:
 *   (a) The constraint name resolves to an interface, and the concrete type
 *       is an object that declares `implements ConstraintName`, OR
 *   (b) The constraint name resolves to an interface, and the concrete type
 *       is an object that has methods matching all interface method signatures
 *       (structural check via program->decls scan).
 * Returns true if satisfied or if constraint cannot be resolved. */
static bool type_satisfies_constraint(TypeCtx *ctx, Iron_Type *concrete_type,
                                       const char *constraint_name) {
    if (!concrete_type || !constraint_name) return true;
    if (concrete_type->kind == IRON_TYPE_ERROR) return true;

    /* Look up the constraint as an interface */
    Iron_Symbol *csym = iron_scope_lookup(ctx->global_scope, constraint_name);
    if (!csym || csym->sym_kind != IRON_SYM_INTERFACE) return true;

    /* PROT-03 row 10 (AUDIT-01 M-severity): csym->decl_node may be NULL for
     * builtin interfaces with no source decl; guard then assert kind before
     * casting so a wrong-kind decl_node aborts in Debug instead of silently
     * misreading memory. */
    if (!csym->decl_node) return true;
    /* HARD-10 KEEP (audit row typecheck.c:474): csym->sym_kind is filtered to
     * IRON_SYM_INTERFACE above (line 467) which is only set by the resolver
     * for IRON_NODE_INTERFACE_DECL symbols — structural invariant. */
    IRON_NODE_ASSERT_KIND(csym->decl_node, IRON_NODE_INTERFACE_DECL);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)csym->decl_node;
    if (!iface) return true;

    /* Check (a): object explicitly implements the interface */
    if (concrete_type->kind == IRON_TYPE_OBJECT && concrete_type->object.decl) {
        Iron_ObjectDecl *od = concrete_type->object.decl;
        for (int i = 0; i < od->implements_count; i++) {
            if (strcmp(od->implements_names[i], constraint_name) == 0)
                return true;
        }
    }

    /* Check (b): structural -- object has all required methods */
    if (concrete_type->kind == IRON_TYPE_OBJECT && concrete_type->object.decl) {
        Iron_ObjectDecl *od = concrete_type->object.decl;
        bool all_found = true;
        for (int k = 0; k < iface->method_count; k++) {
            Iron_Node *sig = iface->method_sigs[k];
            if (!sig) continue;
            const char *mname = NULL;
            if (sig->kind == IRON_NODE_FUNC_DECL)
                mname = ((Iron_FuncDecl *)sig)->name;
            if (!mname) continue;

            bool found = false;
            for (int m = 0; m < ctx->program->decl_count; m++) {
                Iron_Node *d = ctx->program->decls[m];
                if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
                Iron_MethodDecl *meth = (Iron_MethodDecl *)d;
                if (strcmp(meth->type_name, od->name) == 0 &&
                    strcmp(meth->method_name, mname) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) { all_found = false; break; }
        }
        if (all_found) return true;
    }

    /* Primitives and other non-object types do not satisfy interface constraints */
    return false;
}

/* Check generic constraints for a declaration with generic params.
 * generic_params: array of Iron_Ident* nodes (from FuncDecl/ObjectDecl)
 * generic_param_count: number of generic params
 * concrete_types: array of Iron_Type* for each param
 * concrete_count: number of concrete types provided
 * span: source span for error reporting */
static void check_generic_constraints(TypeCtx *ctx,
                                       Iron_Node **generic_params,
                                       int generic_param_count,
                                       Iron_Type **concrete_types,
                                       int concrete_count,
                                       Iron_Span span) {
    int check_count = generic_param_count < concrete_count
                      ? generic_param_count : concrete_count;
    for (int i = 0; i < check_count; i++) {
        if (!generic_params[i]) continue;
        /* PROT-03 row 11 (AUDIT-01 M-severity): generic_params[] is a void**
         * of Iron_Ident*; assert the kind before casting so any future drift
         * (e.g., generic-param syntax growing constraints into a richer node)
         * aborts in Debug. */
        IRON_NODE_ASSERT_KIND(generic_params[i], IRON_NODE_IDENT);
        Iron_Ident *gp = (Iron_Ident *)generic_params[i];
        if (!gp->constraint_name) continue;

        Iron_Type *concrete = concrete_types[i];
        if (!type_satisfies_constraint(ctx, concrete, gp->constraint_name)) {
            char msg[512];
            const char *type_str = concrete
                ? iron_type_to_string(concrete, ctx->arena)
                : "unknown";
            snprintf(msg, sizeof(msg),
                     "type '%s' does not satisfy constraint '%s'",
                     type_str, gp->constraint_name);
            emit_error(ctx, IRON_ERR_GENERIC_CONSTRAINT, span, msg, NULL);
        }
    }
}

/* ── Narrowing map helpers ────────────────────────────────────────────────── */

static Iron_Type *narrowing_get(TypeCtx *ctx, const char *name) {
    int idx = shgeti(ctx->narrowed, name);
    if (idx < 0) return NULL;
    return ctx->narrowed[idx].value;
}

static void narrowing_set(TypeCtx *ctx, const char *name, Iron_Type *ty) {
    shput(ctx->narrowed, name, ty);
}

/* Deep-copy the current narrowing map for branch analysis */
static NarrowEntry *narrowing_copy(TypeCtx *ctx) {
    NarrowEntry *copy = NULL;
    sh_new_strdup(copy);
    int n = (int)shlenu(ctx->narrowed);
    for (int i = 0; i < n; i++) {
        shput(copy, ctx->narrowed[i].key, ctx->narrowed[i].value);
    }
    return copy;
}

/* ── Generic type substitution ───────────────────────────────────────────── */

/* Recursively substitute IRON_TYPE_GENERIC_PARAM with concrete types.
 * Maps ed->generic_params[i].name -> type_args[i]. */
/* ── Type annotation resolution ─────────────────────────────────────────── */

static Iron_Type *resolve_type_annotation(TypeCtx *ctx, Iron_Node *ann_node) {
    if (!ann_node) return iron_type_make_primitive(IRON_TYPE_VOID);
    /* HARD-05: cancel poll at type-annotation walker entry. */
    if (iron_cancel_requested(ctx->cancel_flag)) {
        return iron_type_make_primitive(IRON_TYPE_ERROR);
    }

    if (ann_node->kind != IRON_NODE_TYPE_ANNOTATION) {
        return iron_type_make_primitive(IRON_TYPE_ERROR);
    }

    Iron_TypeAnnotation *ann = (Iron_TypeAnnotation *)ann_node;

    /* Phase 20 PTR-01/13: lower `*T` / `*var T` / `?*T` / `?*var T`. The
     * outer is_nullable on a pointer annotation surfaces as
     * IRON_TYPE_NULLABLE wrapping IRON_TYPE_PTR — `?*T` composes the two
     * existing constructors. */
    if (ann->is_pointer) {
        Iron_Type *pointee_t = ann->pointer_pointee
            ? resolve_type_annotation(ctx, ann->pointer_pointee)
            : iron_type_make_primitive(IRON_TYPE_ERROR);
        if (!pointee_t) pointee_t = iron_type_make_primitive(IRON_TYPE_ERROR);
        Iron_Type *pt = iron_type_make_ptr(ctx->arena, pointee_t,
                                            ann->is_var_pointer,
                                            ann->is_unchecked); /* Phase 25 PTR-02 */
        if (!pt) pt = iron_type_make_primitive(IRON_TYPE_ERROR);
        if (ann->is_nullable) {
            Iron_Type *np = iron_type_make_nullable(ctx->arena, pt);
            return np ? np : iron_type_make_primitive(IRON_TYPE_ERROR);
        }
        return pt;
    }

    const char *name = ann->name;
    Iron_Type *base = NULL;

    /* Phase 87-02 SELF-01/02: resolve `Self` to the enclosing ObjectDecl type.
     * is_self_type is set by iron_parse_type_annotation when the identifier is
     * literally "Self". If we are inside a method body (enclosing_type_name is
     * set), look up the type in the global scope and return it. Otherwise emit
     * E0259 with the locked substring "'Self' is only valid". */
    if (ann->is_self_type) {
        if (ctx->enclosing_type_name) {
            Iron_Symbol *type_sym =
                iron_scope_lookup(ctx->global_scope, ctx->enclosing_type_name);
            if (type_sym && type_sym->type) {
                return ann->is_nullable
                    ? iron_type_make_nullable(ctx->arena, type_sym->type)
                    : type_sym->type;
            }
            /* enclosing_type_name is set but not yet registered (should not
             * happen in normal flow) — fall through to error type. */
        }
        /* Top-level or non-method context: emit E0259. */
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_SELF_OUTSIDE_CONTEXT,
                       ann_node->span,
                       "'Self' is only valid in method or interface signature "
                       "return types, not in free function or top-level contexts",
                       NULL);
        return iron_type_make_primitive(IRON_TYPE_ERROR);
    }

    /* Phase 59 01d: tuple-type annotation — (T0, T1, ...) */
    if (ann->is_tuple) {
        int n = ann->tuple_elem_count;
        Iron_Type **elem_types = (Iron_Type **)iron_arena_alloc(
            ctx->arena, sizeof(Iron_Type *) * (size_t)n,
            _Alignof(Iron_Type *));
        if (!elem_types) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation tuple_elems) */ return NULL; }
        for (int i = 0; i < n; i++) {
            elem_types[i] = ann->tuple_elems
                ? resolve_type_annotation(ctx, ann->tuple_elems[i])
                : iron_type_make_primitive(IRON_TYPE_ERROR);
            if (!elem_types[i]) {
                elem_types[i] = iron_type_make_primitive(IRON_TYPE_ERROR);
            }
        }
        return iron_type_make_tuple(ctx->arena, elem_types, n);
    }

    /* Phase 33: func-type annotation — func(T, U) -> R */
    if (ann->is_func) {
        /* Resolve parameter types */
        Iron_Type **param_types = NULL;
        int param_count = ann->func_param_count;
        if (param_count > 0) {
            param_types = iron_arena_alloc(ctx->arena, sizeof(Iron_Type *) * param_count, _Alignof(Iron_Type *));
            if (!param_types) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation func_params) */ return NULL; }
            for (int i = 0; i < param_count; i++) {
                param_types[i] = resolve_type_annotation(ctx, ann->func_params[i]);
            }
        }

        /* Resolve return type (NULL means void) */
        Iron_Type *ret = ann->func_return
            ? resolve_type_annotation(ctx, ann->func_return)
            : iron_type_make_primitive(IRON_TYPE_VOID);

        base = iron_type_make_func(ctx->arena, param_types, param_count, ret);
        /* HARD-09 CR-02: propagate NULL from OOM to IRON_TYPE_ERROR poison. */
        if (!base) base = iron_type_make_primitive(IRON_TYPE_ERROR);

        /* If this is an array-of-func, wrap in array type */
        if (ann->is_array) {
            int size = -1;
            if (ann->array_size && ann->array_size->kind == IRON_NODE_INT_LIT) {
                Iron_IntLit *il = (Iron_IntLit *)ann->array_size;
                if (il->value) size = (int)strtol(il->value, NULL, 10);
            }
            Iron_Type *arr = iron_type_make_array(ctx->arena, base, size, ann->bounded);
            /* HARD-09 CR-02: NULL-propagation fallback. */
            if (!arr) arr = iron_type_make_primitive(IRON_TYPE_ERROR);
            base = arr;
            /* Phase 48: propagate layout annotations; only safe if the wrapped
             * type is IRON_TYPE_ARRAY (not the IRON_TYPE_ERROR fallback). */
            if (base && base->kind == IRON_TYPE_ARRAY) {
                base->array.layout_hint  = ann->layout_hint;
                base->array.is_unordered = ann->is_unordered;
                /* Phase 23 VEC-01: propagate bounded flag from annotation to type */
                base->array.is_bounded   = ann->bounded;
            }
        }

        if (ann->is_nullable) {
            Iron_Type *nb = iron_type_make_nullable(ctx->arena, base);
            /* HARD-09 CR-02: NULL-propagation fallback. */
            base = nb ? nb : iron_type_make_primitive(IRON_TYPE_ERROR);
        }

        return base;
    }

    /* Check primitives by name */
    if      (strcmp(name, "Int")     == 0) base = iron_type_make_primitive(IRON_TYPE_INT);
    else if (strcmp(name, "Int8")    == 0) base = iron_type_make_primitive(IRON_TYPE_INT8);
    else if (strcmp(name, "Int16")   == 0) base = iron_type_make_primitive(IRON_TYPE_INT16);
    else if (strcmp(name, "Int32")   == 0) base = iron_type_make_primitive(IRON_TYPE_INT32);
    else if (strcmp(name, "Int64")   == 0) base = iron_type_make_primitive(IRON_TYPE_INT64);
    else if (strcmp(name, "UInt")    == 0) base = iron_type_make_primitive(IRON_TYPE_UINT);
    else if (strcmp(name, "UInt8")   == 0) base = iron_type_make_primitive(IRON_TYPE_UINT8);
    else if (strcmp(name, "UInt16")  == 0) base = iron_type_make_primitive(IRON_TYPE_UINT16);
    else if (strcmp(name, "UInt32")  == 0) base = iron_type_make_primitive(IRON_TYPE_UINT32);
    else if (strcmp(name, "UInt64")  == 0) base = iron_type_make_primitive(IRON_TYPE_UINT64);
    else if (strcmp(name, "Float")   == 0) base = iron_type_make_primitive(IRON_TYPE_FLOAT);
    else if (strcmp(name, "Float32") == 0) base = iron_type_make_primitive(IRON_TYPE_FLOAT32);
    else if (strcmp(name, "Float64") == 0) base = iron_type_make_primitive(IRON_TYPE_FLOAT64);
    else if (strcmp(name, "Bool")    == 0) base = iron_type_make_primitive(IRON_TYPE_BOOL);
    else if (strcmp(name, "String")  == 0) base = iron_type_make_primitive(IRON_TYPE_STRING);
    else if (strcmp(name, "void")    == 0) base = iron_type_make_primitive(IRON_TYPE_VOID);
    else if (strcmp(name, "Void")    == 0) base = iron_type_make_primitive(IRON_TYPE_VOID);
    else {
        /* User-defined type: look up in global scope */
        Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope, name);
        if (sym) {
            base = sym->type;
            /* Generic enum instantiation: Option[Int], Result[T, E] */
            if (base && base->kind == IRON_TYPE_ENUM &&
                base->enu.decl && base->enu.decl->generic_param_count > 0 &&
                ann->generic_arg_count > 0) {
                Iron_EnumDecl *ed = base->enu.decl;
                if (ann->generic_arg_count != ed->generic_param_count) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "generic enum '%s' expects %d type argument(s) but got %d",
                             ed->name, ed->generic_param_count, ann->generic_arg_count);
                    emit_error(ctx, IRON_ERR_TYPE_MISMATCH, ann_node->span, msg, NULL);
                } else {
                    /* Resolve each generic arg to a concrete type */
                    Iron_Type **type_args = iron_arena_alloc(ctx->arena,
                        sizeof(Iron_Type *) * (size_t)ann->generic_arg_count,
                        _Alignof(Iron_Type *));
                    if (!type_args) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation type_args) */ return NULL; }
                    for (int i = 0; i < ann->generic_arg_count; i++) {
                        type_args[i] = resolve_type_annotation(ctx, ann->generic_args[i]);
                    }

                    /* Build mangled name: "Iron_Option_Int", "Iron_Result_Int_String" */
                    Iron_StrBuf sb = iron_strbuf_create(64);
                    iron_strbuf_appendf(&sb, "Iron_%s", ed->name);
                    for (int i = 0; i < ann->generic_arg_count; i++) {
                        iron_strbuf_appendf(&sb, "_%s",
                            type_mangle_component(type_args[i], ctx->arena));
                    }
                    const char *mangled = iron_arena_strdup(ctx->arena,
                        iron_strbuf_get(&sb), sb.len);
                    if (!mangled) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation mangled) */ return NULL; }
                    iron_strbuf_free(&sb);

                    /* Cycle detection / caching: if this mangled name is already being
                     * resolved (recursive generic enum like Tree[T] whose Branch variant
                     * references Tree[T] again), return the in-progress mono type to
                     * break the cycle. Also serves as a cache for repeat uses. */
                    {
                        ptrdiff_t reg_idx = shgeti(ctx->mono_registry, mangled);
                        if (reg_idx >= 0) {
                            base = ctx->mono_registry[reg_idx].value;
                            goto done_generic_mono;
                        }
                    }

                    /* Build monomorphized Iron_Type */
                    Iron_Type *mono = iron_arena_alloc(ctx->arena, sizeof(Iron_Type),
                        _Alignof(Iron_Type));
                    if (!mono) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation mono) */ return NULL; }
                    memset(mono, 0, sizeof(*mono));
                    mono->kind = IRON_TYPE_ENUM;
                    mono->enu.decl = ed;
                    mono->enu.type_args = type_args;
                    mono->enu.type_arg_count = ann->generic_arg_count;
                    mono->enu.mangled_name = mangled;

                    /* Register mono BEFORE resolving payloads to break recursive cycles
                     * (e.g. Tree[T] → Branch(Tree[T], Tree[T]) → Tree[T] again). */
                    const char *mono_key = iron_arena_strdup(ctx->arena, mangled, strlen(mangled));
                    if (!mono_key) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation mono_key) */ return NULL; }
                    shput(ctx->mono_registry, mono_key, mono);

                    /* Substitute variant_payload_types:
                     * Bind generic param names to their CONCRETE type args in a temporary
                     * scope so that recursive resolve_type_annotation calls for payloads
                     * like Tree[T] resolve directly to Tree[Int] (no post-substitution
                     * needed).  Self-referential payloads (e.g. Branch(Tree[T], Tree[T]))
                     * will find "Iron_Tree_Int" already in mono_registry and return mono,
                     * breaking the cycle without infinite recursion. */
                    Iron_Scope *saved_scope = ctx->global_scope;
                    Iron_Scope *gen_scope = iron_scope_create(ctx->arena,
                        ctx->global_scope, IRON_SCOPE_BLOCK);
                    for (int i = 0; i < ed->generic_param_count; i++) {
                        /* PROT-03 row 12 (AUDIT-01 M-severity): assert kind on
                         * the generic-param node before the Iron_Ident cast. */
                        if (ed->generic_params[i])
                            IRON_NODE_ASSERT_KIND(ed->generic_params[i], IRON_NODE_IDENT);
                        Iron_Ident *param = (Iron_Ident *)ed->generic_params[i];
                        if (param) {
                            Iron_Symbol *gsym = iron_symbol_create(ctx->arena,
                                param->name, IRON_SYM_TYPE, NULL,
                                (Iron_Span){0, 0, 0, 0, 0});
                            /* Bind the CONCRETE type arg (not a GENERIC_PARAM placeholder).
                             * This ensures Tree[T] resolves to Tree[Int] directly. */
                            gsym->type = (i < ann->generic_arg_count) ? type_args[i]
                                         : iron_type_make_generic_param(
                                               ctx->arena, param->name, NULL);
                            iron_scope_define(gen_scope, ctx->arena, gsym);
                        }
                    }
                    ctx->global_scope = gen_scope;

                    Iron_Type ***vpt = iron_arena_alloc(ctx->arena,
                        sizeof(Iron_Type **) * (size_t)ed->variant_count,
                        _Alignof(Iron_Type **));
                    if (!vpt) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation vpt) */ return NULL; }
                    memset(vpt, 0, sizeof(Iron_Type **) * (size_t)ed->variant_count);
                    for (int j = 0; j < ed->variant_count; j++) {
                        Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                        if (ev->payload_count == 0) { vpt[j] = NULL; continue; }
                        Iron_Type **row = iron_arena_alloc(ctx->arena,
                            sizeof(Iron_Type *) * (size_t)ev->payload_count,
                            _Alignof(Iron_Type *));
                        if (!row) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation vpt row) */ return NULL; }
                        for (int k = 0; k < ev->payload_count; k++) {
                            /* T is already bound to the concrete type arg in gen_scope,
                             * so no post-substitution is needed. */
                            row[k] = resolve_type_annotation(
                                ctx, ev->payload_type_anns[k]);
                        }
                        vpt[j] = row;
                    }
                    ctx->global_scope = saved_scope;

                    mono->enu.variant_payload_types = vpt;

                    /* Compute payload_is_boxed for monomorphized type */
                    bool **pib = iron_arena_alloc(ctx->arena,
                        sizeof(bool *) * (size_t)ed->variant_count, _Alignof(bool *));
                    if (!pib) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation pib) */ return NULL; }
                    memset(pib, 0, sizeof(bool *) * (size_t)ed->variant_count);
                    for (int j = 0; j < ed->variant_count; j++) {
                        Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                        if (ev->payload_count == 0) continue;
                        bool *pib_row = iron_arena_alloc(ctx->arena,
                            sizeof(bool) * (size_t)ev->payload_count, _Alignof(bool));
                        if (!pib_row) { /* HARD-09 REPLACE (typecheck.c:resolve_type_annotation pib row) */ return NULL; }
                        memset(pib_row, 0, sizeof(bool) * (size_t)ev->payload_count);
                        for (int k = 0; k < ev->payload_count; k++) {
                            if (vpt[j] && vpt[j][k]) {
                                pib_row[k] = iron_type_equals(vpt[j][k], mono);
                            }
                        }
                        pib[j] = pib_row;
                    }
                    mono->enu.payload_is_boxed = pib;
                    base = mono;
                    done_generic_mono:;
                }
            }
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg), "unknown type '%s'", name);
            emit_error(ctx, IRON_ERR_TYPE_MISMATCH, ann_node->span, msg, NULL);
            base = iron_type_make_primitive(IRON_TYPE_ERROR);
        }
    }

    if (!base) base = iron_type_make_primitive(IRON_TYPE_ERROR);

    /* Wrap in nullable if needed */
    if (ann->is_nullable) {
        Iron_Type *nb = iron_type_make_nullable(ctx->arena, base);
        /* HARD-09 CR-02: NULL-propagation fallback. */
        base = nb ? nb : iron_type_make_primitive(IRON_TYPE_ERROR);
    }

    /* Wrap in array if needed */
    if (ann->is_array) {
        int size = -1;  /* dynamic by default */
        if (ann->array_size && ann->array_size->kind == IRON_NODE_INT_LIT) {
            Iron_IntLit *il = (Iron_IntLit *)ann->array_size;
            if (il->value) size = (int)strtol(il->value, NULL, 10);
        }
        Iron_Type *arr = iron_type_make_array(ctx->arena, base, size, ann->bounded);
        /* HARD-09 CR-02: NULL-propagation fallback. */
        if (!arr) arr = iron_type_make_primitive(IRON_TYPE_ERROR);
        base = arr;
        /* Phase 48: propagate layout annotations; only safe if the wrapped
         * type is IRON_TYPE_ARRAY (not the IRON_TYPE_ERROR fallback). */
        if (base && base->kind == IRON_TYPE_ARRAY) {
            base->array.layout_hint  = ann->layout_hint;
            base->array.is_unordered = ann->is_unordered;
            /* Phase 23 VEC-01: propagate bounded flag from annotation to type */
            base->array.is_bounded   = ann->bounded;
        }
    }

    return base;
}

/* ── Narrowing condition classifier ──────────────────────────────────────── */

/* Check whether `expr` is a binary comparison of `sym_name != null` or
 * `sym_name == null`. Returns: 1 for != null, -1 for == null, 0 otherwise. */
static int classify_null_check(Iron_Node *expr, const char **out_name) {
    if (!expr || expr->kind != IRON_NODE_BINARY) return 0;
    Iron_BinaryExpr *be = (Iron_BinaryExpr *)expr;
    int is_neq = (be->op == IRON_TOK_NOT_EQUALS);
    int is_eq  = (be->op == IRON_TOK_EQUALS);
    if (!is_neq && !is_eq) return 0;

    Iron_Node *ident_side = NULL;
    if (be->right && be->right->kind == IRON_NODE_NULL_LIT) {
        ident_side = be->left;
    } else if (be->left && be->left->kind == IRON_NODE_NULL_LIT) {
        ident_side = be->right;
    }
    if (!ident_side || ident_side->kind != IRON_NODE_IDENT) return 0;
    Iron_Ident *id = (Iron_Ident *)ident_side;
    if (out_name) *out_name = id->name;
    return is_neq ? 1 : -1;
}

/* Check if expr is `e is TypeName`, return type_name or NULL */
static const char *classify_is_check(Iron_Node *expr) {
    if (!expr || expr->kind != IRON_NODE_IS) return NULL;
    Iron_IsExpr *ie = (Iron_IsExpr *)expr;
    return ie->type_name;
}

/* Check if a block always returns (for early-return narrowing) */
static bool block_always_returns(Iron_Block *block) {
    if (!block || block->stmt_count == 0) return false;
    Iron_Node *last = block->stmts[block->stmt_count - 1];
    return last && last->kind == IRON_NODE_RETURN;
}

/* Phase 4 Plan 04-01 (EDIT-07): recursive "always returns" check that also
 * counts if/else with both arms terminating + match with all arms
 * terminating. This is a minimal-but-correct reachability pass for the
 * IRON_ERR_MISSING_RETURN walker below.
 *
 * Conservatively returns false for node kinds the walker doesn't understand —
 * so the walker may miss a genuine always-returns path, which is safe
 * (emits a false positive at worst; user can annotate with an explicit
 * return statement to suppress). Never returns true on ambiguous code. */
static bool stmt_always_returns(Iron_Node *node) {
    if (!node) return false;
    switch ((int)node->kind) {
        case IRON_NODE_RETURN:
            return true;
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            if (b->stmt_count == 0) return false;
            return stmt_always_returns(b->stmts[b->stmt_count - 1]);
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *is = (Iron_IfStmt *)node;
            /* if-else with BOTH arms terminating always returns. */
            if (!is->else_body) return false;
            return stmt_always_returns(is->body) &&
                   stmt_always_returns(is->else_body);
        }
        default:
            return false;
    }
}

/* Phase 4 Plan 04-01 (EDIT-07): missing-return walker.
 *
 * Emits IRON_ERR_MISSING_RETURN (code 236) with a type-appropriate
 * "return 0" / "return 0.0" / "return false" / "return \"\""
 * suggestion when a non-void function body does not demonstrably return
 * on every path. (Phase 5 Plan 05-05: semicolon-free -- Iron grammar
 * rejects trailing `;` on return statements.)
 *
 * The walker is intentionally conservative — it only fires when the
 * function has a non-void declared return type and the body's terminal
 * statement is NOT a return (and not an if-else where both arms return).
 * This mirrors rustc's "not all control paths return a value" check. */
static void check_missing_return(TypeCtx *ctx, Iron_FuncDecl *fd) {
    if (!fd || !fd->resolved_return_type) return;
    Iron_Type *rt = fd->resolved_return_type;
    if (rt->kind == IRON_TYPE_VOID || rt->kind == IRON_TYPE_ERROR) return;

    /* Extern functions have no body to check. */
    if (!fd->body) return;
    /* `.iron-stub` companion files are auto-generated signature-only
     * surfaces emitted by `iron build` for path-deps. Their function
     * bodies are intentionally empty; the implementation lives in the
     * compiled artifact (.a) that gets linked alongside. Skip the
     * missing-return walker for any decl whose source span belongs to a
     * stub file — the @file: lexer directive already tags spans with
     * the stub filename when the consumer concats the stub into its
     * combined source. */
    if (fd->span.filename) {
        size_t fnlen = strlen(fd->span.filename);
        const char suffix[] = ".iron-stub";
        const size_t slen = sizeof(suffix) - 1;
        if (fnlen >= slen &&
            strcmp(fd->span.filename + fnlen - slen, suffix) == 0) {
            return;
        }
    }
    if (stmt_always_returns(fd->body)) return;

    /* Pick a type-appropriate "zero" return snippet. Skip emit for types
     * without an obvious zero (objects, enums, arrays) — Plan 04-04's code
     * action handler treats the absence of a suggestion as "no quickfix".
     *
     * Phase 5 Plan 05-05 (D-07 fmt-clean gate): Iron grammar does not
     * accept trailing semicolons on return statements (parser errors
     * with "expected expression" on `return 0;`). Earlier seeds used
     * "return 0;" which was both invalid Iron source AND broke the
     * missing_return quickfix's post-apply fmt-cleanliness. Emit
     * canonical semicolon-free forms. */
    const char *zero = NULL;
    switch ((int)rt->kind) {
        case IRON_TYPE_INT:
        case IRON_TYPE_INT8:
        case IRON_TYPE_INT16:
        case IRON_TYPE_INT32:
        case IRON_TYPE_INT64:
        case IRON_TYPE_UINT:
        case IRON_TYPE_UINT8:
        case IRON_TYPE_UINT16:
        case IRON_TYPE_UINT32:
        case IRON_TYPE_UINT64:
            zero = "return 0"; break;
        case IRON_TYPE_FLOAT:
        case IRON_TYPE_FLOAT32:
        case IRON_TYPE_FLOAT64:
            zero = "return 0.0"; break;
        case IRON_TYPE_BOOL:
            zero = "return false"; break;
        case IRON_TYPE_STRING:
            zero = "return \"\""; break;
        default:
            /* No obvious zero; emit without a suggestion. */
            break;
    }

    /* Use the body's span so the squiggle highlights the whole function body
     * rather than just the decl header. */
    Iron_Span emit_span = fd->body->span;

    if (zero) {
        emit_error(ctx, IRON_ERR_MISSING_RETURN, emit_span,
                   "function may reach end without returning a value", zero);
    } else {
        /* No well-defined zero snippet; synthesize a type-name-ish suggestion
         * so .suggestion is non-NULL (every P1 emit-site seeds .suggestion).
         *
         * Phase 5 Plan 05-05: semicolon-free forms -- Iron grammar does
         * not accept trailing `;`. */
        const char *fallback = iron_type_to_string(rt, ctx->arena);
        if (!fallback) fallback = "<value>";
        char *sug = (char *)iron_arena_alloc(ctx->arena,
                                              strlen("return ") + strlen(fallback)
                                              + strlen("(...)") + 1, 1);
        if (sug) {
            sprintf(sug, "return %s(...)", fallback);
            emit_error(ctx, IRON_ERR_MISSING_RETURN, emit_span,
                       "function may reach end without returning a value",
                       sug);
        } else {
            emit_error(ctx, IRON_ERR_MISSING_RETURN, emit_span,
                       "function may reach end without returning a value",
                       "return <value>");
        }
    }
}

/* ── Array extension method return type resolution ──────────────────────── */

/* Resolve the return type of a method call on an array by searching for a
 * matching array extension method declaration (func [T].method(...)).
 * Returns the resolved type, or NULL if no matching extension was found
 * (caller should fall back to heuristics). */
static Iron_Type *resolve_array_ext_method(TypeCtx *ctx,
                                           Iron_MethodCallExpr *mc,
                                           Iron_Type *arr_type) {
    if (!ctx->program || !arr_type) return NULL;
    Iron_Type *elem_type = arr_type->array.elem;
    const char *method = mc->method;

    for (int m = 0; m < ctx->program->decl_count; m++) {
        Iron_Node *d = ctx->program->decls[m];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *ext = (Iron_MethodDecl *)d;
        if (!ext->is_array_extension) continue;
        if (strcmp(ext->method_name, method) != 0) continue;

        /* Found matching extension method. Resolve return type. */

        /* Type error: sum() on non-numeric arrays */
        if (strcmp(method, "sum") == 0 && elem_type) {
            if (elem_type->kind != IRON_TYPE_INT && elem_type->kind != IRON_TYPE_INT32 &&
                elem_type->kind != IRON_TYPE_FLOAT) {
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                               IRON_ERR_TYPE_MISMATCH, mc->span,
                               "sum() requires [Int] or [Float] array", NULL);
            }
        }

        if (!ext->return_type) {
            return iron_type_make_primitive(IRON_TYPE_VOID);
        }

        Iron_TypeAnnotation *ret_ann = (ext->return_type->kind == IRON_NODE_TYPE_ANNOTATION)
            ? (Iron_TypeAnnotation *)ext->return_type : NULL;

        if (ret_ann && ret_ann->is_array) {
            /* Return type is [SomeType] */
            const char *inner = ret_ann->name;
            if (inner && ext->elem_type_name && strcmp(inner, ext->elem_type_name) == 0) {
                /* [T] -> same array type as input (filter) */
                return arr_type;
            } else {
                /* [U] -> infer U from lambda return type in first arg (map).
                 * Re-check arg to get resolved type (idempotent after initial check). */
                Iron_Type *inferred_u = NULL;
                if (mc->arg_count > 0) {
                    Iron_Type *arg0_type = check_expr(ctx, mc->args[0]);
                    if (arg0_type && arg0_type->kind == IRON_TYPE_FUNC) {
                        inferred_u = arg0_type->func.return_type;
                    }
                }
                if (inferred_u) {
                    return iron_type_make_array(ctx->arena, inferred_u, -1, false);
                }
                return arr_type;
            }
        } else if (ret_ann && ret_ann->name) {
            /* Scalar return type */
            if (ext->elem_type_name && strcmp(ret_ann->name, ext->elem_type_name) == 0) {
                /* T -> element type (sum) */
                return elem_type ? elem_type : iron_type_make_primitive(IRON_TYPE_INT);
            } else {
                /* U -> infer from init arg (reduce) or lambda return */
                Iron_Type *inferred_u = NULL;
                if (mc->arg_count > 0) {
                    inferred_u = check_expr(ctx, mc->args[0]);
                }
                return inferred_u ? inferred_u : iron_type_make_primitive(IRON_TYPE_VOID);
            }
        }
        return iron_type_make_primitive(IRON_TYPE_VOID);
    }
    return NULL;  /* no matching extension found */
}

/* Phase 56 Plan 02: Human-readable type name for diagnostics.
 * iron_type_to_string returns "<object>" / "<interface>" for object and
 * interface types (no decl name). This helper fetches the decl name directly
 * so error messages contain "Circle" and "Square" (the named types users
 * wrote) rather than "<object>". Falls back to iron_type_to_string for other
 * kinds. */
static const char *type_display_name(const Iron_Type *t, Iron_Arena *arena) {
    if (!t) return "unknown";
    if (t->kind == IRON_TYPE_OBJECT && t->object.decl && t->object.decl->name) {
        return t->object.decl->name;
    }
    if (t->kind == IRON_TYPE_INTERFACE && t->interface.decl &&
        t->interface.decl->name) {
        return t->interface.decl->name;
    }
    const char *s = iron_type_to_string(t, arena);
    return s ? s : "unknown";
}

/* Phase 56 Plan 02: Push arg-type compatibility check.
 * Returns true if `arg_type` can be pushed onto an array whose element type
 * is `elem_type`. Prevents silent miscompilation where a mono-collapsed
 * collection (e.g. `var shapes = [Circle(1)]` narrowed to [Circle]) accepts
 * a heterogeneous push like `shapes.push(Square(2))`: before Plan 01, this
 * was caught indirectly by a C codegen error on the undeclared
 * Iron_List_Iron_Circle_push symbol; after Plan 01 the codegen path succeeds,
 * so the type checker must validate the arg-elem match itself.
 *
 * Rules:
 *   - Permissive on NULL or ERROR types (lets other diagnostics fire first).
 *   - Primitive kinds must match exactly (Int == Int, not Int == Int32).
 *   - Object == Object requires identical decl pointers (Circle == Circle).
 *   - Object arg into Interface elem is allowed iff the object's decl lists
 *     the interface in implements_names.
 *   - Interface == Interface requires identical decl pointers.
 *   - All other combinations (e.g. primitive arg into object elem) reject.
 */
static bool push_type_compatible(const Iron_Type *elem_type,
                                 const Iron_Type *arg_type) {
    if (!elem_type || !arg_type) return true;
    if (elem_type->kind == IRON_TYPE_ERROR || arg_type->kind == IRON_TYPE_ERROR) {
        return true;
    }

    /* Exact structural match (primitive singletons, func types, arrays, etc.) */
    if (iron_type_equals(elem_type, arg_type)) return true;

    /* Object == Object: same decl required. iron_type_equals should cover
     * this, but we double-check in case two Iron_Type values reference the
     * same decl via different allocations. */
    if (elem_type->kind == IRON_TYPE_OBJECT && arg_type->kind == IRON_TYPE_OBJECT) {
        return elem_type->object.decl == arg_type->object.decl;
    }

    /* Interface elem accepting an object arg: the object's decl must list the
     * interface in implements_names. */
    if (elem_type->kind == IRON_TYPE_INTERFACE &&
        arg_type->kind == IRON_TYPE_OBJECT &&
        elem_type->interface.decl && arg_type->object.decl) {
        const char *iface_name = elem_type->interface.decl->name;
        Iron_ObjectDecl *od = arg_type->object.decl;
        if (!iface_name) return false;
        for (int i = 0; i < od->implements_count; i++) {
            if (od->implements_names[i] &&
                strcmp(od->implements_names[i], iface_name) == 0) {
                return true;
            }
        }
        return false;
    }

    /* Interface elem accepting an interface arg: same decl pointer. */
    if (elem_type->kind == IRON_TYPE_INTERFACE &&
        arg_type->kind == IRON_TYPE_INTERFACE) {
        return elem_type->interface.decl == arg_type->interface.decl;
    }

    /* Anything else: reject, surface as diagnostic. */
    return false;
}

/* Heuristic fallback for built-in array methods (push, pop, len, etc.)
 * that don't have explicit extension method declarations yet. */
static Iron_Type *resolve_array_builtin_method(const char *method,
                                               Iron_Type *arr_type) {
    if (strcmp(method, "len") == 0) {
        return iron_type_make_primitive(IRON_TYPE_INT);
    } else if (strcmp(method, "push") == 0 || strcmp(method, "set") == 0 ||
               strcmp(method, "free") == 0 || strcmp(method, "sort") == 0 ||
               strcmp(method, "reverse") == 0 || strcmp(method, "for_each") == 0) {
        return iron_type_make_primitive(IRON_TYPE_VOID);
    } else if (strcmp(method, "get") == 0 || strcmp(method, "pop") == 0 ||
               strcmp(method, "find") == 0) {
        return (arr_type->array.elem != NULL)
                   ? arr_type->array.elem
                   : iron_type_make_primitive(IRON_TYPE_VOID);
    } else if (strcmp(method, "any") == 0 || strcmp(method, "all") == 0) {
        return iron_type_make_primitive(IRON_TYPE_BOOL);
    }
    return arr_type;
}

/* ── Expression type inference ───────────────────────────────────────────── */

static Iron_Type *check_expr(TypeCtx *ctx, Iron_Node *node) {
    if (!node) return iron_type_make_primitive(IRON_TYPE_VOID);
    /* HARD-05: cancel poll at recursive expression walker entry. */
    if (iron_cancel_requested(ctx->cancel_flag)) {
        return iron_type_make_primitive(IRON_TYPE_VOID);
    }

    Iron_Type *result = NULL;

    switch ((int)(node->kind)) {
        case IRON_NODE_INT_LIT: {
            Iron_IntLit *n = (Iron_IntLit *)node;
            result = iron_type_make_primitive(IRON_TYPE_INT);
            n->resolved_type = result;
            break;
        }

        case IRON_NODE_FLOAT_LIT: {
            Iron_FloatLit *n = (Iron_FloatLit *)node;
            result = iron_type_make_primitive(IRON_TYPE_FLOAT);
            n->resolved_type = result;
            break;
        }

        case IRON_NODE_BOOL_LIT: {
            Iron_BoolLit *n = (Iron_BoolLit *)node;
            result = iron_type_make_primitive(IRON_TYPE_BOOL);
            n->resolved_type = result;
            break;
        }

        case IRON_NODE_STRING_LIT: {
            Iron_StringLit *n = (Iron_StringLit *)node;
            result = iron_type_make_primitive(IRON_TYPE_STRING);
            n->resolved_type = result;
            break;
        }

        case IRON_NODE_INTERP_STRING: {
            Iron_InterpString *n = (Iron_InterpString *)node;
            for (int i = 0; i < n->part_count; i++) {
                Iron_Type *part_type = check_expr(ctx, n->parts[i]);
                /* Skip string literals -- they are always stringifiable */
                if (n->parts[i]->kind != IRON_NODE_STRING_LIT && part_type) {
                    if (!is_stringifiable(ctx, part_type)) {
                        char msg[256];
                        const char *ts = iron_type_to_string(part_type, ctx->arena);
                        snprintf(msg, sizeof(msg),
                                 "type '%s' cannot be interpolated into a string "
                                 "(will use address printing)", ts);
                        emit_warning(ctx, IRON_WARN_NOT_STRINGABLE,
                                     n->parts[i]->span, msg,
                                     "add a to_string() method to this type");
                    }
                }
            }
            result = iron_type_make_primitive(IRON_TYPE_STRING);
            n->resolved_type = result;
            break;
        }

        case IRON_NODE_NULL_LIT: {
            Iron_NullLit *n = (Iron_NullLit *)node;
            result = iron_type_make_primitive(IRON_TYPE_NULL);
            n->resolved_type = result;
            break;
        }

        case IRON_NODE_IDENT: {
            Iron_Ident *id = (Iron_Ident *)node;

            /* Phase 84 MUTTIER-03 E0241: pure-tier read of a mutable
             * top-level `var`. Detected by looking up the name in
             * ctx->global_scope directly (not via tc_lookup, which walks
             * the whole chain); a mutable VARIABLE registered there is a
             * top-level `var counter: Int = 0`. Top-level `val` is not
             * mutable (resolver sets is_mutable=false for IRON_NODE_VAL_DECL)
             * so val globals pass cleanly. The check runs before narrowing /
             * type resolution so it fires on every read regardless of
             * where the type came from. */
            if (ctx->in_pure_method && id->name && ctx->global_scope) {
                Iron_Symbol *global_sym =
                    iron_scope_lookup_local(ctx->global_scope, id->name);
                if (global_sym &&
                    global_sym->sym_kind == IRON_SYM_VARIABLE &&
                    global_sym->is_mutable) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "cannot read mutable global '%s' in pure method",
                             id->name);
                    emit_error(ctx, IRON_ERR_PURE_MUTABLE_GLOBAL,
                               id->span, msg, NULL);
                }
            }

            /* 1. Check narrowing map first */
            Iron_Type *narrowed = narrowing_get(ctx, id->name);
            if (narrowed) {
                result = narrowed;
                id->resolved_type = result;
                break;
            }

            /* 2. Look up in type-checker scope chain (has params + locals) */
            Iron_Symbol *tc_sym = tc_lookup(ctx, id->name);
            if (tc_sym && tc_sym->type) {
                result = tc_sym->type;
                id->resolved_type = result;
                break;
            }

            /* 3. Fall back to resolver's resolved_sym */
            if (id->resolved_sym && id->resolved_sym->type) {
                result = id->resolved_sym->type;
                id->resolved_type = result;
                break;
            }

            /* Unresolved or untyped */
            result = iron_type_make_primitive(IRON_TYPE_ERROR);
            id->resolved_type = result;
            break;
        }

        case IRON_NODE_BINARY: {
            Iron_BinaryExpr *be = (Iron_BinaryExpr *)node;
            Iron_Type *lt = check_expr(ctx, be->left);
            Iron_Type *rt = check_expr(ctx, be->right);

            int op = be->op;
            bool is_comparison = (op == IRON_TOK_EQUALS || op == IRON_TOK_NOT_EQUALS ||
                                   op == IRON_TOK_LESS   || op == IRON_TOK_GREATER   ||
                                   op == IRON_TOK_LESS_EQ || op == IRON_TOK_GREATER_EQ);
            bool is_logic = (op == IRON_TOK_AND || op == IRON_TOK_OR);
            bool is_arithmetic = (op == IRON_TOK_PLUS || op == IRON_TOK_MINUS ||
                                   op == IRON_TOK_STAR || op == IRON_TOK_SLASH ||
                                   op == IRON_TOK_PERCENT);
            bool is_bitwise = (op == IRON_TOK_SHL  || op == IRON_TOK_SHR  ||
                               op == IRON_TOK_AMP  || op == IRON_TOK_PIPE ||
                               op == IRON_TOK_CARET);

            if (lt && rt && lt->kind != IRON_TYPE_ERROR && rt->kind != IRON_TYPE_ERROR) {
                /* Phase 20 PTR-11: pointer arithmetic in checked regime is
                 * forbidden. Fires for + - * / %% when EITHER operand is
                 * IRON_TYPE_PTR. The diagnostic hint mentions Phase 25's
                 * `*unchecked T` + `Ptr.offset` escape hatch as the
                 * forward-migration path for performance-critical pointer
                 * arithmetic code. */
                if (is_arithmetic &&
                    (lt->kind == IRON_TYPE_PTR || rt->kind == IRON_TYPE_PTR)) {
                    emit_error(ctx, IRON_ERR_PTR_NO_ARITH, be->span,
                               "no pointer arithmetic in checked regime; "
                               "use Ptr.offset on *unchecked T",
                               "checked-pointer arithmetic is deferred to "
                               "Phase 25's *unchecked T regime via Ptr.offset");
                    result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    be->resolved_type = result;
                    break;
                }
                bool lt_is_int   = (lt->kind == IRON_TYPE_INT);
                bool lt_is_float = (lt->kind == IRON_TYPE_FLOAT ||
                                    lt->kind == IRON_TYPE_FLOAT32 ||
                                    lt->kind == IRON_TYPE_FLOAT64);
                bool rt_is_int   = (rt->kind == IRON_TYPE_INT);
                bool rt_is_float = (rt->kind == IRON_TYPE_FLOAT ||
                                    rt->kind == IRON_TYPE_FLOAT32 ||
                                    rt->kind == IRON_TYPE_FLOAT64);

                /* Phase 59 01d: tuple == / != — arity + element-type check.
                 * Fails the arithmetic path below (lt != rt) but is legal
                 * for equality. Result is always Bool. */
                if (is_comparison &&
                    (op == IRON_TOK_EQUALS || op == IRON_TOK_NOT_EQUALS) &&
                    lt->kind == IRON_TYPE_TUPLE && rt->kind == IRON_TYPE_TUPLE) {
                    if (lt->tuple.elem_count != rt->tuple.elem_count) {
                        emit_error(ctx, IRON_ERR_TYPE_MISMATCH, be->span,
                                   "tuple equality requires matching arity", NULL);
                    } else {
                        for (int i = 0; i < lt->tuple.elem_count; i++) {
                            if (!iron_type_equals(lt->tuple.elem_types[i],
                                                   rt->tuple.elem_types[i])) {
                                emit_error(ctx, IRON_ERR_TYPE_MISMATCH, be->span,
                                           "tuple equality requires matching element types", NULL);
                                break;
                            }
                        }
                    }
                    result = iron_type_make_primitive(IRON_TYPE_BOOL);
                    be->resolved_type = result;
                    break;
                }

                if ((lt_is_int && rt_is_float) || (lt_is_float && rt_is_int)) {
                    emit_error(ctx, IRON_ERR_NUMERIC_CONVERSION, be->span,
                               "cannot mix Int and Float in expression without explicit cast",
                               "Use explicit cast: Float(x)");
                    result = iron_type_make_primitive(IRON_TYPE_ERROR);
                } else if (is_comparison) {
                    /* Comparison: operands should be compatible */
                    if (!iron_type_equals(lt, rt) &&
                        !(rt->kind == IRON_TYPE_NULL) &&
                        !((lt->kind == IRON_TYPE_INT32 && rt->kind == IRON_TYPE_INT) ||
                          (lt->kind == IRON_TYPE_INT && rt->kind == IRON_TYPE_INT32))) {
                        /* Allow comparison with null literal and Int32<->Int widening */
                    }
                    result = iron_type_make_primitive(IRON_TYPE_BOOL);
                } else if (is_logic) {
                    if (lt->kind != IRON_TYPE_BOOL) {
                        emit_error(ctx, IRON_ERR_TYPE_MISMATCH, be->span,
                                   "logical operator requires Bool operands", NULL);
                    }
                    result = iron_type_make_primitive(IRON_TYPE_BOOL);
                } else if (is_arithmetic) {
                    /* Phase 96 STR-01/02: special-case `+` for strings.
                     * Three cases, all triggered by op == IRON_TOK_PLUS:
                     *
                     *   1. String + non-String (or non-String + String):
                     *      fire the narrowed E0202 directly (instead of
                     *      falling through to TYPE_MISMATCH) so the
                     *      diagnostic message tells the user `+` accepts
                     *      numeric operands or two `String` values. The
                     *      `(lt==STRING) != (rt==STRING)` guard catches
                     *      both `"foo" + 42` and `42 + "foo"`.
                     *
                     *   2. String + String: set the AST bit so hir_lower
                     *      rewrites this binop into a runtime call to
                     *      iron_string_concat. No diagnostic — typecheck
                     *      succeeds and produces a String result.
                     *
                     *   3. Anything else: keep the v3.1 behavior — type-
                     *      mismatch on lt != rt; the GENERIC E0202 message
                     *      "arithmetic operator requires numeric operands"
                     *      survives for non-numeric same-type operands
                     *      (e.g. `"a" - "b"`, `true * false`) so that `-`,
                     *      `*`, `/`, `%` continue to reject those without
                     *      advertising a String overload that does not
                     *      exist for those operators. */
                    if (op == IRON_TOK_PLUS &&
                        (lt->kind == IRON_TYPE_STRING) != (rt->kind == IRON_TYPE_STRING)) {
                        emit_error(ctx, IRON_ERR_TYPE_MISMATCH, be->span,
                                   "operator `+` requires numeric operands or two `String` values",
                                   NULL);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    } else if (op == IRON_TOK_PLUS &&
                               lt->kind == IRON_TYPE_STRING &&
                               rt->kind == IRON_TYPE_STRING) {
                        be->is_string_concat = true;
                        result = lt;  /* String */
                    } else if (!iron_type_equals(lt, rt)) {
                        emit_type_mismatch(ctx, be->span, lt, rt);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    } else if (!iron_type_is_numeric(lt)) {
                        emit_error(ctx, IRON_ERR_TYPE_MISMATCH, be->span,
                                   "arithmetic operator requires numeric operands", NULL);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    } else {
                        result = lt;
                    }
                } else if (is_bitwise) {
                    if (lt->kind != IRON_TYPE_INT) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "bitwise operator requires Int operands, got '%s'",
                                 iron_type_to_string(lt, ctx->arena));
                        emit_error(ctx, IRON_ERR_BITWISE_NON_INT, be->span, msg, NULL);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    } else if (rt->kind != IRON_TYPE_INT) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "bitwise operator requires Int operands, got '%s'",
                                 iron_type_to_string(rt, ctx->arena));
                        emit_error(ctx, IRON_ERR_BITWISE_NON_INT, be->span, msg, NULL);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    } else {
                        result = lt;  /* Int */
                    }
                } else {
                    result = lt;
                }
            } else {
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
            }

            be->resolved_type = result;
            break;
        }

        case IRON_NODE_UNARY: {
            Iron_UnaryExpr *ue = (Iron_UnaryExpr *)node;
            /* Phase 20 PTR-04 / PTR-07 (Plan 20-02a): `&` resolves to *T at
             * the analyzer level. The operand must be an lvalue (named
             * binding, field, element); any rvalue (literal, function-call
             * result, binop) emits E0270. The result is `*var T` when the
             * operand source is mutable (var binding or var-rooted field)
             * and `*T` otherwise. HIR/LIR lowering (Plan 20-02b) reads the
             * resolved Iron_Type to emit Iron_FatPtr and tag the gen source.
             *
             * Order: handle AMP BEFORE the generic check_expr(operand) so
             * we can short-circuit on rvalue operands and avoid surfacing
             * unrelated diagnostics from re-checking. */
            if (ue->op == (Iron_OpKind)IRON_TOK_AMP) {
                if (!is_lvalue_expression(ue->operand)) {
                    /* Still check the operand to surface its own
                     * diagnostics (e.g. undefined call inside &g()). */
                    check_expr(ctx, ue->operand);
                    emit_error(ctx, IRON_ERR_PTR_AMP_ON_RVALUE, ue->span,
                               "cannot take address of rvalue (literal or "
                               "temporary)",
                               "auto-address requires a named binding, "
                               "field, or element");
                    result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    ue->resolved_type = result;
                    break;
                }
                Iron_Type *operand_t = check_expr(ctx, ue->operand);
                if (!operand_t || operand_t->kind == IRON_TYPE_ERROR) {
                    result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    ue->resolved_type = result;
                    break;
                }
                bool is_var_src = arg_source_is_mutable(ctx, ue->operand);
                Iron_Type *ptr_t = iron_type_make_ptr(ctx->arena,
                                                       operand_t,
                                                       is_var_src,
                                                       false); /* &expr always yields checked *T (UNCK-04) */
                result = ptr_t ? ptr_t
                               : iron_type_make_primitive(IRON_TYPE_ERROR);
                ue->resolved_type = result;
                break;
            }
            Iron_Type *ot = check_expr(ctx, ue->operand);
            if (ue->op == IRON_TOK_NOT) {
                if (ot && ot->kind != IRON_TYPE_BOOL && ot->kind != IRON_TYPE_ERROR) {
                    emit_error(ctx, IRON_ERR_TYPE_MISMATCH, ue->span,
                               "'not' operator requires Bool operand", NULL);
                }
                result = iron_type_make_primitive(IRON_TYPE_BOOL);
            } else if (ue->op == IRON_TOK_MINUS) {
                if (ot && !iron_type_is_numeric(ot) && ot->kind != IRON_TYPE_ERROR) {
                    emit_error(ctx, IRON_ERR_TYPE_MISMATCH, ue->span,
                               "unary '-' requires numeric operand", NULL);
                }
                result = ot ? ot : iron_type_make_primitive(IRON_TYPE_ERROR);
            } else if (ue->op == IRON_TOK_TILDE) {
                if (ot && ot->kind != IRON_TYPE_INT && ot->kind != IRON_TYPE_ERROR) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "bitwise operator '~' requires Int operand, got '%s'",
                             iron_type_to_string(ot, ctx->arena));
                    emit_error(ctx, IRON_ERR_BITWISE_NON_INT, ue->span, msg, NULL);
                    result = iron_type_make_primitive(IRON_TYPE_ERROR);
                } else {
                    result = ot ? ot : iron_type_make_primitive(IRON_TYPE_ERROR);
                }
            } else {
                result = ot ? ot : iron_type_make_primitive(IRON_TYPE_ERROR);
            }
            ue->resolved_type = result;
            break;
        }

        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;

            /* Phase 20 OQ-D (Plan 20-02a): `Ptr.cast[T](p)` compiler
             * builtin. Parses as CALL(callee=INDEX(object=FIELD_ACCESS(
             * Ptr.cast), index=Ident(T)), args=[p]). Compile-time
             * pointee-size check: sizeof(T) must equal sizeof(*S) where
             * S is the pointee of arg p (an IRON_TYPE_PTR).
             *   - Mismatch → IRON_ERR_PTR_CAST_SIZE_MISMATCH (E0269)
             *   - Success → return *T preserving is_var from the source
             *
             * Per CONTEXT.md OQ-D lock: Ptr.cast is a compiler builtin in
             * typecheck.c, NOT a stdlib function. Phase 25 ships the rest
             * of the Ptr namespace as stdlib functions; the dedicated
             * builtin path here owns the size check. */
            if (ce->callee && ce->callee->kind == IRON_NODE_INDEX) {
                Iron_IndexExpr *idx_callee = (Iron_IndexExpr *)ce->callee;
                if (idx_callee->object &&
                    idx_callee->object->kind == IRON_NODE_FIELD_ACCESS) {
                    Iron_FieldAccess *fa_inner =
                        (Iron_FieldAccess *)idx_callee->object;
                    bool is_ptr_cast =
                        fa_inner->object &&
                        fa_inner->object->kind == IRON_NODE_IDENT &&
                        ((Iron_Ident *)fa_inner->object)->name &&
                        strcmp(((Iron_Ident *)fa_inner->object)->name,
                               "Ptr") == 0 &&
                        fa_inner->field &&
                        strcmp(fa_inner->field, "cast") == 0;
                    if (is_ptr_cast) {
                        /* Resolve target type T from the index expression.
                         * The parser produced an Iron_Ident or other
                         * type-name expression; resolve it via scope
                         * lookup against IRON_SYM_TYPE. */
                        Iron_Type *target_t = NULL;
                        if (idx_callee->index &&
                            idx_callee->index->kind == IRON_NODE_IDENT) {
                            Iron_Ident *tid = (Iron_Ident *)idx_callee->index;
                            Iron_Symbol *tsym = tid->name
                                ? iron_scope_lookup(ctx->global_scope, tid->name)
                                : NULL;
                            if (tsym && tsym->sym_kind == IRON_SYM_TYPE) {
                                target_t = tsym->type;
                            } else {
                                target_t = iron_type_make_primitive(IRON_TYPE_INT);
                                if (tsym == NULL) {
                                    /* Try built-in primitives by name. */
                                    if (strcmp(tid->name, "Int")  == 0) target_t = iron_type_make_primitive(IRON_TYPE_INT);
                                    else if (strcmp(tid->name, "Bool")  == 0) target_t = iron_type_make_primitive(IRON_TYPE_BOOL);
                                    else if (strcmp(tid->name, "Float") == 0) target_t = iron_type_make_primitive(IRON_TYPE_FLOAT);
                                    else if (strcmp(tid->name, "String")== 0) target_t = iron_type_make_primitive(IRON_TYPE_STRING);
                                }
                            }
                        }
                        /* Validate exactly one positional arg of type *S. */
                        if (ce->arg_count != 1) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "Ptr.cast[T] expects 1 argument, got %d",
                                     ce->arg_count);
                            emit_error(ctx, IRON_ERR_ARG_COUNT,
                                       ce->span, msg, NULL);
                            for (int i = 0; i < ce->arg_count; i++)
                                check_expr(ctx, ce->args[i]);
                            result = iron_type_make_primitive(IRON_TYPE_ERROR);
                            ce->resolved_type = result;
                            break;
                        }
                        Iron_Type *src_arg_t = check_expr(ctx, ce->args[0]);
                        if (!src_arg_t || src_arg_t->kind != IRON_TYPE_PTR ||
                            !src_arg_t->ptr.pointee) {
                            emit_error(ctx, IRON_ERR_TYPE_MISMATCH,
                                       ce->args[0]->span,
                                       "Ptr.cast[T] expects a pointer "
                                       "argument",
                                       NULL);
                            result = iron_type_make_primitive(IRON_TYPE_ERROR);
                            ce->resolved_type = result;
                            break;
                        }
                        if (target_t) {
                            int sz_target = iron_type_pointee_size(target_t);
                            int sz_source = iron_type_pointee_size(
                                src_arg_t->ptr.pointee);
                            if (sz_target > 0 && sz_source > 0 &&
                                sz_target != sz_source) {
                                char msg[320];
                                snprintf(msg, sizeof(msg),
                                         "Ptr.cast pointee size mismatch: "
                                         "cannot cast '*%s' (size %d) to "
                                         "'*%s' (size %d); cast requires "
                                         "sizeof(target) == sizeof(source)",
                                         iron_type_to_string(
                                             src_arg_t->ptr.pointee, ctx->arena),
                                         sz_source,
                                         iron_type_to_string(target_t, ctx->arena),
                                         sz_target);
                                emit_error(ctx,
                                           IRON_ERR_PTR_CAST_SIZE_MISMATCH,
                                           ce->span, msg,
                                           "use *unchecked T (Phase 25) "
                                           "for arbitrary pointer casts");
                                result = iron_type_make_primitive(IRON_TYPE_ERROR);
                                ce->resolved_type = result;
                                break;
                            }
                            /* Success: return *T preserving is_var.
                             * Ptr.cast stays in checked regime (UNCK-04). */
                            Iron_Type *out_t = iron_type_make_ptr(
                                ctx->arena, target_t,
                                src_arg_t->ptr.is_var,
                                false); /* Ptr.cast preserves checked regime */
                            result = out_t
                                ? out_t
                                : iron_type_make_primitive(IRON_TYPE_ERROR);
                            ce->resolved_type = result;
                            break;
                        }
                        /* target_t couldn't be resolved — fall through to
                         * the standard CALL path which will emit a more
                         * generic diagnostic for the unknown callee. */
                    }
                }
            }

            /* Phase 87-02 SELF-03: Self(args) inside a method body dispatches
             * to the enclosing type's anonymous init. Rewrite the callee ident
             * from "Self" to the concrete type name so the existing anonymous-
             * init dispatch path handles it without any new codegen path. */
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *self_check_id = (Iron_Ident *)ce->callee;
                if (self_check_id->name &&
                    strcmp(self_check_id->name, "Self") == 0) {
                    if (!ctx->enclosing_type_name) {
                        /* E0259: Self used outside method context. */
                        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                       IRON_ERR_SELF_OUTSIDE_CONTEXT,
                                       ce->span,
                                       "'Self' is only valid in method or interface "
                                       "signature return types, not in free function "
                                       "or top-level contexts",
                                       NULL);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                        ce->resolved_type = result;
                        break;
                    }
                    /* Rewrite "Self" ident to the concrete enclosing type name
                     * so the anonymous-init dispatch below handles it normally. */
                    self_check_id->name = ctx->enclosing_type_name;
                }
            }

            /* Disambiguation: if callee is an Ident that resolves to a type,
             * treat this CallExpr as object construction (per plan decision). */
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *callee_id = (Iron_Ident *)ce->callee;
                Iron_Symbol *callee_sym = iron_scope_lookup(ctx->global_scope, callee_id->name);
                if (callee_sym && callee_sym->sym_kind == IRON_SYM_TYPE) {
                    /* Primitive type cast: Float(x), Int(x), Bool(x), etc.
                     * When the target type is a numeric/bool primitive and
                     * exactly one argument is provided, treat as a cast. */
                    Iron_Type *target_t = callee_sym->type;
                    if (target_t && ce->arg_count == 1) {
                        bool is_numeric_or_bool = false;
                        switch ((int)(target_t->kind)) {
                            case IRON_TYPE_INT:
                            case IRON_TYPE_INT8:
                            case IRON_TYPE_INT16:
                            case IRON_TYPE_INT32:
                            case IRON_TYPE_INT64:
                            case IRON_TYPE_UINT:
                            case IRON_TYPE_UINT8:
                            case IRON_TYPE_UINT16:
                            case IRON_TYPE_UINT32:
                            case IRON_TYPE_UINT64:
                            case IRON_TYPE_FLOAT:
                            case IRON_TYPE_FLOAT32:
                            case IRON_TYPE_FLOAT64:
                            case IRON_TYPE_BOOL:
                                is_numeric_or_bool = true;
                                break;
                            /* -Wswitch-enum opt-out: cast-target check accepts
                             * only numeric + bool targets; every other kind
                             * falls through leaving is_numeric_or_bool false
                             * so non-primitive "casts" stay as ordinary calls. */
                            default:
                                break;
                        }
                        if (is_numeric_or_bool) {
                            /* Type-check the argument */
                            Iron_Type *src_type = check_expr(ctx, ce->args[0]);

                            /* Cast source validation: source must be numeric or bool */
                            if (src_type && src_type->kind != IRON_TYPE_ERROR) {
                                bool src_ok = iron_type_is_numeric(src_type) ||
                                              src_type->kind == IRON_TYPE_BOOL;
                                if (!src_ok) {
                                    char msg[256];
                                    const char *src_s = iron_type_to_string(src_type, ctx->arena);
                                    const char *tgt_s = iron_type_to_string(target_t, ctx->arena);
                                    snprintf(msg, sizeof(msg),
                                             "cannot cast '%s' to '%s': source must be numeric or Bool",
                                             src_s, tgt_s);
                                    emit_error(ctx, IRON_ERR_INVALID_CAST, ce->span, msg, NULL);
                                }
                                /* Int->Bool is disallowed (must use explicit comparison) */
                                else if (iron_type_is_integer(src_type) &&
                                         target_t->kind == IRON_TYPE_BOOL) {
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "cannot cast integer to Bool");
                                    emit_error(ctx, IRON_ERR_INVALID_CAST, ce->span, msg,
                                               "use 'x != 0' instead");
                                }
                                /* Narrowing check: wider integer -> narrower integer */
                                else if (iron_type_is_integer(src_type) &&
                                         iron_type_is_integer(target_t) &&
                                         type_bit_width(src_type) > type_bit_width(target_t)) {
                                    /* Check if source is a constant that fits */
                                    if (ce->args[0]->kind == IRON_NODE_INT_LIT) {
                                        Iron_IntLit *lit = (Iron_IntLit *)ce->args[0];
                                        errno = 0;
                                        int64_t val = strtoll(lit->value, NULL, 10);
                                        if (errno == ERANGE || !value_fits_type(val, target_t)) {
                                            char msg[256];
                                            snprintf(msg, sizeof(msg),
                                                     "%s does not fit in %s",
                                                     lit->value,
                                                     iron_type_to_string(target_t, ctx->arena));
                                            emit_error(ctx, IRON_ERR_CAST_OVERFLOW, ce->span,
                                                       msg, NULL);
                                        }
                                        /* else: constant fits, no warning */
                                    } else {
                                        char msg[256];
                                        const char *src_s = iron_type_to_string(src_type, ctx->arena);
                                        const char *tgt_s = iron_type_to_string(target_t, ctx->arena);
                                        snprintf(msg, sizeof(msg),
                                                 "narrowing cast from '%s' to '%s' may lose data",
                                                 src_s, tgt_s);
                                        emit_warning(ctx, IRON_WARN_NARROWING_CAST, ce->span,
                                                     msg, "verify value is in range");
                                    }
                                }
                                /* Phase 4 Plan 04-01 (EDIT-07): redundant-cast
                                 * check. `Float(x)` where x is already Float
                                 * (or any primitive cast where src kind ==
                                 * target kind) is a no-op. Emit a warning
                                 * whose .suggestion is the bare inner
                                 * expression so the code-action dispatcher
                                 * can propose "remove cast". */
                                else if (src_type->kind == target_t->kind) {
                                    const char *src_s = iron_type_to_string(src_type, ctx->arena);
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "redundant cast — expression already has type '%s'",
                                             src_s);
                                    /* Best-effort inner-expression text. For
                                     * literal Iron_IntLit / Iron_FloatLit we
                                     * have the source-string directly; fall
                                     * back to the printed type-string for
                                     * non-literal expressions. Either way
                                     * .suggestion is non-NULL. */
                                    const char *inner = NULL;
                                    Iron_Node *a = ce->args[0];
                                    if (a && a->kind == IRON_NODE_INT_LIT) {
                                        Iron_IntLit *il = (Iron_IntLit *)a;
                                        if (il->value) inner = iron_arena_strdup(
                                            ctx->arena, il->value, strlen(il->value));
                                    } else if (a && a->kind == IRON_NODE_FLOAT_LIT) {
                                        Iron_FloatLit *fl = (Iron_FloatLit *)a;
                                        if (fl->value) inner = iron_arena_strdup(
                                            ctx->arena, fl->value, strlen(fl->value));
                                    } else if (a && a->kind == IRON_NODE_IDENT) {
                                        Iron_Ident *id = (Iron_Ident *)a;
                                        if (id->name) inner = iron_arena_strdup(
                                            ctx->arena, id->name, strlen(id->name));
                                    }
                                    if (!inner) inner = iron_arena_strdup(
                                        ctx->arena, "<expr>", strlen("<expr>"));
                                    emit_warning(ctx, IRON_WARN_REDUNDANT_CAST,
                                                 ce->span, msg, inner);
                                }
                            }

                            /* Mark as primitive cast for the lowerer */
                            ce->is_primitive_cast = true;
                            result = target_t;
                            ce->resolved_type = result;
                            callee_id->resolved_type = result;
                            break;
                        }
                    }
                    /* Phase 85 INIT-14 E0251: constructing an instance of the
                     * enclosing type inside an init body is delegation; emit
                     * and fall through so arg-type errors still surface. */
                    if (ctx->in_init_method && ctx->current_method_type &&
                        callee_id->name &&
                        strcmp(callee_id->name, ctx->current_method_type) == 0) {
                        emit_error(ctx, IRON_ERR_INIT_DELEGATION, ce->span,
                                   "init cannot delegate to another init of "
                                   "the enclosing type",
                                   NULL);
                    }

                    /* Treat as construction: validate args against fields.
                     *
                     * PROT-04 rewrite (rank 5, AUDIT-01): SYM_TYPE can point to
                     * Iron_InterfaceDecl, Iron_EnumDecl, or NULL (builtin primitive
                     * types). The previous code cast decl_node to Iron_ObjectDecl
                     * unconditionally and silently misread interface/enum memory
                     * (or NULL-deref'd for builtins). Guard on decl_node->kind
                     * before the concrete cast and emit a diagnostic for the
                     * non-object case instead of proceeding with a bogus cast. */
                    if (!callee_sym->decl_node ||
                        callee_sym->decl_node->kind != IRON_NODE_OBJECT_DECL) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "type '%s' is not constructible with call syntax",
                                 callee_id->name);
                        emit_error(ctx, IRON_ERR_NOT_CALLABLE, ce->span, msg, NULL);
                        for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                        ce->resolved_type = result;
                        callee_id->resolved_type = result;
                        break;
                    }
                    /* HARD-10 REPLACE (audit row typecheck.c:1435):
                     * callee_sym->decl_node can be IRON_NODE_ERROR after parse
                     * recovery — early-return with a diagnostic instead of aborting. */
                    if (!callee_sym->decl_node ||
                        callee_sym->decl_node->kind != IRON_NODE_OBJECT_DECL) {
                        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_NOTE,
                                       IRON_ERR_UNDEFINED_VAR, ce->span,
                                       "skipping partially-parsed object construction",
                                       NULL);
                        for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                        ce->resolved_type = result;
                        callee_id->resolved_type = result;
                        break;
                    }
                    Iron_ObjectDecl *od = (Iron_ObjectDecl *)callee_sym->decl_node;
                    int field_count = od->field_count;

                    /* Phase 85 INIT-08: dispatch Type(args) to the explicit
                     * anonymous init when one exists (synth or user-written).
                     * Param shape on an init MethodDecl is [self, ...declared
                     * params] so the explicit-param count is param_count - 1.
                     * If no anonymous init is declared, fall through to the
                     * v2.2 field-shape positional check (pure-superset
                     * preservation through Phase 87). */
                    Iron_MethodDecl *anon_init = iron_find_init_by_name(
                        ctx->program, callee_id->name, NULL);
                    if (anon_init) {
                        int init_param_count = anon_init->param_count > 0
                            ? anon_init->param_count - 1 : 0;
                        if (ce->arg_count != init_param_count) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "init '%s' expects %d argument(s), got %d",
                                     callee_id->name, init_param_count,
                                     ce->arg_count);
                            emit_error(ctx, IRON_ERR_ARG_COUNT, ce->span, msg, NULL);
                            for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                        } else {
                            for (int i = 0; i < ce->arg_count; i++) {
                                Iron_Type *arg_t = check_expr(ctx, ce->args[i]);
                                Iron_Param *pp =
                                    (Iron_Param *)anon_init->params[i + 1];
                                Iron_Type *param_t = pp
                                    ? resolve_type_annotation(ctx, pp->type_ann)
                                    : NULL;
                                if (arg_t && param_t &&
                                    arg_t->kind   != IRON_TYPE_ERROR &&
                                    param_t->kind != IRON_TYPE_ERROR &&
                                    !types_assignable(param_t, arg_t) &&
                                    !is_int_literal_narrowing(param_t, arg_t, ce->args[i])) {
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "init param '%s' expects '%s', got '%s'",
                                             pp && pp->name ? pp->name : "?",
                                             iron_type_to_string(param_t, ctx->arena),
                                             iron_type_to_string(arg_t, ctx->arena));
                                    emit_error(ctx, IRON_ERR_ARG_TYPE,
                                               ce->args[i]->span, msg, NULL);
                                }
                                if (is_int_literal_narrowing(param_t, arg_t, ce->args[i])) {
                                    ((Iron_IntLit *)ce->args[i])->resolved_type = param_t;
                                }
                            }
                        }
                        /* Check generic constraints still apply — they are
                         * tied to the ObjectDecl, not the init. */
                        if (od->generic_param_count > 0 && od->generic_params) {
                            Iron_Type *concrete[16];
                            int gc = od->generic_param_count < 16 ? od->generic_param_count : 16;
                            for (int gi = 0; gi < gc; gi++) concrete[gi] = NULL;
                            check_generic_constraints(ctx, od->generic_params,
                                                      od->generic_param_count,
                                                      concrete, gc, ce->span);
                        }
                        result = callee_sym->type;
                        ce->resolved_type = result;
                        callee_id->resolved_type = result;
                        break;
                    }

                    if (ce->arg_count != field_count) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "object '%s' has %d field(s), but %d argument(s) given",
                                 callee_id->name, field_count, ce->arg_count);
                        emit_error(ctx, IRON_ERR_ARG_COUNT, ce->span, msg, NULL);
                        for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                    } else {
                        for (int i = 0; i < ce->arg_count; i++) {
                            Iron_Type *arg_t = check_expr(ctx, ce->args[i]);
                            Iron_Field *fld = (Iron_Field *)od->fields[i];
                            Iron_Type *fld_t = resolve_type_annotation(ctx, fld->type_ann);
                            if (arg_t && fld_t &&
                                arg_t->kind  != IRON_TYPE_ERROR &&
                                fld_t->kind  != IRON_TYPE_ERROR &&
                                !types_assignable(fld_t, arg_t) &&
                                !is_int_literal_narrowing(fld_t, arg_t, ce->args[i])) {
                                char msg[256];
                                snprintf(msg, sizeof(msg),
                                         "field '%s' expects '%s', got '%s'",
                                         fld->name,
                                         iron_type_to_string(fld_t, ctx->arena),
                                         iron_type_to_string(arg_t, ctx->arena));
                                emit_error(ctx, IRON_ERR_ARG_TYPE, ce->args[i]->span,
                                           msg, NULL);
                            }
                            /* Narrow literal args to match field type */
                            if (is_int_literal_narrowing(fld_t, arg_t, ce->args[i])) {
                                ((Iron_IntLit *)ce->args[i])->resolved_type = fld_t;
                            }
                        }
                    }
                    /* Check generic constraints on call-as-construction */
                    if (od->generic_param_count > 0 && od->generic_params) {
                        Iron_Type *concrete[16];
                        int gc = od->generic_param_count < 16 ? od->generic_param_count : 16;
                        for (int gi = 0; gi < gc; gi++) {
                            concrete[gi] = NULL;
                            /* PROT-03 row 13 (AUDIT-01 M-severity): assert kind
                             * on od->generic_params[gi] before the Iron_Ident cast. */
                            if (od->generic_params[gi])
                                IRON_NODE_ASSERT_KIND(od->generic_params[gi], IRON_NODE_IDENT);
                            Iron_Ident *gp = (Iron_Ident *)od->generic_params[gi];
                            if (!gp) continue;
                            for (int fi = 0; fi < od->field_count && fi < ce->arg_count; fi++) {
                                Iron_Field *fld = (Iron_Field *)od->fields[fi];
                                if (fld && fld->type_ann && fld->type_ann->kind == IRON_NODE_TYPE_ANNOTATION) {
                                    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)fld->type_ann;
                                    if (strcmp(ta->name, gp->name) == 0) {
                                        concrete[gi] = check_expr(ctx, ce->args[fi]);
                                        break;
                                    }
                                }
                            }
                        }
                        check_generic_constraints(ctx, od->generic_params, od->generic_param_count,
                                                  concrete, gc, ce->span);
                    }
                    result = callee_sym->type;
                    ce->resolved_type = result;
                    callee_id->resolved_type = result;
                    break;
                }
            }

            Iron_Type *callee_type = check_expr(ctx, ce->callee);

            if (!callee_type || callee_type->kind == IRON_TYPE_ERROR) {
                for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
                ce->resolved_type = result;
                break;
            }

            if (callee_type->kind != IRON_TYPE_FUNC) {
                emit_error(ctx, IRON_ERR_NOT_CALLABLE, ce->span,
                           "expression is not callable", NULL);
                for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
                ce->resolved_type = result;
                break;
            }

            /* Phase 84 MUTTIER-03 E0240: pure method calling an I/O builtin.
             * The hardcoded allowlist covers the v3.0 I/O surface: println,
             * print, readline. Future stdlib I/O bindings (file, network)
             * would be added here; a user-level is_io annotation on extern
             * decls is a v3.1+ item (see 84-CONTEXT.md "Claude's Discretion"
             * note on I/O list expansion). The check fires once per matched
             * name; the rest of the call pipeline still runs so arg types
             * and return-type propagation stay intact. */
            if (ctx->in_pure_method && ce->callee &&
                ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *fn_id = (Iron_Ident *)ce->callee;
                static const char *const IRON_PURE_IO_BUILTINS[] = {
                    "println", "print", "readline",
                };
                if (fn_id->name) {
                    for (size_t i = 0;
                         i < sizeof(IRON_PURE_IO_BUILTINS) /
                             sizeof(IRON_PURE_IO_BUILTINS[0]);
                         i++) {
                        if (strcmp(fn_id->name, IRON_PURE_IO_BUILTINS[i]) == 0) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "cannot call I/O function '%s' in pure method",
                                     fn_id->name);
                            emit_error(ctx, IRON_ERR_PURE_IO, ce->span, msg, NULL);
                            break;
                        }
                    }
                }
            }
            /* Phase 22 READ-04: readonly method calling I/O builtin function.
             * Mirrors IRON_ERR_PURE_IO but for the readonly tier.
             * §6: readonly methods may not perform I/O (file, network, console, log).
             * Pitfall 1 guard: !in_pure_method prevents double-emit when the
             * enclosing method is pure (ctx->in_readonly_method is true for BOTH
             * pure and readonly methods — see typecheck.c:5700). */
            if (ctx->in_readonly_method && !ctx->in_pure_method &&
                ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *fn_id_ro = (Iron_Ident *)ce->callee;
                if (fn_id_ro->name) {
                    /* REUSE IRON_PURE_IO_BUILTINS declared in the pure block above. */
                    static const char *const IRON_RO_IO_BUILTINS[] = {
                        "println", "print", "readline",
                    };
                    for (size_t i = 0;
                         i < sizeof(IRON_RO_IO_BUILTINS) /
                             sizeof(IRON_RO_IO_BUILTINS[0]);
                         i++) {
                        if (strcmp(fn_id_ro->name, IRON_RO_IO_BUILTINS[i]) == 0) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "cannot call I/O function '%s' in readonly method",
                                     fn_id_ro->name);
                            emit_error(ctx, IRON_ERR_READONLY_IO, ce->span, msg,
                                       "§6: readonly methods may not perform I/O");
                            break;
                        }
                    }
                }
            }

            /* Special case: len(array) -> Int.
             * The len builtin is registered as len(String)->Int, but we also
             * support len([T]) -> Int.  Detect this pattern early and bypass the
             * strict argument type check. */
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT &&
                ce->arg_count == 1) {
                Iron_Ident *fn_id = (Iron_Ident *)ce->callee;
                if (strcmp(fn_id->name, "len") == 0) {
                    Iron_Type *arg_t = check_expr(ctx, ce->args[0]);
                    if (arg_t && arg_t->kind == IRON_TYPE_ARRAY) {
                        result = iron_type_make_primitive(IRON_TYPE_INT);
                        ce->resolved_type = result;
                        break;
                    }
                }
            }

            /* Special case: fill(count, value) -> [T].
             * Registered as fill(Int, Int) -> [Int] but we infer the element
             * type from the second argument to support fill(n, 0.0) -> [Float]. */
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT &&
                ce->arg_count == 2) {
                Iron_Ident *fn_id = (Iron_Ident *)ce->callee;
                if (strcmp(fn_id->name, "fill") == 0) {
                    Iron_Type *count_t = check_expr(ctx, ce->args[0]);
                    Iron_Type *val_t   = check_expr(ctx, ce->args[1]);
                    /* Count must be Int */
                    if (count_t && count_t->kind != IRON_TYPE_INT &&
                        count_t->kind != IRON_TYPE_ERROR) {
                        emit_error(ctx, IRON_ERR_ARG_TYPE, ce->args[0]->span,
                                   "fill() first argument must be Int", NULL);
                    }
                    /* Return type is [T] where T is the type of val */
                    if (val_t) {
                        result = iron_type_make_array(ctx->arena, val_t, -1, false);
                    } else {
                        result = iron_type_make_array(ctx->arena,
                                   iron_type_make_primitive(IRON_TYPE_INT), -1, false);
                    }
                    ce->resolved_type = result;
                    break;
                }
            }

            /* Check arg count */
            int expected_count = callee_type->func.param_count;

            /* Phase 18 PARM-03: lookup callee FuncDecl once for per-param
             * is_var enforcement at the call site. Free-function path; the
             * IRON_NODE_METHOD_CALL handler below carries the method-call
             * sibling check (Pitfall 3 method-call coverage lock). */
            Iron_FuncDecl *fd_for_parm = NULL;
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *fn_id = (Iron_Ident *)ce->callee;
                Iron_Symbol *fn_sym =
                    fn_id->name
                        ? iron_scope_lookup(ctx->global_scope, fn_id->name)
                        : NULL;
                if (fn_sym && fn_sym->sym_kind == IRON_SYM_FUNCTION &&
                    fn_sym->decl_node &&
                    fn_sym->decl_node->kind == IRON_NODE_FUNC_DECL) {
                    fd_for_parm = (Iron_FuncDecl *)fn_sym->decl_node;
                }
            }

            if (ce->arg_count != expected_count) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "expected %d argument(s), got %d",
                         expected_count, ce->arg_count);
                emit_error(ctx, IRON_ERR_ARG_COUNT, ce->span, msg, NULL);
                for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
            } else {
                /* Check arg types */
                for (int i = 0; i < ce->arg_count; i++) {
                    Iron_Type *param_type = callee_type->func.param_types[i];
                    Iron_Type *arg_type = check_expr_with_expected(ctx, ce->args[i], param_type);
                    /* Phase 20 PTR-07: auto-address shape check. When the
                     * param is `*T` / `*var T` and the arg is a non-pointer
                     * matching the pointee structurally, the type-mismatch
                     * branch is suppressed — auto-address (handled below)
                     * inserts the implicit `&` and the call still type-
                     * checks. The downstream PTR-07 block emits E0270 (on
                     * rvalue) / E0267 (on val→*var T) where applicable. */
                    bool auto_address_applies =
                        param_type && param_type->kind == IRON_TYPE_PTR &&
                        !param_type->ptr.is_unchecked && /* Phase 25 UNCK-05 (Plan 25-01): suppress auto-address for *unchecked T params */
                        arg_type && arg_type->kind != IRON_TYPE_PTR &&
                        arg_type->kind != IRON_TYPE_ERROR &&
                        param_type->ptr.pointee &&
                        iron_type_equals(param_type->ptr.pointee, arg_type);
                    if (param_type && arg_type &&
                        param_type->kind != IRON_TYPE_ERROR &&
                        arg_type->kind   != IRON_TYPE_ERROR &&
                        !auto_address_applies &&
                        !types_assignable(param_type, arg_type) &&
                        !is_int_literal_narrowing(param_type, arg_type, ce->args[i])) {
                        /* Phase 25 PTR-02/03/UNCK-05 (Plan 25-01): specialize to
                         * E0289 IRON_ERR_PTR_REGIME_MISMATCH when both types are
                         * IRON_TYPE_PTR and is_unchecked differs (regime crossing
                         * at call-arg site). Specificity over generic IRON_ERR_ARG_TYPE.
                         * RESEARCH Pitfall 2: same regime-mismatch predicate fires at
                         * val/var-decl + call-arg + return (three sites). */
                        if (param_type->kind == IRON_TYPE_PTR &&
                            arg_type->kind   == IRON_TYPE_PTR &&
                            param_type->ptr.is_unchecked != arg_type->ptr.is_unchecked) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "argument %d regime mismatch: expected '%s', got '%s'; "
                                     "checked and unchecked pointer regimes are disjoint",
                                     i + 1,
                                     iron_type_to_string(param_type, ctx->arena),
                                     iron_type_to_string(arg_type, ctx->arena));
                            emit_error(ctx, IRON_ERR_PTR_REGIME_MISMATCH,
                                       ce->args[i]->span, msg,
                                       "§4.3-§4.4: checked and unchecked pointer regimes are disjoint; "
                                       "use Box.unwrap() to escape from Box[T] to *unchecked T");
                        } else {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "argument %d type mismatch: expected '%s', got '%s'",
                                     i + 1,
                                     iron_type_to_string(param_type, ctx->arena),
                                     iron_type_to_string(arg_type, ctx->arena));
                            emit_error(ctx, IRON_ERR_ARG_TYPE, ce->args[i]->span, msg, NULL);
                        }
                    }
                    /* Narrow literal args to match parameter type */
                    if (is_int_literal_narrowing(param_type, arg_type, ce->args[i])) {
                        ((Iron_IntLit *)ce->args[i])->resolved_type = param_type;
                    }
                    /* Phase 24 DROP-08 (Plan 24-02): nocopy type passed by value — E0286.
                     * Site (b): IRON_NODE_FUNC_CALL param-pass. Only fires when the arg
                     * is an IDENT (copy from existing binding). Constructed values are moves. */
                    if (arg_type && arg_type->kind == IRON_TYPE_OBJECT &&
                        arg_type->object.decl && arg_type->object.decl->is_nocopy &&
                        param_type && param_type->kind == IRON_TYPE_OBJECT &&
                        ce->args[i] && ce->args[i]->kind == IRON_NODE_IDENT) {
                        emit_error(ctx, IRON_ERR_COPY_OF_NOCOPY_TYPE, ce->args[i]->span,
                                   "cannot pass nocopy type by value — parameter requires copy",
                                   "§7: nocopy types cannot be copied; pass `*T` or `*var T` to avoid copy");
                    }
                    /* Phase 18 PARM-03: read-only argument passed to a
                     * 'var' parameter slot. arg_source_is_mutable returns
                     * false for val bindings, val fields, literals, calls,
                     * and any non-IDENT/non-FIELD_ACCESS rvalue (Pitfall 7
                     * lock — hint mentions only `var`, not `*var`). */
                    if (fd_for_parm && i < fd_for_parm->param_count &&
                        fd_for_parm->params[i] &&
                        fd_for_parm->params[i]->kind == IRON_NODE_PARAM) {
                        Iron_Param *fp = (Iron_Param *)fd_for_parm->params[i];
                        if (fp->is_var &&
                            !arg_source_is_mutable(ctx, ce->args[i])) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "cannot pass read-only argument to "
                                     "'var' parameter '%s'",
                                     fp->name ? fp->name : "<unnamed>");
                            emit_error(ctx,
                                       IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT,
                                       ce->args[i]->span, msg,
                                       "make the argument source mutable "
                                       "(declare as 'var')");
                        }
                    }

                    /* Phase 20 PTR-07 (Plan 20-02a): auto-address insertion
                     * at call sites. When the param is `*T` / `*var T` and
                     * the arg is a non-pointer expression matching the
                     * pointee type, set is_auto_address_target on the arg
                     * (flag-on-existing-node per Pitfall 4 — formatter must
                     * not see a synthesized `&` wrapper, parity-fmt gate
                     * stays green). Three branches:
                     *   (a) arg is an rvalue (literal, call result, binop)
                     *       → emit E0270 "& on rvalue".
                     *   (b) param is *var T but arg source is not mutable
                     *       → emit E0267 (PARM-03 reused per CONTEXT.md
                     *       lock; same code Phase 18 ships for var params).
                     *   (c) auto-address ok → set the flag.
                     * Skip when the arg is already an Iron_UnaryExpr with
                     * op==IRON_TOK_AMP (explicit & path resolves to *T at
                     * the unary handler; types_assignable already accepted
                     * it above). */
                    if (param_type && param_type->kind == IRON_TYPE_PTR &&
                        arg_type && arg_type->kind != IRON_TYPE_PTR &&
                        arg_type->kind != IRON_TYPE_ERROR &&
                        param_type->ptr.pointee &&
                        iron_type_equals(param_type->ptr.pointee, arg_type)) {
                        if (!is_lvalue_expression(ce->args[i])) {
                            emit_error(ctx, IRON_ERR_PTR_AMP_ON_RVALUE,
                                       ce->args[i]->span,
                                       "cannot take address of rvalue "
                                       "(literal or temporary)",
                                       "auto-address requires a named "
                                       "binding, field, or element; bind "
                                       "to a local first");
                        } else if (param_type->ptr.is_var &&
                                   !arg_source_is_mutable(ctx, ce->args[i])) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "cannot pass read-only argument to "
                                     "'*var %s' parameter",
                                     iron_type_to_string(
                                         param_type->ptr.pointee, ctx->arena));
                            emit_error(ctx,
                                       IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT,
                                       ce->args[i]->span, msg,
                                       "make the argument source mutable "
                                       "(declare as 'var') or pass an "
                                       "explicit '&'");
                        } else {
                            set_auto_address_target_flag(ce->args[i]);
                        }
                    }
                }
            }

            /* Check generic constraints if callee is a generic function */
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *fn_id = (Iron_Ident *)ce->callee;
                Iron_Symbol *fn_sym = iron_scope_lookup(ctx->global_scope, fn_id->name);
                if (fn_sym && fn_sym->sym_kind == IRON_SYM_FUNCTION && fn_sym->decl_node) {
                    /* HARD-10 REPLACE (audit row typecheck.c:1607):
                     * fn_sym->decl_node can be IRON_NODE_ERROR after parse
                     * recovery — skip generic constraint check gracefully. */
                    if (fn_sym->decl_node->kind != IRON_NODE_FUNC_DECL) {
                        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_NOTE,
                                       IRON_ERR_UNDEFINED_VAR, ce->span,
                                       "skipping generic-constraint check on partially-parsed function",
                                       NULL);
                        goto skip_generic_constraints;
                    }
                    Iron_FuncDecl *fd = (Iron_FuncDecl *)fn_sym->decl_node;
                    if (fd->generic_param_count > 0 && fd->generic_params) {
                        Iron_Type *concrete[16];
                        int gc = fd->generic_param_count < 16 ? fd->generic_param_count : 16;
                        for (int gi = 0; gi < gc; gi++) {
                            concrete[gi] = NULL;
                            /* PROT-03 row 15 (AUDIT-01 M-severity): assert kind
                             * on fd->generic_params[gi] before the Iron_Ident cast. */
                            if (fd->generic_params[gi])
                                IRON_NODE_ASSERT_KIND(fd->generic_params[gi], IRON_NODE_IDENT);
                            Iron_Ident *gp = (Iron_Ident *)fd->generic_params[gi];
                            if (!gp) continue;
                            for (int pi = 0; pi < fd->param_count && pi < ce->arg_count; pi++) {
                                Iron_Param *fp = (Iron_Param *)fd->params[pi];
                                if (fp && fp->type_ann && fp->type_ann->kind == IRON_NODE_TYPE_ANNOTATION) {
                                    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)fp->type_ann;
                                    if (strcmp(ta->name, gp->name) == 0) {
                                        concrete[gi] = check_expr(ctx, ce->args[pi]);
                                        break;
                                    }
                                    if (ta->is_array && concrete[gi] == NULL) {
                                        Iron_Type *arg_t = check_expr(ctx, ce->args[pi]);
                                        if (arg_t && arg_t->kind == IRON_TYPE_ARRAY) {
                                            concrete[gi] = arg_t->array.elem;
                                        }
                                    }
                                }
                            }
                        }
                        check_generic_constraints(ctx, fd->generic_params, fd->generic_param_count,
                                                  concrete, gc, ce->span);
                    }
                }
            }
            skip_generic_constraints: ;

            result = callee_type->func.return_type
                     ? callee_type->func.return_type
                     : iron_type_make_primitive(IRON_TYPE_VOID);
            ce->resolved_type = result;
            break;
        }

        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            Iron_Type *obj_type_mc = check_expr(ctx, mc->object);
            for (int i = 0; i < mc->arg_count; i++) check_expr(ctx, mc->args[i]);

            /* Phase 20 PTR-06 (Plan 20-02a): auto-deref the receiver when
             * its resolved type is `*T` / `*var T`. Set is_auto_deref on the
             * Iron_MethodCallExpr so HIR lowering (Plan 20-02b) emits a
             * `iron_check_pointer_gen` + load before dispatching the method
             * against the pointee type. Single-level only per CONTEXT.md
             * lock; multi-level **T receivers fall through to existing
             * not-found / not-callable diagnostics downstream. */
            if (obj_type_mc && obj_type_mc->kind == IRON_TYPE_PTR &&
                obj_type_mc->ptr.pointee &&
                obj_type_mc->ptr.pointee->kind != IRON_TYPE_PTR) {
                obj_type_mc = obj_type_mc->ptr.pointee;
                mc->is_auto_deref = true;
            }

            /* Phase 85 INIT-09 E0249 + INIT-14 E0251: inside an init body,
             *   (a) calling self.<anything> while any field is still
             *       unassigned emits E0249 (method call on partial self);
             *   (b) calling self.init(...) OR self.<named_init>() where
             *       <named_init> matches an is_init=true MethodDecl on the
             *       enclosing type emits E0251 (init delegation);
             *   (c) calling Type.<named_init>() where Type is the enclosing
             *       object type and <named_init> is a named init on that
             *       type also emits E0251 (explicit named-init delegation).
             * Both fire regardless of whether the callee is later found in
             * the auto-static / instance dispatch code below. */
            if (ctx->in_init_method && mc->object &&
                mc->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *obj_id_init = (Iron_Ident *)mc->object;
                bool object_is_self = obj_id_init->name &&
                                      strcmp(obj_id_init->name, "self") == 0;
                if (object_is_self) {
                    /* E0249: any self.<method>() while unassigned set non-empty. */
                    if (ctx->unassigned_fields &&
                        shlen(ctx->unassigned_fields) > 0) {
                        const char *first = ctx->unassigned_fields[0].key;
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "method call on partially-constructed self "
                                 "(field '%s' still unassigned)",
                                 first ? first : "?");
                        emit_error(ctx, IRON_ERR_INIT_METHOD_ON_PARTIAL,
                                   mc->span, msg, NULL);
                    }
                    /* E0251: delegation via self.init(...) or self.<named>. */
                    if (mc->method) {
                        bool is_anon_delegation = strcmp(mc->method, "init") == 0;
                        bool is_named_delegation =
                            !is_anon_delegation && ctx->current_method_type &&
                            iron_find_init_by_name(ctx->program,
                                                   ctx->current_method_type,
                                                   mc->method) != NULL;
                        if (is_anon_delegation || is_named_delegation) {
                            emit_error(ctx, IRON_ERR_INIT_DELEGATION,
                                       mc->span,
                                       "init cannot delegate to another init",
                                       NULL);
                        }
                    }
                } else {
                    /* E0251 case (c): Type.<named_init>() where the IDENT
                     * resolves to SYM_TYPE matching the enclosing current
                     * type AND <named_init> is an is_init=true MethodDecl
                     * on that type. */
                    Iron_Symbol *obj_sym_init = NULL;
                    if (obj_id_init->resolved_sym) {
                        obj_sym_init = obj_id_init->resolved_sym;
                    } else if (obj_id_init->name) {
                        obj_sym_init = iron_scope_lookup(
                            ctx->current_scope, obj_id_init->name);
                    }
                    if (obj_sym_init &&
                        obj_sym_init->sym_kind == IRON_SYM_TYPE &&
                        ctx->current_method_type && obj_id_init->name &&
                        strcmp(obj_id_init->name, ctx->current_method_type) == 0 &&
                        mc->method &&
                        iron_find_init_by_name(ctx->program, obj_id_init->name,
                                               mc->method) != NULL) {
                        emit_error(ctx, IRON_ERR_INIT_DELEGATION, mc->span,
                                   "init cannot delegate to another init of "
                                   "the enclosing type",
                                   NULL);
                    }
                }
            }

            /* Phase 87-02 SELF-03: Self.name(args) inside a method body
             * dispatches to the enclosing type's named init. Rewrite the
             * receiver ident from "Self" to the concrete enclosing type name
             * so the existing named-init dispatch below handles it. If outside
             * a method context, emit E0259. */
            if (mc->object && mc->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *self_recv = (Iron_Ident *)mc->object;
                if (self_recv->name && strcmp(self_recv->name, "Self") == 0) {
                    if (!ctx->enclosing_type_name) {
                        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                       IRON_ERR_SELF_OUTSIDE_CONTEXT,
                                       mc->span,
                                       "'Self' is only valid in method or interface "
                                       "signature return types, not in free function "
                                       "or top-level contexts",
                                       NULL);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                        mc->resolved_type = result;
                        break;
                    }
                    /* Rewrite Self -> concrete type so named-init dispatch works.
                     * Also set resolved_sym to the global type symbol so the
                     * named-init dispatch below (which looks up resolved_sym)
                     * finds the type without depending on local scope. */
                    self_recv->name = ctx->enclosing_type_name;
                    Iron_Symbol *concrete_sym =
                        iron_scope_lookup(ctx->global_scope, ctx->enclosing_type_name);
                    if (concrete_sym) {
                        self_recv->resolved_sym = concrete_sym;
                        self_recv->resolved_type = concrete_sym->type;
                        obj_type_mc = concrete_sym->type;
                    }
                }
            }

            /* Phase 85 INIT-08: named-init dispatch. When the method's
             * object is a bare IDENT resolving to SYM_TYPE AND <method> is
             * a named init on that type, handle the call as init dispatch:
             * arg-check against init params (skipping synth self), rewrite
             * the result type to the type's resolved type so a downstream
             * `val p: P = P.zero()` assignment does not see Void. Skip the
             * regular auto-static / instance-method resolver path which
             * would otherwise trigger MUT-04 on the synth mut self receiver
             * and leave result=Void. */
            if (mc->object && mc->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *obj_id_ni = (Iron_Ident *)mc->object;
                Iron_Symbol *obj_sym_ni = obj_id_ni->resolved_sym
                    ? obj_id_ni->resolved_sym
                    : (obj_id_ni->name
                        ? iron_scope_lookup(ctx->current_scope, obj_id_ni->name)
                        : NULL);
                if (obj_sym_ni && obj_sym_ni->sym_kind == IRON_SYM_TYPE &&
                    mc->method) {
                    Iron_MethodDecl *named_init = iron_find_init_by_name(
                        ctx->program, obj_id_ni->name, mc->method);
                    if (named_init) {
                        int init_param_count = named_init->param_count > 0
                            ? named_init->param_count - 1 : 0;
                        if (mc->arg_count != init_param_count) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "init '%s.%s' expects %d argument(s), got %d",
                                     obj_id_ni->name, mc->method,
                                     init_param_count, mc->arg_count);
                            emit_error(ctx, IRON_ERR_ARG_COUNT, mc->span, msg, NULL);
                        } else {
                            for (int i = 0; i < mc->arg_count; i++) {
                                /* args were already check_expr'd at the top
                                 * of this case; re-check here is idempotent
                                 * (check_expr sets resolved_type once). */
                                Iron_Type *at = check_expr(ctx, mc->args[i]);
                                Iron_Param *pp =
                                    (Iron_Param *)named_init->params[i + 1];
                                Iron_Type *pt = pp
                                    ? resolve_type_annotation(ctx, pp->type_ann)
                                    : NULL;
                                if (at && pt &&
                                    at->kind != IRON_TYPE_ERROR &&
                                    pt->kind != IRON_TYPE_ERROR &&
                                    !types_assignable(pt, at) &&
                                    !is_int_literal_narrowing(pt, at, mc->args[i])) {
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "init param '%s' expects '%s', got '%s'",
                                             pp && pp->name ? pp->name : "?",
                                             iron_type_to_string(pt, ctx->arena),
                                             iron_type_to_string(at, ctx->arena));
                                    emit_error(ctx, IRON_ERR_ARG_TYPE,
                                               mc->args[i]->span, msg, NULL);
                                }
                                if (is_int_literal_narrowing(pt, at, mc->args[i])) {
                                    ((Iron_IntLit *)mc->args[i])->resolved_type = pt;
                                }
                            }
                        }
                        result = obj_sym_ni->type;
                        mc->resolved_type = result;
                        break;
                    }
                }
            }

            /* Try to resolve the return type by finding the matching method decl.
             * This handles both auto-static (Math.sin) and instance method calls. */
            result = iron_type_make_primitive(IRON_TYPE_VOID);
            if (mc->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *obj_id = (Iron_Ident *)mc->object;
                const char *type_name_mc = NULL;
                /* Phase 20 PTR-06 (Plan 20-02a): when the receiver is `*T` or
                 * `*var T`, resolve the method against the pointee's
                 * ObjectDecl by treating the pointee as the effective
                 * receiver type for dispatch. mc->is_auto_deref was set
                 * earlier in this branch when the auto-deref kicked in. */
                Iron_Type *eff_recv_t = obj_id->resolved_type;
                if (eff_recv_t && eff_recv_t->kind == IRON_TYPE_PTR &&
                    eff_recv_t->ptr.pointee) {
                    eff_recv_t = eff_recv_t->ptr.pointee;
                }
                if (obj_id->resolved_sym &&
                    obj_id->resolved_sym->sym_kind == IRON_SYM_TYPE) {
                    /* Auto-static: receiver is the type itself */
                    type_name_mc = obj_id->name;
                } else if (eff_recv_t &&
                           eff_recv_t->kind == IRON_TYPE_OBJECT) {
                    /* Instance method: receiver has object type (post-auto-deref
                     * for *T receivers). */
                    type_name_mc = eff_recv_t->object.decl->name;
                } else if (obj_id->resolved_type &&
                           obj_id->resolved_type->kind == IRON_TYPE_STRING) {
                    /* String instance method: resolve via string.iron wrapper decls */
                    type_name_mc = "String";
                } else if (obj_id->resolved_type &&
                           obj_id->resolved_type->kind == IRON_TYPE_INT) {
                    /* Phase 78 FMT-01: Int instance method (e.g. n.to_string()).
                     * Resolve via int.iron wrapper decls (unconditionally prepended
                     * by build.c / check.c — parallel to string.iron). */
                    type_name_mc = "Int";
                } else if (obj_id->resolved_type &&
                           obj_id->resolved_type->kind == IRON_TYPE_INT32) {
                    /* Phase 78 FMT-02: Int32 instance method (e.g. n.to_string()).
                     * Int32.to_string is a distinct method, NOT a widening delegate
                     * through Int (CONTEXT.md: type-first design). */
                    type_name_mc = "Int32";
                } else if (obj_id->resolved_type &&
                           obj_id->resolved_type->kind == IRON_TYPE_FLOAT) {
                    /* Phase 78 FMT-03: Float instance method (e.g. f.to_string()).
                     * Resolve via float.iron wrapper decls. */
                    type_name_mc = "Float";
                } else if (obj_id->resolved_type &&
                           obj_id->resolved_type->kind == IRON_TYPE_ARRAY) {
                    /* Collection method: try extension method decls first,
                     * fall back to built-in heuristics for push/pop/len/etc. */
                    Iron_Type *arr_type = obj_id->resolved_type;
                    Iron_Type *ext_result = resolve_array_ext_method(ctx, mc, arr_type);
                    result = ext_result ? ext_result
                                        : resolve_array_builtin_method(mc->method, arr_type);

                    /* Phase 56 Plan 02: Validate .push(arg) against elem type.
                     * Prevents silent miscompilation for narrowed mono collections
                     * (e.g. `var shapes = [Circle(1)]; shapes.push(Square(2))`).
                     * Fires when there's no explicit extension method decl (so we
                     * fell through to the builtin heuristic) and it's a single-arg
                     * push. */
                    if (ext_result == NULL && strcmp(mc->method, "push") == 0 &&
                        mc->arg_count == 1 && arr_type->array.elem) {
                        /* check_expr is idempotent — args were already checked at
                         * line 1407 above, so this just fetches the resolved type. */
                        Iron_Type *arg_type = check_expr(ctx, mc->args[0]);
                        if (arg_type &&
                            !push_type_compatible(arr_type->array.elem, arg_type)) {
                            /* iron_type_to_string returns "<object>" / "<interface>"
                             * for object/interface types, so we fetch the decl name
                             * directly to get Circle / Square / Shape in the message. */
                            const char *expected_s = type_display_name(
                                arr_type->array.elem, ctx->arena);
                            const char *actual_s = type_display_name(
                                arg_type, ctx->arena);
                            char msg[512];
                            snprintf(msg, sizeof(msg),
                                "cannot push value of type '%s' onto array of "
                                "element type '%s': the collection narrows to a "
                                "single concrete type",
                                actual_s, expected_s);
                            emit_error(ctx, IRON_ERR_TYPE_MISMATCH, mc->span, msg,
                                "to push mixed types, annotate the variable with "
                                "an interface array type, e.g. `var xs: [Shape] = ...`");
                        }
                    }

                    mc->resolved_type = result;
                    break;  /* skip decl scan — return type already resolved */
                } else if (obj_id->resolved_type &&
                           obj_id->resolved_type->kind == IRON_TYPE_INTERFACE &&
                           obj_id->resolved_type->interface.decl) {
                    /* Interface dispatch: find the method in the interface's
                     * method signatures and resolve the return type. */
                    Iron_InterfaceDecl *iface_mc = obj_id->resolved_type->interface.decl;
                    for (int mi = 0; mi < iface_mc->method_count; mi++) {
                        Iron_Node *msig = iface_mc->method_sigs[mi];
                        if (!msig || msig->kind != IRON_NODE_FUNC_DECL) continue;
                        Iron_FuncDecl *fd = (Iron_FuncDecl *)msig;
                        if (strcmp(fd->name, mc->method) != 0) continue;
                        if (fd->resolved_return_type) {
                            result = fd->resolved_return_type;
                        } else if (fd->return_type &&
                                   fd->return_type->kind == IRON_NODE_TYPE_ANNOTATION) {
                            /* Resolve return type from annotation */
                            Iron_TypeAnnotation *rta = (Iron_TypeAnnotation *)fd->return_type;
                            Iron_Type *resolved_rt = resolve_type_annotation(ctx, (Iron_Node *)rta);
                            if (resolved_rt) result = resolved_rt;
                        }
                        break;
                    }
                    mc->resolved_type = result;
                    break;
                } else if (obj_id->resolved_type &&
                           obj_id->resolved_type->kind == IRON_TYPE_ENUM &&
                           obj_id->resolved_type->enu.decl) {
                    /* Instance method on enum value */
                    type_name_mc = obj_id->resolved_type->enu.decl->name;
                }
                if (type_name_mc && ctx->program) {
                    for (int i = 0; i < ctx->program->decl_count; i++) {
                        Iron_Node *d = ctx->program->decls[i];
                        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
                        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
                        if (strcmp(md->type_name, type_name_mc) == 0 &&
                            strcmp(md->method_name, mc->method) == 0) {
                            if (md->resolved_return_type) {
                                result = md->resolved_return_type;
                            }
                            /* Phase 80 MUT-04: if callee is a receiver-form method
                             * whose receiver binding was declared `mut`, require the
                             * caller's receiver expression to be rooted in a mutable
                             * binding. Non-ident receivers (chained calls, literals)
                             * are skipped — Phase 80 only enforces the ident-rooted
                             * case per CONTEXT.md. */
                            if (md->is_receiver_form && md->param_count > 0) {
                                Iron_Param *recv_p = (Iron_Param *)md->params[0];
                                if (recv_p && recv_p->is_mut_receiver) {
                                    /* The real receiver identifier depends on
                                     * the call form:
                                     *   Instance method  `x.m(...)`    -> obj_id
                                     *   Auto-static      `T.m(x, ...)` -> args[0]
                                     * obj_id is a TYPE symbol in the auto-static
                                     * case; its is_mutable is always false and
                                     * must not be used as the mutability check
                                     * (Phase 89/91 regression). Fall back to the
                                     * first argument when it is an ident. */
                                    Iron_Ident *recv_ident = NULL;
                                    if (obj_id->resolved_sym &&
                                        obj_id->resolved_sym->sym_kind == IRON_SYM_TYPE) {
                                        if (mc->arg_count > 0 &&
                                            mc->args[0] &&
                                            mc->args[0]->kind == IRON_NODE_IDENT) {
                                            recv_ident = (Iron_Ident *)mc->args[0];
                                        }
                                    } else {
                                        recv_ident = obj_id;
                                    }
                                    if (recv_ident && recv_ident->resolved_sym &&
                                        recv_ident->resolved_sym->sym_kind != IRON_SYM_TYPE &&
                                        !recv_ident->resolved_sym->is_mutable) {
                                        char msg[256];
                                        snprintf(msg, sizeof(msg),
                                                 "cannot call mutable method on immutable binding");
                                        emit_error(ctx, IRON_ERR_MUT_CALL_ON_VAL,
                                                   mc->span, msg, NULL);
                                    }
                                    /* Phase 84 MUTTIER-02 E0239: readonly caller
                                     * calling a mutating callee (is_mut_receiver
                                     * AND NOT readonly/pure). The receiver-param
                                     * is_mut_receiver bit is the canonical "this
                                     * method writes self" signal established by
                                     * Plan 83-02 (synth setter=true). */
                                    bool callee_is_mutating =
                                        !md->is_readonly && !md->is_pure;
                                    if (ctx->in_readonly_method && callee_is_mutating) {
                                        char msg[256];
                                        snprintf(msg, sizeof(msg),
                                                 "cannot call mutating method '%s.%s' "
                                                 "from readonly context",
                                                 md->type_name, md->method_name);
                                        emit_error(ctx,
                                                   IRON_ERR_READONLY_CALLS_MUTATING,
                                                   mc->span, msg,
                                                   "§6: readonly methods may not call non-readonly functions");
                                    }
                                }
                            }
                            /* Phase 84 MUTTIER-03 E0242: pure caller calling a
                             * non-pure callee. Applies to every method call from
                             * pure context regardless of receiver-mutability
                             * (pure can only call pure — readonly is not pure
                             * enough). Skip the synth-accessor getter special
                             * case: synth getters are retrofitted is_pure=true
                             * in Plan 84-01, so `self.pub_field` reads from a
                             * pure method already pass md->is_pure here. */
                            if (ctx->in_pure_method && !md->is_pure) {
                                char msg[256];
                                snprintf(msg, sizeof(msg),
                                         "cannot call non-pure method '%s.%s' "
                                         "from pure method",
                                         md->type_name, md->method_name);
                                emit_error(ctx, IRON_ERR_PURE_NON_PURE_CALL,
                                           mc->span, msg, NULL);
                            }
                            /* Phase 18 PARM-03: per-arg read-only-source check
                             * at the method call site (Pitfall 3 method-call
                             * coverage lock — free-function-only fix leaves
                             * v4 method fixtures unprotected).
                             *
                             * For receiver-form methods, params[0] is the
                             * receiver binding and user args start at
                             * params[1]; offset accordingly so mc->args[i]
                             * lines up with params[i + recv_off]. */
                            int recv_off = md->is_receiver_form ? 1 : 0;
                            for (int ai = 0; ai < mc->arg_count; ai++) {
                                int pi = ai + recv_off;
                                if (pi >= md->param_count) break;
                                if (!md->params[pi] ||
                                    md->params[pi]->kind != IRON_NODE_PARAM)
                                    continue;
                                Iron_Param *mp = (Iron_Param *)md->params[pi];
                                if (mp->is_var &&
                                    !arg_source_is_mutable(ctx, mc->args[ai])) {
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "cannot pass read-only argument to "
                                             "'var' parameter '%s'",
                                             mp->name ? mp->name : "<unnamed>");
                                    emit_error(ctx,
                                               IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT,
                                               mc->args[ai]->span, msg,
                                               "make the argument source mutable "
                                               "(declare as 'var')");
                                }
                                /* Phase 20 PTR-07 (Plan 20-02a): auto-address
                                 * insertion at method call sites mirrors the
                                 * IRON_NODE_CALL arg-loop. Param `*T` /
                                 * `*var T` + non-pointer arg matching the
                                 * pointee → set is_auto_address_target on
                                 * the existing AST node (Pitfall 4 lock).
                                 * E0270 on rvalue, E0267 on val→*var T. */
                                Iron_Type *mp_t = resolve_type_annotation(
                                    ctx, mp->type_ann);
                                Iron_Type *ma_t = mc->args[ai]
                                    ? ((Iron_ExprNode *)mc->args[ai])->resolved_type
                                    : NULL;
                                if (mp_t && mp_t->kind == IRON_TYPE_PTR &&
                                    ma_t && ma_t->kind != IRON_TYPE_PTR &&
                                    ma_t->kind != IRON_TYPE_ERROR &&
                                    mp_t->ptr.pointee &&
                                    iron_type_equals(mp_t->ptr.pointee, ma_t)) {
                                    if (!is_lvalue_expression(mc->args[ai])) {
                                        emit_error(ctx,
                                                   IRON_ERR_PTR_AMP_ON_RVALUE,
                                                   mc->args[ai]->span,
                                                   "cannot take address of "
                                                   "rvalue (literal or "
                                                   "temporary)",
                                                   "auto-address requires a "
                                                   "named binding, field, or "
                                                   "element");
                                    } else if (mp_t->ptr.is_var &&
                                               !arg_source_is_mutable(
                                                    ctx, mc->args[ai])) {
                                        char msg[256];
                                        snprintf(msg, sizeof(msg),
                                                 "cannot pass read-only "
                                                 "argument to '*var %s' "
                                                 "parameter",
                                                 iron_type_to_string(
                                                     mp_t->ptr.pointee,
                                                     ctx->arena));
                                        emit_error(ctx,
                                                   IRON_ERR_PARM_VAR_SLOT_NEEDS_MUT,
                                                   mc->args[ai]->span, msg,
                                                   "make the argument source "
                                                   "mutable (declare as 'var')");
                                    } else {
                                        set_auto_address_target_flag(mc->args[ai]);
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            } else if (obj_type_mc && obj_type_mc->kind == IRON_TYPE_STRING) {
                /* Non-ident receiver with String type (e.g. string literal, interp string,
                 * or chained method call): resolve via string.iron wrapper decls. */
                if (ctx->program) {
                    for (int i = 0; i < ctx->program->decl_count; i++) {
                        Iron_Node *d = ctx->program->decls[i];
                        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
                        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
                        if (strcmp(md->type_name, "String") == 0 &&
                            strcmp(md->method_name, mc->method) == 0) {
                            if (md->resolved_return_type) {
                                result = md->resolved_return_type;
                            }
                            break;
                        }
                    }
                }
            } else if (obj_type_mc && (obj_type_mc->kind == IRON_TYPE_INT   ||
                                        obj_type_mc->kind == IRON_TYPE_INT32 ||
                                        obj_type_mc->kind == IRON_TYPE_FLOAT)) {
                /* Phase 78 FMT-01/02/03: non-ident receiver with primitive numeric type.
                 * Covers integer/float literals as receivers (42.to_string()) and
                 * chained calls ((a + b).to_string()). */
                const char *tn =
                    (obj_type_mc->kind == IRON_TYPE_INT)   ? "Int"   :
                    (obj_type_mc->kind == IRON_TYPE_INT32) ? "Int32" :
                                                              "Float";
                if (ctx->program) {
                    for (int i = 0; i < ctx->program->decl_count; i++) {
                        Iron_Node *d = ctx->program->decls[i];
                        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
                        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
                        if (strcmp(md->type_name, tn) == 0 &&
                            strcmp(md->method_name, mc->method) == 0) {
                            if (md->resolved_return_type) {
                                result = md->resolved_return_type;
                            }
                            break;
                        }
                    }
                }
            } else if (obj_type_mc && obj_type_mc->kind == IRON_TYPE_ARRAY) {
                /* Non-ident receiver with Array type (e.g. chained method call
                 * like arr.map(...).filter(...)): resolve via extension methods. */
                Iron_Type *ext_result = resolve_array_ext_method(ctx, mc, obj_type_mc);
                result = ext_result ? ext_result
                                    : resolve_array_builtin_method(mc->method, obj_type_mc);

                /* Phase 56 Plan 02: Mirror push arg validation for chained receivers. */
                if (ext_result == NULL && strcmp(mc->method, "push") == 0 &&
                    mc->arg_count == 1 && obj_type_mc->array.elem) {
                    Iron_Type *arg_type = check_expr(ctx, mc->args[0]);
                    if (arg_type &&
                        !push_type_compatible(obj_type_mc->array.elem, arg_type)) {
                        const char *expected_s = type_display_name(
                            obj_type_mc->array.elem, ctx->arena);
                        const char *actual_s = type_display_name(
                            arg_type, ctx->arena);
                        char msg[512];
                        snprintf(msg, sizeof(msg),
                            "cannot push value of type '%s' onto chained array "
                            "result of element type '%s'",
                            actual_s, expected_s);
                        emit_error(ctx, IRON_ERR_TYPE_MISMATCH, mc->span, msg, NULL);
                    }
                }
            }
            mc->resolved_type = result;

            /* Phase 22 READ-04: readonly method calling I/O stdlib module method.
             * Covers Log.info, IO.write_file, Net.connect, Raylib.draw_text, etc.
             * The check fires when (1) the enclosing method is readonly (not pure —
             * Pitfall 1 guard) AND (2) the call receiver is a known I/O module
             * identifier. */
            if (ctx->in_readonly_method && !ctx->in_pure_method &&
                mc->object && mc->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *recv_id_ro = (Iron_Ident *)mc->object;
                if (recv_id_ro->name) {
                    static const char *const IRON_RO_IO_MODULES[] = {
                        "IO", "Log", "Net", "Raylib",
                    };
                    for (size_t i = 0;
                         i < sizeof(IRON_RO_IO_MODULES) /
                             sizeof(IRON_RO_IO_MODULES[0]);
                         i++) {
                        if (strcmp(recv_id_ro->name, IRON_RO_IO_MODULES[i]) == 0) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "cannot call I/O method '%s.%s' in readonly method",
                                     recv_id_ro->name,
                                     mc->method ? mc->method : "?");
                            emit_error(ctx, IRON_ERR_READONLY_IO, mc->span, msg,
                                       "§6: readonly methods may not perform I/O");
                            break;
                        }
                    }
                }
            }

            break;
        }

        case IRON_NODE_FIELD_ACCESS: {
            Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
            Iron_Type *obj_type = check_expr(ctx, fa->object);

            if (!obj_type || obj_type->kind == IRON_TYPE_ERROR) {
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
                fa->resolved_type = result;
                break;
            }

            /* Phase 85 INIT-05 E0246: reading self.<field> inside an init
             * body while the field is still in unassigned_fields is a
             * read-before-assign. Fires BEFORE the pub-access rewrite below
             * so the message is specific to init flow rather than a generic
             * accessor-dispatch error. Suppressed when this FIELD_ACCESS
             * node is the immediate target of an enclosing assignment
             * (`self.x = ...` is a write, not a read). */
            if (ctx->in_init_method && fa->object &&
                fa->object->kind == IRON_NODE_IDENT && fa->field &&
                ctx->cur_assign_target != (Iron_Node *)fa) {
                Iron_Ident *fo = (Iron_Ident *)fa->object;
                if (fo->name && strcmp(fo->name, "self") == 0 &&
                    ctx->unassigned_fields &&
                    shget(ctx->unassigned_fields, fa->field) == 1) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "field '%s' read before assignment in init",
                             fa->field);
                    emit_error(ctx, IRON_ERR_INIT_READ_BEFORE_ASSIGN,
                               fa->span, msg, NULL);
                }
            }

            /* Unwrap rc pointer types to access the inner object type.
             * heap types already expose the inner object type directly (IRON_NODE_HEAP
             * sets resolved_type to the inner construct type, not an RC wrapper). */
            if (obj_type->kind == IRON_TYPE_RC) {
                obj_type = obj_type->rc.inner;
            }

            /* Phase 20 PTR-06 (Plan 20-02a): auto-deref through `*T` /
             * `*var T` receivers. Single-level only per CONTEXT.md lock —
             * if the pointee is itself a pointer, emit an error and bail.
             * `?*T` flow-typing narrowing reuses the existing nullable
             * pipeline upstream of this handler: when an `if p != null`
             * branch narrows IRON_TYPE_NULLABLE{IRON_TYPE_PTR} to its
             * inner IRON_TYPE_PTR via narrowing_set, the IDENT lookup at
             * 1645 returns the PTR type, and we land here naturally. */
            if (obj_type->kind == IRON_TYPE_PTR) {
                if (obj_type->ptr.pointee &&
                    obj_type->ptr.pointee->kind == IRON_TYPE_PTR) {
                    emit_error(ctx, IRON_ERR_PTR_NULL_DEREF, fa->span,
                               "multi-level auto-deref through '**T' is not "
                               "supported in checked regime",
                               "use explicit dereference (Phase 25 Ptr.deref) "
                               "or unwrap one level first");
                    result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    fa->resolved_type = result;
                    break;
                }
                obj_type = obj_type->ptr.pointee;
                fa->is_auto_deref = true;
            }

            if (!obj_type || obj_type->kind != IRON_TYPE_OBJECT) {
                if (obj_type && obj_type->kind == IRON_TYPE_NULLABLE) {
                    emit_error(ctx, IRON_ERR_NULLABLE_ACCESS, fa->span,
                               "cannot access field of nullable type without null check",
                               "Check for null before accessing");
                }
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
                fa->resolved_type = result;
                break;
            }

            Iron_ObjectDecl *od = obj_type->object.decl;
            Iron_Type *field_type = NULL;
            Iron_Field *matched_field = NULL;
            for (int i = 0; i < od->field_count; i++) {
                Iron_Field *f = (Iron_Field *)od->fields[i];
                if (strcmp(f->name, fa->field) == 0) {
                    field_type    = resolve_type_annotation(ctx, f->type_ann);
                    matched_field = f;
                    break;
                }
            }

            if (!field_type) {
                char msg[256];
                snprintf(msg, sizeof(msg), "no field '%s' on type", fa->field);
                emit_error(ctx, IRON_ERR_NO_SUCH_FIELD, fa->span, msg, NULL);
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
            } else {
                result = field_type;
            }
            fa->resolved_type = result;
            /* Phase 83-02 ACCESS-05: flag pub-field reads so HIR can lower
             * them as method calls against the synthesized getter. Default
             * stays false for non-pub fields, preserving the direct-load
             * path (pure-superset guard).
             *
             * Suppressed inside synth accessor bodies — the synth getter's
             * `return self.field` must remain a direct load, otherwise HIR
             * would infinitely re-dispatch through the same getter.
             *
             * NOTE: this fires for every FIELD_ACCESS the typechecker sees,
             * including the LHS of an assignment. The assign handler below
             * may further set Iron_AssignStmt.is_pub_setter on the parent
             * AssignStmt; when is_pub_setter is true, HIR ignores the
             * target's is_pub_access (the target is never lowered — the
             * assign is lowered as set_<field>(value) directly). */
            fa->is_pub_access = (!ctx->in_synth_accessor &&
                                 matched_field && matched_field->is_pub);
            break;
        }

        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;

            /* Phase 85 INIT-14 E0251: inside an init body, constructing an
             * instance of the enclosing type is delegation and is rejected.
             * Fires before the sym lookup so even an anonymous init that
             * doesn't otherwise resolve still surfaces this diagnostic.
             * Fall through after emit so arg-type errors still surface. */
            if (ctx->in_init_method && ctx->current_method_type &&
                ce->type_name &&
                strcmp(ce->type_name, ctx->current_method_type) == 0) {
                emit_error(ctx, IRON_ERR_INIT_DELEGATION, ce->span,
                           "init cannot delegate to another init of the "
                           "enclosing type",
                           NULL);
            }

            Iron_Symbol *sym = iron_scope_lookup(ctx->global_scope, ce->type_name);
            if (!sym) {
                char msg[256];
                snprintf(msg, sizeof(msg), "unknown type or function '%s'", ce->type_name);
                emit_error(ctx, IRON_ERR_NOT_CALLABLE, ce->span, msg, NULL);
                for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
                ce->resolved_type = result;
                break;
            }

            if (sym->sym_kind == IRON_SYM_TYPE) {
                /* PROT-04 rewrite (rank 6, AUDIT-01): sym->decl_node for a
                 * SYM_TYPE may be InterfaceDecl, EnumDecl, or NULL for builtins.
                 * Guard before the concrete Iron_ObjectDecl cast and bail with
                 * a diagnostic for the non-object case. */
                if (!sym->decl_node ||
                    sym->decl_node->kind != IRON_NODE_OBJECT_DECL) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "type '%s' is not constructible", ce->type_name);
                    emit_error(ctx, IRON_ERR_NOT_CALLABLE, ce->span, msg, NULL);
                    for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                    result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    ce->resolved_type = result;
                    break;
                }
                /* HARD-10 REPLACE (audit row typecheck.c:1887):
                 * guard above at line 1898 already handles IRON_NODE_ERROR /
                 * non-OBJECT_DECL cases gracefully — the former assert is now
                 * redundant and its role has been promoted to the explicit
                 * guard, so no runtime abort can fire here. */
                Iron_ObjectDecl *od = (Iron_ObjectDecl *)sym->decl_node;
                int field_count = od->field_count;

                if (ce->arg_count != field_count) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "object '%s' has %d field(s), but %d argument(s) given",
                             ce->type_name, field_count, ce->arg_count);
                    emit_error(ctx, IRON_ERR_ARG_COUNT, ce->span, msg, NULL);
                    for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                } else {
                    for (int i = 0; i < ce->arg_count; i++) {
                        Iron_Type *arg_t = check_expr(ctx, ce->args[i]);
                        Iron_Field *fld = (Iron_Field *)od->fields[i];
                        Iron_Type *fld_t = resolve_type_annotation(ctx, fld->type_ann);
                        if (arg_t && fld_t &&
                            arg_t->kind  != IRON_TYPE_ERROR &&
                            fld_t->kind  != IRON_TYPE_ERROR &&
                            !types_assignable(fld_t, arg_t) &&
                            !is_int_literal_narrowing(fld_t, arg_t, ce->args[i])) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "field '%s' expects '%s', got '%s'",
                                     fld->name,
                                     iron_type_to_string(fld_t, ctx->arena),
                                     iron_type_to_string(arg_t, ctx->arena));
                            emit_error(ctx, IRON_ERR_ARG_TYPE, ce->args[i]->span, msg, NULL);
                        }
                        /* Narrow literal args to match field type */
                        if (is_int_literal_narrowing(fld_t, arg_t, ce->args[i])) {
                            ((Iron_IntLit *)ce->args[i])->resolved_type = fld_t;
                        }
                    }
                }
                /* Check generic constraints on type construction */
                if (od->generic_param_count > 0 && od->generic_params &&
                    ce->generic_arg_count > 0 && ce->generic_args) {
                    Iron_Type *concrete[16];
                    int gc = od->generic_param_count < 16 ? od->generic_param_count : 16;
                    int ac = ce->generic_arg_count < gc ? ce->generic_arg_count : gc;
                    for (int gi = 0; gi < gc; gi++) {
                        concrete[gi] = NULL;
                        if (gi < ac && ce->generic_args[gi]) {
                            concrete[gi] = resolve_type_annotation(ctx, ce->generic_args[gi]);
                        }
                    }
                    check_generic_constraints(ctx, od->generic_params, od->generic_param_count,
                                              concrete, gc, ce->span);
                }
                result = sym->type;
            } else if (sym->sym_kind == IRON_SYM_FUNCTION) {
                Iron_Type *ft = sym->type;
                if (ft && ft->kind == IRON_TYPE_FUNC) {
                    if (ce->arg_count != ft->func.param_count) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "expected %d argument(s), got %d",
                                 ft->func.param_count, ce->arg_count);
                        emit_error(ctx, IRON_ERR_ARG_COUNT, ce->span, msg, NULL);
                    }
                    result = ft->func.return_type
                             ? ft->func.return_type
                             : iron_type_make_primitive(IRON_TYPE_VOID);
                } else {
                    result = iron_type_make_primitive(IRON_TYPE_VOID);
                }
                for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
            } else {
                emit_error(ctx, IRON_ERR_NOT_CALLABLE, ce->span,
                           "expression is not a type or function", NULL);
                for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
            }
            ce->resolved_type = result;
            break;
        }

        case IRON_NODE_IS: {
            Iron_IsExpr *ie = (Iron_IsExpr *)node;
            check_expr(ctx, ie->expr);
            result = iron_type_make_primitive(IRON_TYPE_BOOL);
            ie->resolved_type = result;
            break;
        }

        case IRON_NODE_INDEX: {
            Iron_IndexExpr *idx_e = (Iron_IndexExpr *)node;
            Iron_Type *obj_type = check_expr(ctx, idx_e->object);
            Iron_Type *idx_type = check_expr(ctx, idx_e->index);

            if (obj_type && obj_type->kind == IRON_TYPE_ARRAY) {
                result = obj_type->array.elem;

                /* BOUNDS-03: Validate index expression is an integer type */
                if (idx_type && idx_type->kind != IRON_TYPE_ERROR &&
                    !iron_type_is_integer(idx_type)) {
                    emit_error(ctx, IRON_ERR_TYPE_MISMATCH, idx_e->index->span,
                               "array index must be an integer type", NULL);
                }

                /* BOUNDS-01/02: Check constant index against known array size */
                long long idx_val;
                if (obj_type->array.size >= 0 &&
                    try_get_constant_int(idx_e->index, &idx_val)) {
                    if (idx_val < 0 || idx_val >= obj_type->array.size) {
                        char msg[128];
                        snprintf(msg, sizeof(msg),
                                 "index %lld is out of bounds for array of size %d",
                                 idx_val, obj_type->array.size);
                        emit_error(ctx, IRON_ERR_INDEX_OUT_OF_BOUNDS,
                                   idx_e->index->span, msg, NULL);
                    }
                }
            } else {
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
            }
            idx_e->resolved_type = result;
            break;
        }

        case IRON_NODE_SLICE: {
            Iron_SliceExpr *se = (Iron_SliceExpr *)node;
            Iron_Type *obj_type = check_expr(ctx, se->object);
            Iron_Type *start_type = se->start ? check_expr(ctx, se->start) : NULL;
            Iron_Type *end_type   = se->end   ? check_expr(ctx, se->end)   : NULL;
            result = obj_type ? obj_type : iron_type_make_primitive(IRON_TYPE_ERROR);
            se->resolved_type = result;

            /* SLICE-01: Validate start and end are integer types */
            if (start_type && start_type->kind != IRON_TYPE_ERROR &&
                !iron_type_is_integer(start_type)) {
                emit_error(ctx, IRON_ERR_TYPE_MISMATCH, se->start->span,
                           "slice start must be an integer type", NULL);
            }
            if (end_type && end_type->kind != IRON_TYPE_ERROR &&
                !iron_type_is_integer(end_type)) {
                emit_error(ctx, IRON_ERR_TYPE_MISMATCH, se->end->span,
                           "slice end must be an integer type", NULL);
            }

            /* SLICE-02/03/04: Check constant bounds */
            long long start_val, end_val;
            bool has_start = se->start && try_get_constant_int(se->start, &start_val);
            bool has_end   = se->end   && try_get_constant_int(se->end, &end_val);

            if (has_start && start_val < 0) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "slice start %lld is negative", start_val);
                emit_error(ctx, IRON_ERR_INVALID_SLICE_BOUNDS,
                           se->start->span, msg, NULL);
            } else if (has_start && has_end && start_val > end_val) {
                /* SLICE-02: start <= end */
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "slice start %lld is greater than end %lld",
                         start_val, end_val);
                emit_error(ctx, IRON_ERR_INVALID_SLICE_BOUNDS,
                           se->start->span, msg, NULL);
            }

            /* SLICE-03: end <= array size when all are constants */
            if (has_end && obj_type && obj_type->kind == IRON_TYPE_ARRAY &&
                obj_type->array.size >= 0) {
                if (end_val > obj_type->array.size) {
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                             "slice end %lld exceeds array size %d",
                             end_val, obj_type->array.size);
                    emit_error(ctx, IRON_ERR_INVALID_SLICE_BOUNDS,
                               se->end->span, msg, NULL);
                }
            }

            break;
        }

        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            result = check_expr(ctx, he->inner);
            he->resolved_type = result;
            /* Phase 22 READ-05: readonly method allocating heap memory.
             * Guard: Pitfall 1 — !ctx->in_pure_method prevents double-emit.
             * (Pure-tier heap-escape is a separate future check; readonly-tier
             * owns this diagnostic for non-pure readonly methods.) */
            if (ctx->in_readonly_method && !ctx->in_pure_method) {
                const char *type_nm = (result && result->kind == IRON_TYPE_OBJECT
                                       && result->object.decl
                                       && result->object.decl->name)
                                      ? result->object.decl->name : "T";
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "cannot allocate 'heap %s(...)' in readonly method",
                         type_nm);
                emit_error(ctx, IRON_ERR_READONLY_HEAP_ESCAPE, he->span, msg,
                           "§6: readonly methods may not allocate heap T(...) or rc T(...)");
            }
            break;
        }

        case IRON_NODE_RC: {
            Iron_RcExpr *re = (Iron_RcExpr *)node;
            Iron_Type *inner = check_expr(ctx, re->inner);
            result = inner ? iron_type_make_rc(ctx->arena, inner)
                           : iron_type_make_primitive(IRON_TYPE_ERROR);
            re->resolved_type = result;
            break;
        }

        case IRON_NODE_COMPTIME: {
            Iron_ComptimeExpr *ce = (Iron_ComptimeExpr *)node;
            result = check_expr(ctx, ce->inner);
            ce->resolved_type = result;
            break;
        }

        case IRON_NODE_LAMBDA: {
            Iron_LambdaExpr *le = (Iron_LambdaExpr *)node;
            /* Build the FUNC type for the lambda so it is callable.
             * Collect param types from param annotations. */
            Iron_Type **param_types = NULL;
            if (le->param_count > 0) {
                param_types = (Iron_Type **)iron_arena_alloc(
                    ctx->arena,
                    (size_t)le->param_count * sizeof(Iron_Type *),
                    _Alignof(Iron_Type *));
                if (!param_types) { /* HARD-09 REPLACE (typecheck.c:check_expr LAMBDA param_types) */ return false; }
                for (int p = 0; p < le->param_count; p++) {
                    Iron_Param *ap = (Iron_Param *)le->params[p];
                    param_types[p] = resolve_type_annotation(ctx, ap->type_ann);
                }
            }
            Iron_Type *ret_t = le->return_type
                ? resolve_type_annotation(ctx, le->return_type)
                : iron_type_make_primitive(IRON_TYPE_VOID);
            if (ret_t && ret_t->kind == IRON_TYPE_VOID) ret_t = NULL;
            /* Push a function scope and declare lambda params so the body
             * can type-check variable references correctly. */
            Iron_Type *prev_ret = ctx->current_return_type;
            ctx->current_return_type = ret_t;
            tc_push_scope(ctx, IRON_SCOPE_FUNCTION);
            for (int p = 0; p < le->param_count; p++) {
                Iron_Param *ap = (Iron_Param *)le->params[p];
                tc_define(ctx, ap->name, IRON_SYM_PARAM, (Iron_Node *)le->params[p],
                          ap->span, false, param_types ? param_types[p] : NULL);
            }
            if (le->body) check_stmt(ctx, le->body);
            tc_pop_scope(ctx);
            ctx->current_return_type = prev_ret;
            result = iron_type_make_func(ctx->arena, param_types, le->param_count, ret_t);
            le->resolved_type = result;
            break;
        }

        case IRON_NODE_AWAIT: {
            Iron_AwaitExpr *ae = (Iron_AwaitExpr *)node;
            check_expr(ctx, ae->handle);

            /* Look up the spawn body's return type from the handle name */
            Iron_Type *await_type = iron_type_make_primitive(IRON_TYPE_INT);
            if (ae->handle && ae->handle->kind == IRON_NODE_IDENT) {
                Iron_Ident *ident = (Iron_Ident *)ae->handle;
                int idx = shgeti(ctx->spawn_result_types, ident->name);
                if (idx >= 0) {
                    await_type = ctx->spawn_result_types[idx].value;
                }
            }
            result = await_type;
            ae->resolved_type = result;
            break;
        }

        case IRON_NODE_ARRAY_LIT: {
            Iron_ArrayLit *al = (Iron_ArrayLit *)node;
            /* Phase 59 01d: tuple literal — parser emits these as ARRAY_LIT
             * with a type_ann sentinel (Iron_TypeAnnotation with is_tuple=true).
             * Heterogeneous-retained: we preserve every element type in a
             * fresh IRON_TYPE_TUPLE instead of collapsing to a common type. */
            if (al->type_ann && al->type_ann->kind == IRON_NODE_TYPE_ANNOTATION) {
                Iron_TypeAnnotation *tag = (Iron_TypeAnnotation *)al->type_ann;
                if (tag->is_tuple) {
                    int n = al->element_count;
                    Iron_Type **tup_elems = (Iron_Type **)iron_arena_alloc(
                        ctx->arena, sizeof(Iron_Type *) * (size_t)n,
                        _Alignof(Iron_Type *));
                    if (!tup_elems) { /* HARD-09 REPLACE (typecheck.c:check_expr ARRAY_LIT tuple) */ return false; }
                    for (int i = 0; i < n; i++) {
                        Iron_Type *et = check_expr(ctx, al->elements[i]);
                        if (!et) et = iron_type_make_primitive(IRON_TYPE_ERROR);
                        tup_elems[i] = et;
                    }
                    result = iron_type_make_tuple(ctx->arena, tup_elems, n);
                    al->resolved_type = result;
                    break;
                }
            }
            if (al->size) check_expr(ctx, al->size);
            Iron_Type *elem_type = NULL;
            Iron_Type **elem_types = NULL; /* track all element types for mixed-type detection */
            /* [Type; Size] form: resolve element type from the annotation so
             * the empty-literal E0229 path below does not mis-fire on
             * sized-array syntax (e.g. `heap [UInt8; 100]`). */
            if (al->type_ann && al->element_count == 0) {
                elem_type = resolve_type_annotation(ctx, al->type_ann);
            }
            for (int i = 0; i < al->element_count; i++) {
                Iron_Type *et = check_expr(ctx, al->elements[i]);
                if (!elem_type && et) elem_type = et;
                if (et) arrput(elem_types, et);
            }
            /* Check for mixed-type array: if elements have different object types
             * that all implement a common interface, infer the interface as elem_type */
            if (elem_type && elem_type->kind == IRON_TYPE_OBJECT &&
                arrlen(elem_types) > 1) {
                bool has_different_types = false;
                for (int i = 1; i < arrlen(elem_types); i++) {
                    if (elem_types[i] != elem_type &&
                        !(elem_types[i]->kind == IRON_TYPE_OBJECT &&
                          elem_types[i]->object.decl == elem_type->object.decl)) {
                        has_different_types = true;
                        break;
                    }
                }
                if (has_different_types) {
                    /* Find common interface: check first element's implements list */
                    Iron_ObjectDecl *first_obj = elem_type->object.decl;
                    if (first_obj) {
                        for (int ii = 0; ii < first_obj->implements_count; ii++) {
                            const char *iface_name = first_obj->implements_names[ii];
                            bool all_implement = true;
                            for (int ei = 1; ei < arrlen(elem_types); ei++) {
                                Iron_Type *et2 = elem_types[ei];
                                if (!et2 || et2->kind != IRON_TYPE_OBJECT || !et2->object.decl) {
                                    all_implement = false; break;
                                }
                                Iron_ObjectDecl *od2 = et2->object.decl;
                                bool found = false;
                                for (int ji = 0; ji < od2->implements_count; ji++) {
                                    if (strcmp(od2->implements_names[ji], iface_name) == 0) {
                                        found = true; break;
                                    }
                                }
                                if (!found) { all_implement = false; break; }
                            }
                            if (all_implement) {
                                /* Found common interface — use it as elem type */
                                Iron_Symbol *isym = iron_scope_lookup(ctx->global_scope, iface_name);
                                if (isym && isym->type && isym->type->kind == IRON_TYPE_INTERFACE) {
                                    elem_type = isym->type;
                                }
                                break;
                            }
                        }
                    }
                }
            }
            arrfree(elem_types);
            if (!elem_type) {
                /* Empty literal with no expected-type context: emit a
                 * targeted diagnostic instead of silently producing
                 * [<error>] which causes a misleading downstream
                 * type-mismatch. Callers with an expected type should
                 * use check_expr_with_expected, which short-circuits
                 * this path entirely. */
                if (al->element_count == 0) {
                    emit_error(ctx, IRON_ERR_EMPTY_LITERAL_NO_TYPE, al->span,
                               "cannot infer element type of empty array literal; "
                               "add a type annotation like `var x: [T] = []`",
                               NULL);
                }
                elem_type = iron_type_make_primitive(IRON_TYPE_ERROR);
            }
            result = iron_type_make_array(ctx->arena, elem_type, -1, false);
            al->resolved_type = result;
            break;
        }

        case IRON_NODE_ENUM_CONSTRUCT: {
            Iron_EnumConstruct *ec = (Iron_EnumConstruct *)node;
            /* Look up enum type in global scope */
            Iron_Symbol *esym = iron_scope_lookup(ctx->global_scope, ec->enum_name);
            if (!esym || !esym->type || esym->type->kind != IRON_TYPE_ENUM) {
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
                break;
            }
            Iron_Type *enum_type = esym->type;
            Iron_EnumDecl *ed = enum_type->enu.decl;

            /* Generic enum: infer type args from argument types */
            if (ed->generic_param_count > 0) {
                int vi = find_variant_index(ed, ec->variant_name);
                if (vi < 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "enum '%s' has no variant '%s'",
                             ec->enum_name, ec->variant_name);
                    emit_error(ctx, IRON_ERR_UNKNOWN_VARIANT, ec->span, msg, NULL);
                    result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    break;
                }
                Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[vi];
                if (ec->arg_count != ev->payload_count) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "%s.%s expects %d argument(s) but got %d",
                             ec->enum_name, ec->variant_name, ev->payload_count, ec->arg_count);
                    emit_error(ctx, IRON_ERR_PATTERN_ARITY, ec->span, msg, NULL);
                    ec->resolved_type = enum_type;
                    result = enum_type;
                    break;
                }

                /* Infer type args from argument types */
                Iron_Type **inferred_args = iron_arena_alloc(ctx->arena,
                    sizeof(Iron_Type *) * (size_t)ed->generic_param_count,
                    _Alignof(Iron_Type *));
                if (!inferred_args) { /* HARD-09 REPLACE (typecheck.c:check_expr ENUM_CONSTRUCT inferred_args) */ return false; }
                memset(inferred_args, 0,
                    sizeof(Iron_Type *) * (size_t)ed->generic_param_count);

                /* Push generic param scope for resolving payload type annotations */
                Iron_Scope *saved_gen = ctx->global_scope;
                Iron_Scope *gen_scope = iron_scope_create(ctx->arena,
                    ctx->global_scope, IRON_SCOPE_BLOCK);
                for (int gi = 0; gi < ed->generic_param_count; gi++) {
                    /* PROT-03 row 16 (AUDIT-01 M-severity): assert kind on
                     * ed->generic_params[gi] before the Iron_Ident cast. */
                    if (ed->generic_params[gi])
                        IRON_NODE_ASSERT_KIND(ed->generic_params[gi], IRON_NODE_IDENT);
                    Iron_Ident *param = (Iron_Ident *)ed->generic_params[gi];
                    if (param) {
                        Iron_Type *gpt = iron_type_make_generic_param(
                            ctx->arena, param->name, NULL);
                        Iron_Symbol *gsym = iron_symbol_create(ctx->arena,
                            param->name, IRON_SYM_TYPE, NULL,
                            (Iron_Span){0, 0, 0, 0, 0});
                        gsym->type = gpt;
                        iron_scope_define(gen_scope, ctx->arena, gsym);
                    }
                }
                ctx->global_scope = gen_scope;

                /* Type-check args and infer generic type params */
                for (int j = 0; j < ec->arg_count; j++) {
                    Iron_Type *arg_t = check_expr(ctx, ec->args[j]);
                    Iron_Type *expected = resolve_type_annotation(
                        ctx, ev->payload_type_anns[j]);
                    /* If expected is GENERIC_PARAM, map it to arg_t */
                    if (expected && expected->kind == IRON_TYPE_GENERIC_PARAM) {
                        for (int gi = 0; gi < ed->generic_param_count; gi++) {
                            /* PROT-03 row 17 (AUDIT-01 M-severity): assert kind
                             * on ed->generic_params[gi] before the Iron_Ident cast. */
                            if (ed->generic_params[gi])
                                IRON_NODE_ASSERT_KIND(ed->generic_params[gi], IRON_NODE_IDENT);
                            Iron_Ident *param = (Iron_Ident *)ed->generic_params[gi];
                            if (param && expected->generic_param.name &&
                                strcmp(param->name, expected->generic_param.name) == 0) {
                                inferred_args[gi] = arg_t;
                                break;
                            }
                        }
                    }
                    /* If expected is a generic enum like Tree[T] and arg_t is the
                     * same enum with concrete type args like Tree[Int], infer T=Int.
                     * This handles recursive generic variants: Branch(Tree[T], Tree[T]). */
                    if (expected && expected->kind == IRON_TYPE_ENUM &&
                        arg_t && arg_t->kind == IRON_TYPE_ENUM &&
                        expected->enu.decl && expected->enu.decl == arg_t->enu.decl &&
                        expected->enu.type_arg_count > 0 &&
                        expected->enu.type_arg_count == arg_t->enu.type_arg_count) {
                        for (int ta = 0; ta < expected->enu.type_arg_count; ta++) {
                            Iron_Type *exp_ta = expected->enu.type_args
                                               ? expected->enu.type_args[ta] : NULL;
                            Iron_Type *arg_ta = arg_t->enu.type_args
                                               ? arg_t->enu.type_args[ta] : NULL;
                            if (exp_ta && exp_ta->kind == IRON_TYPE_GENERIC_PARAM && arg_ta) {
                                for (int gi = 0; gi < ed->generic_param_count; gi++) {
                                    /* PROT-03 row 18 (AUDIT-01 M-severity): assert kind
                                     * on ed->generic_params[gi] before the Iron_Ident cast. */
                                    if (ed->generic_params[gi])
                                        IRON_NODE_ASSERT_KIND(ed->generic_params[gi], IRON_NODE_IDENT);
                                    Iron_Ident *param = (Iron_Ident *)ed->generic_params[gi];
                                    if (param && exp_ta->generic_param.name &&
                                        strcmp(param->name, exp_ta->generic_param.name) == 0) {
                                        inferred_args[gi] = arg_ta;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
                ctx->global_scope = saved_gen;

                /* Build mangled name from inferred args.
                 * Use type_mangle_component to ensure C-identifier-safe names
                 * for nested generic types (e.g. Option[Int] -> "Option_Int"). */
                Iron_StrBuf sb = iron_strbuf_create(64);
                iron_strbuf_appendf(&sb, "Iron_%s", ed->name);
                for (int gi = 0; gi < ed->generic_param_count; gi++) {
                    if (inferred_args[gi]) {
                        iron_strbuf_appendf(&sb, "_%s",
                            type_mangle_component(inferred_args[gi], ctx->arena));
                    } else {
                        iron_strbuf_appendf(&sb, "_unknown");
                    }
                }
                const char *mangled = iron_arena_strdup(ctx->arena,
                    iron_strbuf_get(&sb), sb.len);
                if (!mangled) { /* HARD-09 REPLACE (typecheck.c:check_expr ENUM_CONSTRUCT mangled) */ return false; }
                iron_strbuf_free(&sb);

                /* Check mono_registry: if this mangled type was already built
                 * (e.g. from the function signature resolution), reuse it to
                 * ensure payload_is_boxed is correctly populated. */
                {
                    ptrdiff_t reg2_idx = shgeti(ctx->mono_registry, mangled);
                    if (reg2_idx >= 0) {
                        ec->resolved_type = ctx->mono_registry[reg2_idx].value;
                        result = ec->resolved_type;
                        break;
                    }
                }

                /* Create monomorphized type */
                Iron_Type *mono = iron_arena_alloc(ctx->arena, sizeof(Iron_Type),
                    _Alignof(Iron_Type));
                if (!mono) { /* HARD-09 REPLACE (typecheck.c:check_expr ENUM_CONSTRUCT mono) */ return false; }
                memset(mono, 0, sizeof(*mono));
                mono->kind = IRON_TYPE_ENUM;
                mono->enu.decl = ed;
                mono->enu.type_args = inferred_args;
                mono->enu.type_arg_count = ed->generic_param_count;
                mono->enu.mangled_name = mangled;

                /* Register in mono_registry before payload resolution (cycle detection). */
                const char *mono2_key = iron_arena_strdup(ctx->arena, mangled, strlen(mangled));
                if (!mono2_key) { /* HARD-09 REPLACE (typecheck.c:check_expr ENUM_CONSTRUCT mono2_key) */ return false; }
                shput(ctx->mono_registry, mono2_key, mono);

                /* Substitute variant_payload_types:
                 * Bind concrete inferred_args in gen_scope (not GENERIC_PARAMs)
                 * so recursive payload resolution sees Tree[Int] not Tree[T]. */
                Iron_Type ***vpt = iron_arena_alloc(ctx->arena,
                    sizeof(Iron_Type **) * (size_t)ed->variant_count,
                    _Alignof(Iron_Type **));
                if (!vpt) { /* HARD-09 REPLACE (typecheck.c:check_expr ENUM_CONSTRUCT vpt) */ return false; }
                memset(vpt, 0, sizeof(Iron_Type **) * (size_t)ed->variant_count);
                Iron_Scope *saved_gen2 = ctx->global_scope;
                Iron_Scope *gen2 = iron_scope_create(ctx->arena,
                    ctx->global_scope, IRON_SCOPE_BLOCK);
                for (int gi = 0; gi < ed->generic_param_count; gi++) {
                    /* PROT-03 unenumerated bonus (AUDIT-01 M-severity sibling
                     * of rows 16-18): assert kind on ed->generic_params[gi]
                     * before the Iron_Ident cast at the second gen-scope build
                     * for variant payload type substitution. */
                    if (ed->generic_params[gi])
                        IRON_NODE_ASSERT_KIND(ed->generic_params[gi], IRON_NODE_IDENT);
                    Iron_Ident *param = (Iron_Ident *)ed->generic_params[gi];
                    if (param) {
                        Iron_Symbol *gsym = iron_symbol_create(ctx->arena,
                            param->name, IRON_SYM_TYPE, NULL,
                            (Iron_Span){0, 0, 0, 0, 0});
                        gsym->type = (gi < ed->generic_param_count && inferred_args[gi])
                                     ? inferred_args[gi]
                                     : iron_type_make_generic_param(
                                           ctx->arena, param->name, NULL);
                        iron_scope_define(gen2, ctx->arena, gsym);
                    }
                }
                ctx->global_scope = gen2;
                for (int vj = 0; vj < ed->variant_count; vj++) {
                    Iron_EnumVariant *vev = (Iron_EnumVariant *)ed->variants[vj];
                    if (vev->payload_count == 0) { vpt[vj] = NULL; continue; }
                    Iron_Type **row = iron_arena_alloc(ctx->arena,
                        sizeof(Iron_Type *) * (size_t)vev->payload_count,
                        _Alignof(Iron_Type *));
                    if (!row) { /* HARD-09 REPLACE (typecheck.c:check_expr ENUM_CONSTRUCT vpt row) */ return false; }
                    for (int kk = 0; kk < vev->payload_count; kk++) {
                        /* T is bound to concrete inferred_args[i] in gen2 scope,
                         * so no post-substitution needed. */
                        row[kk] = resolve_type_annotation(ctx, vev->payload_type_anns[kk]);
                    }
                    vpt[vj] = row;
                }
                ctx->global_scope = saved_gen2;
                mono->enu.variant_payload_types = vpt;

                /* Compute payload_is_boxed for monomorphized type (path 2) */
                bool **pib2 = iron_arena_alloc(ctx->arena,
                    sizeof(bool *) * (size_t)ed->variant_count, _Alignof(bool *));
                if (!pib2) { /* HARD-09 REPLACE (typecheck.c:check_expr ENUM_CONSTRUCT pib2) */ return false; }
                memset(pib2, 0, sizeof(bool *) * (size_t)ed->variant_count);
                for (int vj2 = 0; vj2 < ed->variant_count; vj2++) {
                    Iron_EnumVariant *vev2 = (Iron_EnumVariant *)ed->variants[vj2];
                    if (vev2->payload_count == 0) continue;
                    bool *pib2_row = iron_arena_alloc(ctx->arena,
                        sizeof(bool) * (size_t)vev2->payload_count, _Alignof(bool));
                    if (!pib2_row) { /* HARD-09 REPLACE (typecheck.c:check_expr ENUM_CONSTRUCT pib2 row) */ return false; }
                    memset(pib2_row, 0, sizeof(bool) * (size_t)vev2->payload_count);
                    for (int kk2 = 0; kk2 < vev2->payload_count; kk2++) {
                        if (vpt[vj2] && vpt[vj2][kk2]) {
                            pib2_row[kk2] = iron_type_equals(vpt[vj2][kk2], mono);
                        }
                    }
                    pib2[vj2] = pib2_row;
                }
                mono->enu.payload_is_boxed = pib2;

                ec->resolved_type = mono;
                result = mono;
                break;
            }

            /* Non-generic enum: standard handling */
            int vi = find_variant_index(ed, ec->variant_name);
            if (vi < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "enum '%s' has no variant '%s'",
                         ec->enum_name, ec->variant_name);
                emit_error(ctx, IRON_ERR_UNKNOWN_VARIANT, ec->span, msg, NULL);
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
                break;
            }
            Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[vi];
            /* Check argument count matches payload count */
            if (ec->arg_count != ev->payload_count) {
                char msg[256];
                snprintf(msg, sizeof(msg), "%s.%s expects %d argument(s) but got %d",
                         ec->enum_name, ec->variant_name, ev->payload_count, ec->arg_count);
                emit_error(ctx, IRON_ERR_PATTERN_ARITY, ec->span, msg, NULL);
                ec->resolved_type = enum_type;
                result = enum_type;
                break;
            }
            /* Type-check each argument against variant payload types */
            Iron_Type **ptypes = enum_type->enu.variant_payload_types
                                 ? enum_type->enu.variant_payload_types[vi] : NULL;
            for (int j = 0; j < ec->arg_count; j++) {
                Iron_Type *arg_t = check_expr(ctx, ec->args[j]);
                if (ptypes && ptypes[j] && arg_t &&
                    arg_t->kind != IRON_TYPE_ERROR && ptypes[j]->kind != IRON_TYPE_ERROR) {
                    if (!iron_type_equals(arg_t, ptypes[j])) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "argument %d to %s.%s: expected %s but got %s",
                                 j + 1, ec->enum_name, ec->variant_name,
                                 iron_type_to_string(ptypes[j], ctx->arena),
                                 iron_type_to_string(arg_t, ctx->arena));
                        emit_error(ctx, IRON_ERR_ARG_TYPE, ec->span, msg, NULL);
                    }
                }
            }
            ec->resolved_type = enum_type;
            result = enum_type;
            break;
        }

        /* HARD-04: graceful poison return on parser ErrorNode — downstream
         * passes see IRON_TYPE_ERROR and propagate it silently. */
        case IRON_NODE_ERROR:
            result = iron_type_make_primitive(IRON_TYPE_ERROR);
            break;

        /* -Wswitch-enum opt-out: check_expr only handles expression node kinds;
         * statement/declaration kinds reach this arm if an upstream caller
         * passes them in by mistake and get an IRON_TYPE_ERROR result that
         * surfaces as a type-mismatch downstream. */
        default:
            result = iron_type_make_primitive(IRON_TYPE_ERROR);
            break;
    }

    if (!result) result = iron_type_make_primitive(IRON_TYPE_ERROR);
    return result;
}

/* Wrapper around check_expr that threads an expected type into the
 * empty array literal inference path. For an empty literal `[]` with
 * an expected `[T]` context, the literal's resolved_type is set to the
 * expected type directly (bypassing the element-loop that would produce
 * IRON_TYPE_ERROR because there are no elements to infer from).
 *
 * All other cases delegate to plain check_expr. Callers that pass
 * expected == NULL get identical behavior to check_expr.
 *
 * Used by: var decl (vd->init), call arg (ce->args[i]), return (rs->value),
 * assignment (as->value). Other check_expr callers remain unchanged.
 */
static Iron_Type *check_expr_with_expected(TypeCtx *ctx, Iron_Node *node,
                                            Iron_Type *expected) {
    /* HARD-05: cancel poll at expected-type walker entry. */
    if (iron_cancel_requested(ctx->cancel_flag)) {
        return iron_type_make_primitive(IRON_TYPE_VOID);
    }
    if (node && node->kind == IRON_NODE_ARRAY_LIT && expected &&
        expected->kind == IRON_TYPE_ARRAY) {
        Iron_ArrayLit *al = (Iron_ArrayLit *)node;
        if (al->element_count == 0) {
            al->resolved_type = expected;
            return expected;
        }
    }
    return check_expr(ctx, node);
}

/* ── Statement type checking ─────────────────────────────────────────────── */

static void check_stmt(TypeCtx *ctx, Iron_Node *node) {
    if (!node) return;
    /* HARD-05: cancel poll at recursive statement walker entry. */
    if (iron_cancel_requested(ctx->cancel_flag)) return;
    switch ((int)(node->kind)) {
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            tc_push_scope(ctx, IRON_SCOPE_BLOCK);
            check_block_stmts(ctx, b->stmts, b->stmt_count);
            tc_pop_scope(ctx);
            break;
        }

        /* Phase 4 Plan 04-01 (EDIT-07) note: VAL_DECL / VAR_DECL use the
         * literal-narrowing emit_type_mismatch_maybe_literal helper below
         * to narrow literal-position mismatches to IRON_ERR_TYPE_MISMATCH_LITERAL
         * (code 235) with retyped-literal .suggestion. See helper definition
         * above emit_type_mismatch. */
        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;

            /* Phase 59 01d: tuple destructure — val (a, b, ...) = expr */
            if (vd->binding_count > 0) {
                Iron_Type *init_type = vd->init
                    ? check_expr(ctx, vd->init)
                    : iron_type_make_primitive(IRON_TYPE_ERROR);
                if (!init_type || init_type->kind == IRON_TYPE_ERROR) {
                    /* Bind every target to ERROR so downstream uses don't crash */
                    Iron_Type *err_ty = iron_type_make_primitive(IRON_TYPE_ERROR);
                    for (int i = 0; i < vd->binding_count; i++) {
                        if (vd->binding_names[i]) {
                            tc_define(ctx, vd->binding_names[i],
                                      IRON_SYM_VARIABLE, (Iron_Node *)vd,
                                      vd->span, false, err_ty);
                        }
                    }
                    vd->declared_type = err_ty;
                    break;
                }
                if (init_type->kind != IRON_TYPE_TUPLE) {
                    emit_error(ctx, IRON_ERR_TYPE_MISMATCH, vd->span,
                               "tuple destructure requires a tuple-typed initializer", NULL);
                    Iron_Type *err_ty = iron_type_make_primitive(IRON_TYPE_ERROR);
                    for (int i = 0; i < vd->binding_count; i++) {
                        if (vd->binding_names[i]) {
                            tc_define(ctx, vd->binding_names[i],
                                      IRON_SYM_VARIABLE, (Iron_Node *)vd,
                                      vd->span, false, err_ty);
                        }
                    }
                    vd->declared_type = err_ty;
                    break;
                }
                if (init_type->tuple.elem_count != vd->binding_count) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "tuple destructure expects %d binding(s) but found %d",
                             init_type->tuple.elem_count, vd->binding_count);
                    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
                    if (!msg_copy) { /* HARD-09 REPLACE (typecheck.c:check_stmt VAL_DECL tuple-mismatch msg) */ msg_copy = "analyzer error"; }
                    emit_error(ctx, IRON_ERR_TYPE_MISMATCH, vd->span, msg_copy, NULL);
                }
                /* Bind each name to its element type (skip wildcards). */
                int defined_count = vd->binding_count < init_type->tuple.elem_count
                    ? vd->binding_count
                    : init_type->tuple.elem_count;
                for (int i = 0; i < defined_count; i++) {
                    if (!vd->binding_names[i]) continue;  /* wildcard */
                    tc_define(ctx, vd->binding_names[i], IRON_SYM_VARIABLE,
                              (Iron_Node *)vd, vd->span, false,
                              init_type->tuple.elem_types[i]);
                }
                vd->declared_type = init_type;
                break;
            }

            Iron_Type *decl_type = NULL;

            if (vd->type_ann) {
                decl_type = resolve_type_annotation(ctx, vd->type_ann);
            }

            Iron_Type *init_type = NULL;
            if (vd->init) {
                if (vd->init->kind == IRON_NODE_SPAWN) {
                    /* val h = spawn(...) { body } -- spawn as handle init */
                    check_stmt(ctx, vd->init);  /* processes the spawn node (handle_name already set) */
                    /* The declared type for h is OBJECT (an Iron_Handle pointer) */
                    init_type = iron_type_make_primitive(IRON_TYPE_OBJECT);
                } else {
                    init_type = check_expr(ctx, vd->init);
                }
            }

            if (!decl_type && init_type) {
                decl_type = init_type;
            } else if (decl_type && init_type) {
                /* Context-directed generic enum completion: if the construct has
                 * unresolved type args, fill them in from the declared type. */
                maybe_fill_missing_generic_args(vd->init, decl_type);
                init_type = vd->init ? (vd->init->kind == IRON_NODE_ENUM_CONSTRUCT
                    ? ((Iron_EnumConstruct *)vd->init)->resolved_type : init_type)
                    : init_type;

                /* Phase 25 PTR-05/UNCK-04 (Plan 25-01): `&` cannot produce
                 * *unchecked T. If the declared type is `*unchecked T` AND the
                 * rhs is a unary `&` expression, emit E0294 before
                 * types_assignable runs (which would emit the generic E0289).
                 * Only Box.unwrap() or RawPtr (Phase 33) can produce *unchecked T.
                 * PHASE-26 HOOK: rc Box[T] interaction — rc + nocopy may be
                 * incompatible; Phase 26 decides. */
                if (decl_type && decl_type->kind == IRON_TYPE_PTR &&
                    decl_type->ptr.is_unchecked &&
                    vd->init && vd->init->kind == IRON_NODE_UNARY &&
                    ((Iron_UnaryExpr *)vd->init)->op == (Iron_OpKind)IRON_TOK_AMP) {
                    emit_error(ctx, IRON_ERR_PTR_AMP_NOT_UNCHECKED, vd->init->span,
                               "cannot produce '*unchecked T' via '&'; "
                               "'&' always yields a checked pointer",
                               "§4.3: '&' cannot produce unchecked pointers; "
                               "use Box.unwrap() or RawPtr (Phase 33) "
                               "for explicit unchecked pointer construction");
                }

                if (init_type->kind != IRON_TYPE_ERROR &&
                    decl_type->kind != IRON_TYPE_ERROR &&
                    !types_assignable(decl_type, init_type) &&
                    !is_int_literal_narrowing(decl_type, init_type, vd->init)) {
                    /* Phase 20 PTR-13: null literal assigned to non-nullable
                     * pointer type. Emit IRON_ERR_PTR_NULL_DEREF=272 with the
                     * spec-locked substring "non-nullable pointer" and a hint
                     * pointing to the `?*T` nullable variant. The check for
                     * IRON_TYPE_NULLABLE inner=PTR is intentionally absent on
                     * decl_type because that path goes through types_assignable
                     * cleanly via NULL-handling on nullables. */
                    Iron_Span lit_span = (vd->init) ? vd->init->span : vd->span;
                    if (decl_type->kind == IRON_TYPE_PTR &&
                        init_type->kind == IRON_TYPE_PTR &&
                        decl_type->ptr.is_unchecked != init_type->ptr.is_unchecked) {
                        /* Phase 25 PTR-02/03 (Plan 25-01): cross-regime pointer assignment.
                         * Emit E0289 IRON_ERR_PTR_REGIME_MISMATCH with §4.3-§4.4 spec hint.
                         * Specificity over E0202: programmer needs to know it's a regime
                         * crossing, not a T mismatch (RESEARCH Specifics). */
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "cannot assign '%s' to '%s': "
                                 "checked and unchecked pointer regimes are disjoint",
                                 iron_type_to_string(init_type, ctx->arena),
                                 iron_type_to_string(decl_type, ctx->arena));
                        emit_error(ctx, IRON_ERR_PTR_REGIME_MISMATCH, lit_span,
                                   msg,
                                   "§4.3-§4.4: checked and unchecked pointer regimes are disjoint; "
                                   "use Box.unwrap() to escape from Box[T] to *unchecked T");
                    } else if (decl_type->kind == IRON_TYPE_PTR &&
                        init_type->kind == IRON_TYPE_NULL) {
                        const char *pt_str = iron_type_to_string(decl_type, ctx->arena);
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "cannot assign null to non-nullable pointer "
                                 "type '%s'", pt_str ? pt_str : "*T");
                        emit_error(ctx, IRON_ERR_PTR_NULL_DEREF, lit_span,
                                   msg,
                                   "use '?*T' for the nullable pointer variant");
                    } else if (decl_type->kind == IRON_TYPE_ARRAY && init_type->kind == IRON_TYPE_ARRAY &&
                               decl_type->array.size >= 0 && init_type->array.size >= 0 &&
                               decl_type->array.is_bounded != init_type->array.is_bounded) {
                        /* Phase 23 VEC-283: specialize bounded<->strict cross-assign.
                         * §3.3: [T; <=N] and [T; N] are disjoint types. */
                        emit_error(ctx, IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN, lit_span,
                                   "cannot assign bounded vector to strict array or vice versa",
                                   "§3.3: [T; <=N] and [T; N] are disjoint types; "
                                   "Phase 33 ships to_fixed()/to_bounded() conversion helpers");
                    } else if (decl_type->kind == IRON_TYPE_ARRAY &&
                               decl_type->array.size >= 0 && !decl_type->array.is_bounded &&
                               vd->init && vd->init->kind == IRON_NODE_ARRAY_LIT) {
                        /* Phase 23 VEC-04: strict array declared with literal.
                         * The literal's inferred type is [T] (dynamic, size=-1) so
                         * types_assignable rejected it. Two sub-cases:
                         *   - element count matches size → valid; suppress E0202.
                         *   - element count differs → emit VEC-04 specialization (E0282).
                         * §3.3: [T; N] requires exactly N elements in the initializer. */
                        Iron_ArrayLit *al = (Iron_ArrayLit *)vd->init;
                        if (al->element_count != decl_type->array.size) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "array literal has %d element(s) but '[%s; %d]' requires exactly %d",
                                     al->element_count,
                                     decl_type->array.elem
                                         ? iron_type_to_string(decl_type->array.elem, ctx->arena) : "?",
                                     decl_type->array.size, decl_type->array.size);
                            const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
                            if (!msg_copy) msg_copy = "array literal element count mismatch";
                            emit_error(ctx, IRON_ERR_VEC_STRICT_LENGTH_MISMATCH,
                                       vd->init->span, msg_copy,
                                       "§3.3: [T; N] requires exactly N elements in the initializer literal");
                        }
                        /* If count matches, suppress the generic E0202: the literal is valid. */
                    } else {
                    /* Phase 4 Plan 04-01 (EDIT-07): narrow literal RHS to
                     * IRON_ERR_TYPE_MISMATCH_LITERAL=235 with retyped-literal
                     * .suggestion. Non-literal RHS stays at 202 with NULL.
                     *
                     * Phase 5 Plan 05-05 (D-07 fmt-clean gate): pass the
                     * literal RHS's span (not the whole decl's span) so
                     * the quickfix replaces only the literal text --
                     * the previous whole-decl span caused the quickfix
                     * to destroy the `val n: Int =` prefix. The 202
                     * general-form emit still uses vd->span below. */
                    emit_type_mismatch_maybe_literal(ctx, lit_span, decl_type,
                                                      init_type, vd->init);
                    }
                }
                /* Narrow literal type to match declaration (e.g., Int literal -> Int32) */
                if (is_int_literal_narrowing(decl_type, init_type, vd->init)) {
                    ((Iron_IntLit *)vd->init)->resolved_type = decl_type;
                }
            }

            /* Phase 24 DROP-08 (Plan 24-02): nocopy type assigned by value — E0286.
             * Site (a): IRON_NODE_VAL_DECL. Only fires when the init is an IDENT
             * (copying from an existing binding). Constructors, call-returns, and
             * heap-allocs are moves/initializations — not copies — so they are safe
             * for nocopy types. */
            if (init_type && init_type->kind == IRON_TYPE_OBJECT &&
                init_type->object.decl && init_type->object.decl->is_nocopy &&
                (!decl_type || decl_type->kind == IRON_TYPE_OBJECT) &&
                vd->init && vd->init->kind == IRON_NODE_IDENT) {
                emit_error(ctx, IRON_ERR_COPY_OF_NOCOPY_TYPE, vd->span,
                           "cannot copy nocopy type — assignment requires a copy operation",
                           "§7: nocopy types cannot be copied; pass `*T` or `*var T` to avoid copy");
            }

            vd->declared_type = decl_type;

            /* Define symbol in type-checker scope (immutable) */
            tc_define(ctx, vd->name, IRON_SYM_VARIABLE, (Iron_Node *)vd, vd->span,
                      false, decl_type);
            break;
        }

        case IRON_NODE_VAR_DECL: {
            Iron_VarDecl *vd = (Iron_VarDecl *)node;
            Iron_Type *decl_type = NULL;

            if (vd->type_ann) {
                decl_type = resolve_type_annotation(ctx, vd->type_ann);
            }

            Iron_Type *init_type = NULL;
            if (vd->init) {
                if (vd->init->kind == IRON_NODE_SPAWN) {
                    /* var h = spawn(...) { body } -- spawn as handle init */
                    check_stmt(ctx, vd->init);
                    init_type = iron_type_make_primitive(IRON_TYPE_OBJECT);
                } else {
                    init_type = check_expr_with_expected(ctx, vd->init, decl_type);
                }
            }

            if (!decl_type && init_type) {
                decl_type = init_type;
            } else if (decl_type && init_type) {
                /* Context-directed generic enum completion: if the construct has
                 * unresolved type args, fill them in from the declared type. */
                maybe_fill_missing_generic_args(vd->init, decl_type);
                init_type = vd->init ? (vd->init->kind == IRON_NODE_ENUM_CONSTRUCT
                    ? ((Iron_EnumConstruct *)vd->init)->resolved_type : init_type)
                    : init_type;

                /* Phase 25 PTR-05/UNCK-04 (Plan 25-01): `&` cannot produce
                 * *unchecked T at a var declaration site. PHASE-26 HOOK: rc
                 * Box[T] interaction — rc + nocopy may be incompatible; Phase 26
                 * decides. */
                if (decl_type && decl_type->kind == IRON_TYPE_PTR &&
                    decl_type->ptr.is_unchecked &&
                    vd->init && vd->init->kind == IRON_NODE_UNARY &&
                    ((Iron_UnaryExpr *)vd->init)->op == (Iron_OpKind)IRON_TOK_AMP) {
                    emit_error(ctx, IRON_ERR_PTR_AMP_NOT_UNCHECKED, vd->init->span,
                               "cannot produce '*unchecked T' via '&'; "
                               "'&' always yields a checked pointer",
                               "§4.3: '&' cannot produce unchecked pointers; "
                               "use Box.unwrap() or RawPtr (Phase 33) "
                               "for explicit unchecked pointer construction");
                }

                if (init_type->kind != IRON_TYPE_ERROR &&
                    decl_type->kind != IRON_TYPE_ERROR &&
                    !types_assignable(decl_type, init_type) &&
                    !is_int_literal_narrowing(decl_type, init_type, vd->init)) {
                    /* Phase 20 PTR-13: null literal assigned to non-nullable
                     * pointer type. Emit IRON_ERR_PTR_NULL_DEREF=272 with the
                     * spec-locked substring "non-nullable pointer" and a hint
                     * pointing to the `?*T` nullable variant. The check for
                     * IRON_TYPE_NULLABLE inner=PTR is intentionally absent on
                     * decl_type because that path goes through types_assignable
                     * cleanly via NULL-handling on nullables. */
                    Iron_Span lit_span = (vd->init) ? vd->init->span : vd->span;
                    if (decl_type->kind == IRON_TYPE_PTR &&
                        init_type->kind == IRON_TYPE_PTR &&
                        decl_type->ptr.is_unchecked != init_type->ptr.is_unchecked) {
                        /* Phase 25 PTR-02/03 (Plan 25-01): cross-regime assignment at
                         * var declaration. E0289 IRON_ERR_PTR_REGIME_MISMATCH. */
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "cannot assign '%s' to '%s': "
                                 "checked and unchecked pointer regimes are disjoint",
                                 iron_type_to_string(init_type, ctx->arena),
                                 iron_type_to_string(decl_type, ctx->arena));
                        emit_error(ctx, IRON_ERR_PTR_REGIME_MISMATCH, lit_span,
                                   msg,
                                   "§4.3-§4.4: checked and unchecked pointer regimes are disjoint; "
                                   "use Box.unwrap() to escape from Box[T] to *unchecked T");
                    } else if (decl_type->kind == IRON_TYPE_ARRAY && init_type->kind == IRON_TYPE_ARRAY &&
                        decl_type->array.size >= 0 && init_type->array.size >= 0 &&
                        decl_type->array.is_bounded != init_type->array.is_bounded) {
                        /* Phase 23 VEC-283: specialize bounded<->strict cross-assign.
                         * §3.3: [T; <=N] and [T; N] are disjoint types. */
                        emit_error(ctx, IRON_ERR_VEC_BOUNDED_TO_FIXED_FORBIDDEN, lit_span,
                                   "cannot assign bounded vector to strict array or vice versa",
                                   "§3.3: [T; <=N] and [T; N] are disjoint types; "
                                   "Phase 33 ships to_fixed()/to_bounded() conversion helpers");
                    } else if (decl_type->kind == IRON_TYPE_ARRAY &&
                               decl_type->array.size >= 0 && !decl_type->array.is_bounded &&
                               vd->init && vd->init->kind == IRON_NODE_ARRAY_LIT) {
                        /* Phase 23 VEC-04: strict array declared with literal.
                         * The literal's inferred type is [T] (dynamic, size=-1) so
                         * types_assignable rejected it. Two sub-cases:
                         *   - element count matches size → valid; suppress E0202.
                         *   - element count differs → emit VEC-04 specialization (E0282).
                         * §3.3: [T; N] requires exactly N elements in the initializer. */
                        Iron_ArrayLit *al = (Iron_ArrayLit *)vd->init;
                        if (al->element_count != decl_type->array.size) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "array literal has %d element(s) but '[%s; %d]' requires exactly %d",
                                     al->element_count,
                                     decl_type->array.elem
                                         ? iron_type_to_string(decl_type->array.elem, ctx->arena) : "?",
                                     decl_type->array.size, decl_type->array.size);
                            const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
                            if (!msg_copy) msg_copy = "array literal element count mismatch";
                            emit_error(ctx, IRON_ERR_VEC_STRICT_LENGTH_MISMATCH,
                                       vd->init->span, msg_copy,
                                       "§3.3: [T; N] requires exactly N elements in the initializer literal");
                        }
                        /* If count matches, suppress the generic E0202: the literal is valid. */
                    } else if (decl_type->kind == IRON_TYPE_PTR &&
                        init_type->kind == IRON_TYPE_NULL) {
                        const char *pt_str = iron_type_to_string(decl_type, ctx->arena);
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "cannot assign null to non-nullable pointer "
                                 "type '%s'", pt_str ? pt_str : "*T");
                        emit_error(ctx, IRON_ERR_PTR_NULL_DEREF, lit_span,
                                   msg,
                                   "use '?*T' for the nullable pointer variant");
                    } else {
                    /* Phase 4 Plan 04-01 (EDIT-07): narrow literal RHS to
                     * IRON_ERR_TYPE_MISMATCH_LITERAL=235 with retyped-literal
                     * .suggestion. Non-literal RHS stays at 202 with NULL.
                     *
                     * Phase 5 Plan 05-05 (D-07 fmt-clean gate): pass the
                     * literal RHS's span (not the whole decl's span) so
                     * the quickfix replaces only the literal text --
                     * the previous whole-decl span caused the quickfix
                     * to destroy the `val n: Int =` prefix. The 202
                     * general-form emit still uses vd->span below. */
                    emit_type_mismatch_maybe_literal(ctx, lit_span, decl_type,
                                                      init_type, vd->init);
                    }
                }
                /* Narrow literal type to match declaration (e.g., Int literal -> Int32) */
                if (is_int_literal_narrowing(decl_type, init_type, vd->init)) {
                    ((Iron_IntLit *)vd->init)->resolved_type = decl_type;
                }
            }

            /* Phase 24 DROP-08 (Plan 24-02): nocopy type assigned by value — E0286.
             * Site (a): IRON_NODE_VAR_DECL. Only fires when the init is an IDENT
             * (copy from an existing binding). Constructors and call-returns are moves. */
            if (init_type && init_type->kind == IRON_TYPE_OBJECT &&
                init_type->object.decl && init_type->object.decl->is_nocopy &&
                (!decl_type || decl_type->kind == IRON_TYPE_OBJECT) &&
                vd->init && vd->init->kind == IRON_NODE_IDENT) {
                emit_error(ctx, IRON_ERR_COPY_OF_NOCOPY_TYPE, vd->span,
                           "cannot copy nocopy type — assignment requires a copy operation",
                           "§7: nocopy types cannot be copied; pass `*T` or `*var T` to avoid copy");
            }

            vd->declared_type = decl_type;

            /* Define symbol in type-checker scope (mutable) */
            tc_define(ctx, vd->name, IRON_SYM_VARIABLE, (Iron_Node *)vd, vd->span,
                      true, decl_type);
            break;
        }

        case IRON_NODE_ASSIGN: {
            Iron_AssignStmt *as = (Iron_AssignStmt *)node;

            /* Mutability check: use resolved_sym (set by resolver) as authoritative
             * source of is_mutable. Also check type-checker scope as fallback. */
            bool is_immutable = false;
            const char *target_name = NULL;
            Iron_Symbol *target_sym = NULL;  /* Phase 18 PARM-01: captured for sym_kind branch */

            if (as->target && as->target->kind == IRON_NODE_IDENT) {
                Iron_Ident *tid = (Iron_Ident *)as->target;
                target_name = tid->name;

                /* Check type-checker scope first */
                Iron_Symbol *tc_sym = tc_lookup(ctx, target_name);
                if (tc_sym) {
                    is_immutable = !tc_sym->is_mutable;
                    target_sym = tc_sym;
                } else if (tid->resolved_sym) {
                    /* Fall back to resolver's symbol */
                    is_immutable = !tid->resolved_sym->is_mutable;
                    target_sym = tid->resolved_sym;
                }

                /* Phase 84 MUTTIER-03: pure-tier write-to-ident diagnostics.
                 * Distinguish two cases:
                 *   E0241 — target is a mutable top-level `var` (global).
                 *   E0243 — target is a function/method parameter (excluding
                 *           the implicit `self`, which is handled by the
                 *           self.field path in the FIELD_ACCESS branch below).
                 * Globals resolve against ctx->global_scope; params are
                 * IRON_SYM_PARAM in the function scope. Both checks run only
                 * when enclosing method is pure. */
                if (ctx->in_pure_method) {
                    Iron_Symbol *global_sym = ctx->global_scope
                        ? iron_scope_lookup_local(ctx->global_scope, target_name)
                        : NULL;
                    if (global_sym &&
                        global_sym->sym_kind == IRON_SYM_VARIABLE &&
                        global_sym->is_mutable) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "cannot write mutable global '%s' in pure method",
                                 target_name);
                        emit_error(ctx, IRON_ERR_PURE_MUTABLE_GLOBAL,
                                   as->span, msg, NULL);
                    } else if (tc_sym && tc_sym->sym_kind == IRON_SYM_PARAM) {
                        /* `self` is IRON_SYM_VARIABLE in classic form and a
                         * param in receiver form; exclude the ident named
                         * "self" so the self.field path owns that diagnostic. */
                        if (strcmp(target_name, "self") != 0) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "cannot write parameter '%s' in pure method",
                                     target_name);
                            emit_error(ctx, IRON_ERR_PURE_PARAM_WRITE,
                                       as->span, msg, NULL);
                        }
                    }
                }
                /* Phase 22 READ-02: readonly method assigning to any parameter.
                 * Mirrors IRON_ERR_PURE_PARAM_WRITE but for the readonly tier.
                 * Guard: Pitfall 1 — !ctx->in_pure_method prevents double-emit when
                 * the enclosing method is pure (in_readonly_method is true for both). */
                if (ctx->in_readonly_method && !ctx->in_pure_method &&
                    tc_sym && tc_sym->sym_kind == IRON_SYM_PARAM &&
                    target_name && strcmp(target_name, "self") != 0) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "cannot assign to parameter '%s' in readonly method",
                             target_name);
                    emit_error(ctx, IRON_ERR_READONLY_PARAM_MUTATION,
                               as->span, msg,
                               "§6: readonly methods may not assign to any parameter");
                }
            }

            /* Phase 80 MUT-03: field-assignment on immutable receiver.
             * When the assignment target is a field-access (or chain of field-accesses
             * like t.inner.field), walk to the root ident and check its resolved_sym's
             * is_mutable flag. If the root is immutable (val-bound, or an immutable
             * receiver binding from Phase 80-01's resolver wiring), reject with E0234.
             *
             * Chain walking: t.inner.field → root ident is `t` (the innermost object
             * that is not itself a field_access). Iron_FieldAccess.object can be
             * another IRON_NODE_FIELD_ACCESS or an IRON_NODE_IDENT (or other expr
             * kinds like method calls — we only fire when the walk terminates at
             * an ident with a resolved_sym; otherwise the broader type system
             * handles it). */
            /* Phase 85 INIT-05: mark the assign target while it's being
             * checked so the FIELD_ACCESS handler can suppress the E0246
             * read-before-assign check on the immediate target node. */
            Iron_Node *prev_assign_target = ctx->cur_assign_target;
            ctx->cur_assign_target = as->target;
            Iron_Type *target_type = check_expr(ctx, as->target);
            ctx->cur_assign_target = prev_assign_target;
            Iron_Type *value_type  = check_expr_with_expected(ctx, as->value, target_type);

            bool is_field_target_immut = false;
            const char *field_root_name = NULL;
            Iron_Symbol *field_root_sym = NULL;  /* Phase 18 PARM-01: captured for sym_kind branch */
            /* Phase 20 OQ-A (Plan 20-02a): when the LHS is a field-access on
             * a `*var T` receiver, the binding's own mutability does NOT
             * gate the write — the pointer's `var` modifier authorizes it.
             * Detect this by inspecting the outermost field-access object's
             * resolved_type (populated by check_expr above): IRON_TYPE_PTR
             * with is_var=true means OQ-A applies, suppressing the
             * immutability gate. The IRON_NODE_FIELD_ACCESS handler
             * already set is_auto_deref on the outermost FA when this
             * condition holds (Phase 20 PTR-06 read side). */
            bool lhs_is_var_ptr_auto_deref = false;
            if (as->target && as->target->kind == IRON_NODE_FIELD_ACCESS) {
                Iron_FieldAccess *outer_fa = (Iron_FieldAccess *)as->target;
                if (outer_fa->object) {
                    Iron_Type *recv_t =
                        ((Iron_ExprNode *)outer_fa->object)->resolved_type;
                    if (recv_t && recv_t->kind == IRON_TYPE_PTR &&
                        recv_t->ptr.is_var) {
                        lhs_is_var_ptr_auto_deref = true;
                    }
                }
            }
            if (as->target && as->target->kind == IRON_NODE_FIELD_ACCESS) {
                Iron_Node *cur = as->target;
                while (cur && cur->kind == IRON_NODE_FIELD_ACCESS) {
                    cur = ((Iron_FieldAccess *)cur)->object;
                }
                if (cur && cur->kind == IRON_NODE_IDENT) {
                    Iron_Ident *root_id = (Iron_Ident *)cur;
                    if (root_id->resolved_sym) {
                        /* OQ-A lock: skip the immutability gate when the
                         * write goes through a *var T pointer. */
                        is_field_target_immut =
                            !root_id->resolved_sym->is_mutable &&
                            !lhs_is_var_ptr_auto_deref;
                        field_root_name = root_id->name;
                        field_root_sym = root_id->resolved_sym;
                    }
                }
            }

            if (is_immutable) {
                /* Phase 18 PARM-01: when the immutable target is a function
                 * parameter (read-only by default per spec §5.3), redirect
                 * to IRON_ERR_PARM_READ_ONLY=266 with a hint that mentions
                 * the 'var' modifier (Phase 34 LSP-06 quickfix-target).
                 * The else branch preserves the pre-Phase-18 path for
                 * val-local rebinds. Mutually exclusive emit — Pitfall 2
                 * lock asserts COUNT(266)==1 AND COUNT(203)==0 on the
                 * canonical PARM-01 case. */
                if (target_sym && target_sym->sym_kind == IRON_SYM_PARAM) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "cannot mutate read-only parameter '%s'",
                             target_name ? target_name : "");
                    emit_error(ctx, IRON_ERR_PARM_READ_ONLY, as->span, msg,
                               "add 'var' modifier to grant in-body mutation: 'var <name>: T'");
                } else {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "cannot assign to val '%s' — val is immutable",
                             target_name ? target_name : "");
                    emit_error(ctx, IRON_ERR_VAL_REASSIGN, as->span, msg, NULL);
                }
            }

            if (is_field_target_immut) {
                /* Phase 18 PARM-01: when the immutable receiver is a
                 * function parameter (rooted at IRON_SYM_PARAM), redirect
                 * to IRON_ERR_PARM_READ_ONLY=266. The else branch preserves
                 * the pre-Phase-18 generic immutable-receiver path
                 * (E0234) for val-local field writes. Mutually exclusive
                 * emit. */
                if (field_root_sym && field_root_sym->sym_kind == IRON_SYM_PARAM) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "cannot mutate read-only parameter '%s'",
                             field_root_name ? field_root_name : "");
                    emit_error(ctx, IRON_ERR_PARM_READ_ONLY, as->span, msg,
                               "add 'var' modifier to grant in-body mutation: 'var <name>: T'");
                } else {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "cannot mutate field on immutable receiver");
                    emit_error(ctx, IRON_ERR_MUT_FIELD_IMMUT_RECV, as->span, msg, NULL);
                    (void)field_root_name;  /* reserved for future hint; silence unused warn */
                }
            }

            /* Phase 84 MUTTIER-02/03: readonly/pure method writing self.field
             * is rejected with E0238 (readonly) or E0244 (pure). Pure gets a
             * distinct code for clearer diagnostic messaging; the branch
             * structure is a single check (ctx->in_readonly_method is true
             * when the caller is readonly OR pure — pure implies readonly
             * for self-writes).
             *
             * Suppressed inside synth accessor bodies so the synth setter
             * `self.field = _v` still lowers to a direct store (belt-and-
             * suspenders: synth setters are not is_readonly/is_pure so this
             * guard is secondary, but keeps the model consistent with the
             * pub-dispatch rewrite below).
             *
             * The target-chain walk mirrors the Phase 80 MUT-03 block above:
             * walk through IRON_NODE_FIELD_ACCESS links to the root ident
             * and require its name == "self". Chained writes like
             * `self.inner.field = v` fire here too — every link in the
             * chain bottoms out at `self`. */
            if (!ctx->in_synth_accessor &&
                ctx->in_readonly_method &&
                as->target && as->target->kind == IRON_NODE_FIELD_ACCESS) {
                Iron_Node *cur = as->target;
                while (cur && cur->kind == IRON_NODE_FIELD_ACCESS) {
                    cur = ((Iron_FieldAccess *)cur)->object;
                }
                if (cur && cur->kind == IRON_NODE_IDENT) {
                    Iron_Ident *root_id = (Iron_Ident *)cur;
                    if (root_id->name && strcmp(root_id->name, "self") == 0) {
                        int code = ctx->in_pure_method
                            ? IRON_ERR_PURE_WRITE_SELF
                            : IRON_ERR_READONLY_WRITE_SELF;
                        const char *tier = ctx->in_pure_method ? "pure" : "readonly";
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "cannot write self.field in %s method", tier);
                        const char *hint_str = ctx->in_pure_method
                            ? NULL
                            : "§6: readonly methods may not assign to self or its fields";
                        emit_error(ctx, code, as->span, msg, hint_str);
                    }
                }
            }

            /* Phase 85 INIT-12 E0248 + definite-assignment state update.
             * Inside an init body, a write to self.<field>:
             *   1. Emits E0248 if the target is a val field that has already
             *      been assigned (field was removed from unassigned_fields
             *      AND the Iron_Field has is_var=false).
             *   2. Removes the field from unassigned_fields so subsequent
             *      reads via FIELD_ACCESS (E0246) and exits (E0247) see it
             *      as assigned on this path.
             * Only fires when the assignment is a DIRECT self.<field>
             * write (not chained like self.inner.field). The chain walk
             * matches the Phase 84 readonly/pure guard above: walk through
             * FIELD_ACCESS links and confirm the innermost FIELD_ACCESS's
             * object is IDENT "self" (no intervening object layers). */
            if (ctx->in_init_method &&
                as->target && as->target->kind == IRON_NODE_FIELD_ACCESS) {
                Iron_FieldAccess *tfa = (Iron_FieldAccess *)as->target;
                if (tfa->object && tfa->object->kind == IRON_NODE_IDENT) {
                    Iron_Ident *rid = (Iron_Ident *)tfa->object;
                    if (rid->name && strcmp(rid->name, "self") == 0 &&
                        tfa->field) {
                        /* Look up whether the field is var or val on the
                         * enclosing ObjectDecl. */
                        bool target_is_val = false;
                        bool field_found = false;
                        if (ctx->current_method_type) {
                            Iron_Symbol *ts = iron_scope_lookup(
                                ctx->global_scope, ctx->current_method_type);
                            if (ts && ts->decl_node &&
                                ts->decl_node->kind == IRON_NODE_OBJECT_DECL) {
                                Iron_ObjectDecl *od = (Iron_ObjectDecl *)ts->decl_node;
                                for (int fi = 0; fi < od->field_count; fi++) {
                                    Iron_Field *ff = (Iron_Field *)od->fields[fi];
                                    if (ff && ff->name &&
                                        strcmp(ff->name, tfa->field) == 0) {
                                        field_found = true;
                                        target_is_val = !ff->is_var;
                                        break;
                                    }
                                }
                            }
                        }
                        if (field_found && target_is_val &&
                            ctx->unassigned_fields &&
                            shget(ctx->unassigned_fields, tfa->field) == 0) {
                            /* Field is a val AND was already removed from
                             * the unassigned set => second assignment. */
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "val field '%s' assigned more than once in init",
                                     tfa->field);
                            emit_error(ctx, IRON_ERR_INIT_VAL_DOUBLE_ASSIGN,
                                       as->span, msg, NULL);
                        }
                        if (ctx->unassigned_fields) {
                            (void)shdel(ctx->unassigned_fields, tfa->field);
                        }
                    }
                }
            }

            /* Phase 17 VAL-03: non-pub val field write OUTSIDE init is a
             * compile error. The pub-val branch below (line ~4011) handles
             * pub fields with IRON_ERR_VAL_REASSIGN=203. This branch covers
             * non-pub val with a dedicated code (IRON_ERR_VAL_FIELD_REASSIGN
             * =265) per CONTEXT.md decision so Phase 34's LSP-06 quickfix
             * can target it independently with the wording "declare field
             * as 'var'".
             *
             * Lexical scope only — methods called from init do NOT
             * transitively count (CONTEXT.md decision). The check matches
             * the chain shape of the in-init double-assign block above:
             * walk to root IDENT, confirm it is `self`, look up the field
             * on the enclosing object via ctx->current_method_type, fire
             * when !is_var && !is_pub. The !is_pub guard makes this branch
             * mutually exclusive with the pub-val branch below — both
             * cannot fire on the same write. */
            if (!ctx->in_synth_accessor &&
                !ctx->in_init_method &&
                as->target && as->target->kind == IRON_NODE_FIELD_ACCESS) {
                Iron_FieldAccess *tfa = (Iron_FieldAccess *)as->target;
                if (tfa->object && tfa->object->kind == IRON_NODE_IDENT) {
                    Iron_Ident *rid = (Iron_Ident *)tfa->object;
                    if (rid->name && strcmp(rid->name, "self") == 0 &&
                        tfa->field && ctx->current_method_type) {
                        Iron_Symbol *ts = iron_scope_lookup(
                            ctx->global_scope, ctx->current_method_type);
                        if (ts && ts->decl_node &&
                            ts->decl_node->kind == IRON_NODE_OBJECT_DECL) {
                            Iron_ObjectDecl *od = (Iron_ObjectDecl *)ts->decl_node;
                            for (int fi = 0; fi < od->field_count; fi++) {
                                Iron_Field *ff = (Iron_Field *)od->fields[fi];
                                if (ff && ff->name &&
                                    strcmp(ff->name, tfa->field) == 0 &&
                                    !ff->is_var && !ff->is_pub) {
                                    /* non-pub val field write outside init */
                                    char msg[256];
                                    snprintf(msg, sizeof(msg),
                                             "cannot reassign 'val' field '%s' "
                                             "after initialization",
                                             tfa->field);
                                    emit_error(ctx, IRON_ERR_VAL_FIELD_REASSIGN,
                                               as->span, msg,
                                               "declare field as 'var' to allow "
                                               "reassignment after init");
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            /* Phase 83-02 ACCESS-05: pub-field write dispatch.
             * When the LHS is a direct field-access on an object field marked
             * `pub var`, flag the assign so HIR lowers it as a call to the
             * synthesized set_<field> method. `pub val` rejects the write
             * with IRON_ERR_VAL_REASSIGN — pub val is read-only.
             *
             * Suppressed inside synth accessor bodies — the synth setter's
             * own `self.field = _v` must remain a direct store, otherwise
             * the lowered setter would infinitely call itself.
             *
             * Only the innermost field-access matters (chained field writes
             * like a.b.c = v treat `c` as the target; the earlier chain
             * already went through FIELD_ACCESS typecheck which set
             * is_pub_access on each link). */
            if (!ctx->in_synth_accessor &&
                as->target && as->target->kind == IRON_NODE_FIELD_ACCESS) {
                Iron_FieldAccess *tfa = (Iron_FieldAccess *)as->target;
                /* PROT-01 layout lock: every expression node begins with
                 * {span, kind, resolved_type}; safe to cast. check_expr on
                 * as->target above already resolved tfa->object's type. */
                Iron_Type *obj_ty = tfa->object
                    ? ((Iron_ExprNode *)tfa->object)->resolved_type
                    : NULL;
                if (obj_ty && obj_ty->kind == IRON_TYPE_RC) {
                    obj_ty = obj_ty->rc.inner;
                }
                if (obj_ty && obj_ty->kind == IRON_TYPE_OBJECT) {
                    Iron_ObjectDecl *od = obj_ty->object.decl;
                    Iron_Field *mf = NULL;
                    for (int fi = 0; fi < od->field_count; fi++) {
                        Iron_Field *f = (Iron_Field *)od->fields[fi];
                        if (strcmp(f->name, tfa->field) == 0) {
                            mf = f;
                            break;
                        }
                    }
                    if (mf && mf->is_pub) {
                        if (mf->is_var) {
                            /* pub var: route through synthesized setter. */
                            as->is_pub_setter = true;
                        } else if (!ctx->in_init_method) {
                            /* pub val: read-only outside init — cannot be
                             * written.  Init bodies are the sole place where
                             * val fields may be populated (Phase 88 E0264
                             * requires this). */
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "cannot assign to pub val field '%s' "
                                     "— use pub var for mutable properties",
                                     tfa->field);
                            emit_error(ctx, IRON_ERR_VAL_REASSIGN,
                                       as->span, msg, NULL);
                        }
                    }
                }
            }

            if (target_type && value_type &&
                target_type->kind != IRON_TYPE_ERROR &&
                value_type->kind  != IRON_TYPE_ERROR &&
                !types_assignable(target_type, value_type) &&
                !is_int_literal_narrowing(target_type, value_type, as->value)) {
                emit_type_mismatch(ctx, as->span, target_type, value_type);
            }
            /* Narrow literal in assignment (e.g., x = 42 where x: Int32) */
            if (is_int_literal_narrowing(target_type, value_type, as->value)) {
                ((Iron_IntLit *)as->value)->resolved_type = target_type;
            }
            /* Compound assignment overflow detection */
            if (is_compound_assign_op(as->op) && target_type &&
                target_type->kind != IRON_TYPE_ERROR &&
                is_narrow_integer(target_type)) {
                /* Check if RHS is a constant that fits the narrow target */
                bool suppress = false;
                if (as->value->kind == IRON_NODE_INT_LIT) {
                    Iron_IntLit *lit = (Iron_IntLit *)as->value;
                    errno = 0;
                    int64_t val = strtoll(lit->value, NULL, 10);
                    if (errno != ERANGE && value_fits_type(val, target_type)) {
                        suppress = true;  /* constant fits -- no warning */
                    }
                }
                if (!suppress) {
                    char msg[256];
                    const char *tgt_s = iron_type_to_string(target_type, ctx->arena);
                    snprintf(msg, sizeof(msg),
                             "compound assignment on narrow type '%s' may overflow",
                             tgt_s);
                    emit_warning(ctx, IRON_WARN_POSSIBLE_OVERFLOW, as->span,
                                 msg, "consider using a wider type or checking bounds");
                }
            }
            break;
        }

        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            Iron_Type *ret_type = NULL;

            /* Phase 20 PTR-10 (Plan 20-02a): compile-time stack-escape
             * detection. `return &local` where `local` is a stack-resident
             * binding (val/var inside a function body) emits E0271 — the
             * pointer would dangle past the frame's return. Indirect
             * escapes (closures, structures, function-pointer storage)
             * remain caught at runtime by the iron_check_stack_pointer_gen
             * panic in Plan 20-02b. */
            if (rs->value && rs->value->kind == IRON_NODE_UNARY) {
                Iron_UnaryExpr *ue_ret = (Iron_UnaryExpr *)rs->value;
                if (ue_ret->op == (Iron_OpKind)IRON_TOK_AMP) {
                    Iron_Symbol *root_sym =
                        iron_walk_to_root_binding(ue_ret->operand);
                    if (root_sym && sym_is_stack_local(root_sym)) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "cannot return reference to stack-local "
                                 "variable '%s'; the binding does not "
                                 "outlive the current function frame",
                                 root_sym->name ? root_sym->name : "?");
                        emit_error(ctx, IRON_ERR_PTR_ESCAPE_STACK_REF,
                                   ue_ret->span, msg,
                                   "allocate on the heap (Phase 21 'heap "
                                   "T(...)') or return the value by-copy");
                    }
                }
            }

            if (rs->value) {
                ret_type = check_expr_with_expected(ctx, rs->value, ctx->current_return_type);
            } else {
                ret_type = iron_type_make_primitive(IRON_TYPE_VOID);
            }

            /* Phase 24 (Plan 24-02, CONTEXT Area 5): drop body must not return
             * early — E0288. Check fires BEFORE init checks so a return in a
             * drop body emits one diagnostic, not two. */
            if (ctx->in_drop_method) {
                emit_error(ctx, IRON_ERR_DROP_NO_EARLY_RETURN, rs->span,
                           "drop body must not return early; let scope exit flow naturally",
                           "§6: drop body must not return early");
            }

            /* Phase 85 INIT-10/11: inside an init body,
             *   E0252 fires when `return <expr>` carries a value (init is a
             *   void-returning constructor; parser already blocks a declared
             *   return type, this blocks value-carrying returns in the body);
             *   E0250 fires when the return is reached with any field still
             *   in unassigned_fields (early-return before full assignment),
             *   regardless of whether the return carries a value. */
            if (ctx->in_init_method) {
                if (rs->value) {
                    emit_error(ctx, IRON_ERR_INIT_RETURN_VALUE, rs->span,
                               "init cannot return a value", NULL);
                }
                if (ctx->unassigned_fields &&
                    shlen(ctx->unassigned_fields) > 0) {
                    const char *first = ctx->unassigned_fields[0].key;
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "init cannot return before field '%s' is assigned on this path",
                             first ? first : "?");
                    emit_error(ctx, IRON_ERR_INIT_EARLY_RETURN, rs->span, msg, NULL);
                }
            }

            /* Phase 24 DROP-08 (Plan 24-02): nocopy type returned by value — E0286.
             * Site (c): IRON_NODE_RETURN. Only fires when the return expr is an IDENT
             * (copying an existing binding). Construction/call-returns are moves. */
            if (ret_type && ret_type->kind == IRON_TYPE_OBJECT &&
                ret_type->object.decl && ret_type->object.decl->is_nocopy &&
                ctx->current_return_type && ctx->current_return_type->kind == IRON_TYPE_OBJECT &&
                rs->value && rs->value->kind == IRON_NODE_IDENT) {
                emit_error(ctx, IRON_ERR_COPY_OF_NOCOPY_TYPE, rs->span,
                           "cannot return nocopy type by value — return requires copy",
                           "§7: nocopy types cannot be copied; pass `*T` or `*var T` to avoid copy");
            }

            if (ctx->current_return_type && ret_type) {
                if (ret_type->kind != IRON_TYPE_ERROR &&
                    ctx->current_return_type->kind != IRON_TYPE_ERROR) {

                    /* If returning a nullable type where non-nullable expected: E0204 */
                    if (ret_type->kind == IRON_TYPE_NULLABLE &&
                        ctx->current_return_type->kind != IRON_TYPE_NULLABLE) {
                        emit_error(ctx, IRON_ERR_NULLABLE_ACCESS, rs->span,
                                   "cannot return nullable value without null check",
                                   "Check for null before returning");
                    } else if (!types_assignable(ctx->current_return_type, ret_type) &&
                               !is_int_literal_narrowing(ctx->current_return_type, ret_type, rs->value)) {
                        /* Phase 25 PTR-03 (Plan 25-01): specialize to E0289
                         * IRON_ERR_PTR_REGIME_MISMATCH when both types are
                         * IRON_TYPE_PTR with differing is_unchecked (regime
                         * mismatch at return site). RESEARCH Pitfall 2: third
                         * site of the three-site coverage (val/var-decl +
                         * call-arg + return). */
                        if (ctx->current_return_type->kind == IRON_TYPE_PTR &&
                            ret_type->kind == IRON_TYPE_PTR &&
                            ctx->current_return_type->ptr.is_unchecked != ret_type->ptr.is_unchecked) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "return type regime mismatch: function returns '%s', got '%s'; "
                                     "checked and unchecked pointer regimes are disjoint",
                                     iron_type_to_string(ctx->current_return_type, ctx->arena),
                                     iron_type_to_string(ret_type, ctx->arena));
                            emit_error(ctx, IRON_ERR_PTR_REGIME_MISMATCH, rs->span, msg,
                                       "§4.3-§4.4: checked and unchecked pointer regimes are disjoint; "
                                       "use Box.unwrap() to escape from Box[T] to *unchecked T");
                        } else {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "return type mismatch: function returns '%s', got '%s'",
                                     iron_type_to_string(ctx->current_return_type, ctx->arena),
                                     iron_type_to_string(ret_type, ctx->arena));
                            emit_error(ctx, IRON_ERR_RETURN_TYPE, rs->span, msg, NULL);
                        }
                    }
                    /* Narrow literal in return (e.g., return 42 in Int32 func) */
                    if (is_int_literal_narrowing(ctx->current_return_type, ret_type, rs->value)) {
                        /* PROT-04 rewrite (rank 11b, AUDIT-01 post-merge): the
                         * is_int_literal_narrowing predicate has already confirmed
                         * rs->value->kind == IRON_NODE_INT_LIT, but leaves no
                         * structural proof. Assert the invariant explicitly so a
                         * future predicate bug aborts in Debug rather than
                         * silently writing to a foreign node layout. */
                        IRON_NODE_ASSERT_KIND(rs->value, IRON_NODE_INT_LIT);
                        Iron_IntLit *int_lit = (Iron_IntLit *)rs->value;
                        int_lit->resolved_type = ctx->current_return_type;
                    }
                }
            }
            break;
        }

        case IRON_NODE_IF: {
            Iron_IfStmt *is_s = (Iron_IfStmt *)node;

            /* Type-check condition */
            check_expr(ctx, is_s->condition);

            const char *null_check_name = NULL;
            int null_check_dir = classify_null_check(is_s->condition, &null_check_name);
            const char *is_check_name  = classify_is_check(is_s->condition);

            /* ── Case 1: x != null — narrow x to non-nullable in then-block ── */
            if (null_check_dir == 1 && null_check_name) {
                Iron_Symbol *sym = tc_lookup(ctx, null_check_name);
                Iron_Type *sym_type = sym ? sym->type : NULL;
                /* Also check resolved_sym fallback */
                if (!sym_type && !sym) {
                    /* look via global for param fallback */
                }

                if (sym_type && sym_type->kind == IRON_TYPE_NULLABLE) {
                    NarrowEntry *saved = narrowing_copy(ctx);
                    narrowing_set(ctx, null_check_name, sym_type->nullable.inner);
                    if (is_s->body) check_stmt(ctx, is_s->body);
                    shfree(ctx->narrowed);
                    ctx->narrowed = saved;
                } else {
                    if (is_s->body) check_stmt(ctx, is_s->body);
                }
                if (is_s->else_body) check_stmt(ctx, is_s->else_body);
            }
            /* ── Case 2: x == null ─────────────────────────────────────────── */
            else if (null_check_dir == -1 && null_check_name) {
                Iron_Symbol *sym = tc_lookup(ctx, null_check_name);
                Iron_Type *sym_type = sym ? sym->type : NULL;

                if (is_s->body) check_stmt(ctx, is_s->body);

                bool then_returns = false;
                if (is_s->body && is_s->body->kind == IRON_NODE_BLOCK) {
                    then_returns = block_always_returns((Iron_Block *)is_s->body);
                }

                /* If then-block always returns: narrow x to non-nullable in continuation */
                if (then_returns && sym_type && sym_type->kind == IRON_TYPE_NULLABLE) {
                    narrowing_set(ctx, null_check_name, sym_type->nullable.inner);
                }

                if (is_s->else_body) {
                    if (sym_type && sym_type->kind == IRON_TYPE_NULLABLE) {
                        NarrowEntry *saved = narrowing_copy(ctx);
                        narrowing_set(ctx, null_check_name, sym_type->nullable.inner);
                        check_stmt(ctx, is_s->else_body);
                        shfree(ctx->narrowed);
                        ctx->narrowed = saved;
                    } else {
                        check_stmt(ctx, is_s->else_body);
                    }
                }
            }
            /* ── Case 3: e is TypeName — narrow in then-block ─────────────── */
            else if (is_check_name) {
                Iron_Symbol *type_sym = iron_scope_lookup(ctx->global_scope, is_check_name);
                if (type_sym && type_sym->sym_kind == IRON_SYM_TYPE) {
                    /* PROT-03 row 19 (AUDIT-01 M-severity): is_s->condition is
                     * already classified as IRON_NODE_IS by classify_is_check
                     * upstream; the assert documents the invariant and catches
                     * future predicate drift. */
                    IRON_NODE_ASSERT_KIND(is_s->condition, IRON_NODE_IS);
                    Iron_IsExpr *ie = (Iron_IsExpr *)is_s->condition;
                    if (ie->expr && ie->expr->kind == IRON_NODE_IDENT) {
                        const char *ident_name = ((Iron_Ident *)ie->expr)->name;
                        NarrowEntry *saved = narrowing_copy(ctx);
                        narrowing_set(ctx, ident_name, type_sym->type);
                        if (is_s->body) check_stmt(ctx, is_s->body);
                        shfree(ctx->narrowed);
                        ctx->narrowed = saved;
                    } else {
                        if (is_s->body) check_stmt(ctx, is_s->body);
                    }
                } else {
                    if (is_s->body) check_stmt(ctx, is_s->body);
                }
                if (is_s->else_body) check_stmt(ctx, is_s->else_body);
            }
            /* ── Default: no narrowing ─────────────────────────────────────── */
            else {
                /* Phase 85 INIT-04/06: inside an init body, the definite-
                 * assignment analysis must union unassigned_fields across
                 * every control-flow branch. A field is considered unassigned
                 * on the merged path if it remains unassigned on ANY branch
                 * (then, any elif, else). If else is absent, the implicit
                 * "fall-through with no writes" branch preserves the
                 * pre-branch set (so no field is removed by the else path).
                 *
                 * Strategy: snapshot the pre-branch set once, run each branch
                 * starting from a fresh clone of the snapshot, capture the
                 * branch's resulting set, and union (keep-unassigned-in-ANY)
                 * them all into a merged set after traversal. */
                if (ctx->in_init_method && ctx->unassigned_fields) {
                    InitUnassignedEntry *pre = NULL;
                    init_unassigned_clone(&pre, ctx->unassigned_fields);

                    /* Collect per-branch post-traversal sets. We cap at
                     * 1 (then) + elif_count + 1 (else/implicit) branches. */
                    int branch_count = 1 + is_s->elif_count + 1;
                    InitUnassignedEntry **post =
                        (InitUnassignedEntry **)iron_arena_alloc(
                            ctx->arena,
                            (size_t)branch_count * sizeof(InitUnassignedEntry *),
                            _Alignof(InitUnassignedEntry *));
                    if (!post) iron_oom_abort("typecheck.c:IF init branch-merge post");
                    int branch_idx = 0;

                    /* then-branch */
                    shfree(ctx->unassigned_fields);
                    init_unassigned_clone(&ctx->unassigned_fields, pre);
                    if (is_s->body) check_stmt(ctx, is_s->body);
                    post[branch_idx++] = ctx->unassigned_fields;
                    ctx->unassigned_fields = NULL;

                    /* elif branches */
                    for (int i = 0; i < is_s->elif_count; i++) {
                        check_expr(ctx, is_s->elif_conds[i]);
                        init_unassigned_clone(&ctx->unassigned_fields, pre);
                        if (is_s->elif_bodies[i]) check_stmt(ctx, is_s->elif_bodies[i]);
                        post[branch_idx++] = ctx->unassigned_fields;
                        ctx->unassigned_fields = NULL;
                    }

                    /* else branch. If absent, the implicit "no writes"
                     * branch means the starting set (pre) is the ending set;
                     * record a clone of pre as the post-set. */
                    if (is_s->else_body) {
                        init_unassigned_clone(&ctx->unassigned_fields, pre);
                        check_stmt(ctx, is_s->else_body);
                        post[branch_idx++] = ctx->unassigned_fields;
                        ctx->unassigned_fields = NULL;
                    } else {
                        InitUnassignedEntry *pre_clone = NULL;
                        init_unassigned_clone(&pre_clone, pre);
                        post[branch_idx++] = pre_clone;
                    }

                    /* Union: a field is unassigned on the merged path if it
                     * is unassigned on ANY branch's post-set. */
                    InitUnassignedEntry *merged = NULL;
                    sh_new_strdup(merged);
                    for (int b = 0; b < branch_idx; b++) {
                        if (!post[b]) continue;
                        for (ptrdiff_t i = 0; i < shlen(post[b]); i++) {
                            if (shget(merged, post[b][i].key) == 0) {
                                shput(merged, post[b][i].key, 1);
                            }
                        }
                    }
                    for (int b = 0; b < branch_idx; b++) {
                        if (post[b]) shfree(post[b]);
                    }
                    shfree(pre);
                    ctx->unassigned_fields = merged;
                } else {
                    if (is_s->body) check_stmt(ctx, is_s->body);
                    for (int i = 0; i < is_s->elif_count; i++) {
                        check_expr(ctx, is_s->elif_conds[i]);
                        if (is_s->elif_bodies[i]) check_stmt(ctx, is_s->elif_bodies[i]);
                    }
                    if (is_s->else_body) check_stmt(ctx, is_s->else_body);
                }
            }
            break;
        }

        case IRON_NODE_WHILE: {
            Iron_WhileStmt *ws = (Iron_WhileStmt *)node;
            Iron_Type *cond_type = check_expr(ctx, ws->condition);
            if (cond_type && cond_type->kind != IRON_TYPE_BOOL &&
                cond_type->kind != IRON_TYPE_ERROR) {
                emit_error(ctx, IRON_ERR_TYPE_MISMATCH, ws->span,
                           "while condition must be Bool", NULL);
            }
            /* Phase 85 INIT-04/06: while bodies may execute zero times, so
             * writes inside the body do NOT remove fields from the outer
             * unassigned set. Snapshot the set, traverse the body (so E0246
             * / E0248 / E0249 / E0250 / E0252 still fire inside), then
             * restore the pre-loop snapshot. */
            if (ctx->in_init_method && ctx->unassigned_fields) {
                InitUnassignedEntry *pre = NULL;
                init_unassigned_clone(&pre, ctx->unassigned_fields);
                if (ws->body) check_stmt(ctx, ws->body);
                shfree(ctx->unassigned_fields);
                ctx->unassigned_fields = pre;
            } else {
                if (ws->body) check_stmt(ctx, ws->body);
            }
            break;
        }

        case IRON_NODE_FOR: {
            Iron_ForStmt *fs = (Iron_ForStmt *)node;
            Iron_Type *iter_t = check_expr(ctx, fs->iterable);
            tc_push_scope(ctx, IRON_SCOPE_BLOCK);
            /* Define loop variable with appropriate type.
             * For array iteration (for x in arr) the loop var has elem type.
             * For integer bound (for i in n) the loop var is Int. */
            Iron_Type *loop_var_type = iron_type_make_primitive(IRON_TYPE_INT);
            if (iter_t && iter_t->kind == IRON_TYPE_ARRAY) {
                loop_var_type = iter_t->array.elem;
            }
            tc_define(ctx, fs->var_name, IRON_SYM_VARIABLE, (Iron_Node *)fs, fs->span,
                      true, loop_var_type);
            /* Phase 85 INIT-04/06: for bodies may execute zero times (empty
             * iterable). Mirror the while-loop snapshot/restore so self.field
             * writes inside the body do not count toward "always assigned". */
            if (ctx->in_init_method && ctx->unassigned_fields) {
                InitUnassignedEntry *pre = NULL;
                init_unassigned_clone(&pre, ctx->unassigned_fields);
                if (fs->body) check_stmt(ctx, fs->body);
                shfree(ctx->unassigned_fields);
                ctx->unassigned_fields = pre;
            } else {
                if (fs->body) check_stmt(ctx, fs->body);
            }
            tc_pop_scope(ctx);
            break;
        }

        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            Iron_Type *subject_type = check_expr(ctx, ms->subject);
            for (int i = 0; i < ms->case_count; i++) {
                if (ms->cases[i]) check_stmt(ctx, ms->cases[i]);
            }
            if (ms->else_body) check_stmt(ctx, ms->else_body);
            /* Exhaustiveness check */
            if (subject_type && subject_type->kind == IRON_TYPE_ENUM) {
                Iron_EnumDecl *ed = subject_type->enu.decl;
                if (ed && ed->has_payloads) {
                    bool *covered = iron_arena_alloc(ctx->arena,
                        sizeof(bool) * (size_t)ed->variant_count, _Alignof(bool));
                    if (!covered) { /* HARD-09 REPLACE (typecheck.c:check_stmt MATCH covered payload) */ return; }
                    memset(covered, 0, sizeof(bool) * (size_t)ed->variant_count);
                    bool has_catch_all = (ms->else_body != NULL);
                    for (int i = 0; i < ms->case_count; i++) {
                        Iron_MatchCase *mc = (Iron_MatchCase *)ms->cases[i];
                        if (!mc || !mc->pattern) continue;
                        if (mc->pattern->kind != IRON_NODE_PATTERN) continue;
                        Iron_Pattern *p = (Iron_Pattern *)mc->pattern;
                        const char *vname = p->variant_name;
                        int vi = find_variant_index(ed, vname);
                        if (vi < 0) {
                            /* Unknown variant — already reported by resolver; skip */
                            continue;
                        }
                        if (covered[vi]) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "unreachable match arm: variant '%s' already covered",
                                     vname);
                            emit_error(ctx, IRON_ERR_UNREACHABLE_ARM, mc->pattern->span,
                                       msg, NULL);
                        } else {
                            covered[vi] = true;
                        }
                        /* Check pattern arity (binding_count must match payload_count) */
                        Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[vi];
                        if (p->binding_count != ev->payload_count) {
                            char msg[256];
                            snprintf(msg, sizeof(msg),
                                     "%s expects %d field(s) but pattern has %d",
                                     vname, ev->payload_count, p->binding_count);
                            emit_error(ctx, IRON_ERR_PATTERN_ARITY, mc->pattern->span,
                                       msg, NULL);
                        }
                    }
                    if (!has_catch_all) {
                        /* Check for missing variants */
                        int missing_count = 0;
                        for (int i = 0; i < ed->variant_count; i++) {
                            if (!covered[i]) missing_count++;
                        }
                        if (missing_count > 0) {
                            char msg[512];
                            int pos = snprintf(msg, sizeof(msg),
                                               "non-exhaustive match: missing variant(s): ");
                            bool first = true;
                            for (int i = 0; i < ed->variant_count; i++) {
                                if (!covered[i]) {
                                    Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[i];
                                    if (!first) pos += snprintf(msg + pos, sizeof(msg) - pos, ", ");
                                    pos += snprintf(msg + pos, sizeof(msg) - pos, "%s", ev->name);
                                    first = false;
                                }
                            }
                            emit_error(ctx, IRON_ERR_NONEXHAUSTIVE_MATCH, ms->span,
                                       msg, "add 'else -> ...' or handle each variant");
                        }
                    } else {
                        /* else arm present: emit info note listing which variants it catches */
                        int uncovered_count = 0;
                        for (int i = 0; i < ed->variant_count; i++) {
                            if (!covered[i]) uncovered_count++;
                        }
                        if (uncovered_count > 0) {
                            char msg[512];
                            int pos = snprintf(msg, sizeof(msg), "else catches: ");
                            bool first = true;
                            for (int i = 0; i < ed->variant_count; i++) {
                                if (!covered[i]) {
                                    Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[i];
                                    if (!first) pos += snprintf(msg + pos, sizeof(msg) - pos, ", ");
                                    pos += snprintf(msg + pos, sizeof(msg) - pos, "%s", ev->name);
                                    first = false;
                                }
                            }
                            (void)pos;  /* suppress unused-variable warning */
                            const char *note_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
                            if (!note_copy) { /* HARD-09 REPLACE (typecheck.c:check_stmt MATCH else-note) */ return; }
                            iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_NOTE, 0,
                                           ms->span, note_copy, NULL);
                        }
                    }
                } else if (ed) {
                    /* Plain enum (no payloads): check ident/pattern-based variant coverage */
                    int vc = ed->variant_count;
                    /* FIX-04 / audit row 13 — replace the former fixed-size
                     * `bool covered[256]` + `if (vc > 256) vc = 256;` silent
                     * truncation with a dynamically-sized buffer so plain
                     * enums with more than 256 variants are checked
                     * correctly instead of having silently-unchecked tail
                     * variants report spurious non-exhaustive match errors.
                     * HARD-09 REPLACE (Plan 01-04) — calloc OOM now causes
                     * us to skip the exhaustiveness check gracefully rather
                     * than abort. Pre-HARD-09 this was iron_oom_abort.
                     * vc is bounded by int so the cast to size_t is safe;
                     * the max(1, vc) guard keeps calloc(0) well-defined. */
                    size_t covered_n = (size_t)(vc > 0 ? vc : 1);
                    bool *covered = (bool *)calloc(covered_n, sizeof(bool));
                    if (!covered) {
                        /* HARD-09 REPLACE (match-exhaustiveness covered[]) —
                         * skip the exhaustiveness check on OOM rather than
                         * abort. Downstream users see an incomplete-match
                         * diagnostic only for the arms they actually wrote;
                         * the OOM itself is not user-visible. */
                        return;
                    }

                    for (int ci = 0; ci < ms->case_count; ci++) {
                        if (!ms->cases[ci]) continue;
                        Iron_MatchCase *mc = (Iron_MatchCase *)ms->cases[ci];
                        if (!mc->pattern) continue;

                        const char *vname = NULL;
                        if (mc->pattern->kind == IRON_NODE_IDENT) {
                            Iron_Ident *pid = (Iron_Ident *)mc->pattern;
                            if (pid->resolved_sym &&
                                pid->resolved_sym->sym_kind == IRON_SYM_ENUM_VARIANT &&
                                pid->resolved_sym->type &&
                                iron_type_equals(pid->resolved_sym->type, subject_type)) {
                                vname = pid->name;
                            }
                        } else if (mc->pattern->kind == IRON_NODE_ENUM_CONSTRUCT) {
                            Iron_EnumConstruct *pec = (Iron_EnumConstruct *)mc->pattern;
                            if (strcmp(pec->enum_name, ed->name) == 0) {
                                vname = pec->variant_name;
                            }
                        } else if (mc->pattern->kind == IRON_NODE_PATTERN) {
                            Iron_Pattern *pp = (Iron_Pattern *)mc->pattern;
                            if (strcmp(pp->enum_name, ed->name) == 0) {
                                vname = pp->variant_name;
                            }
                        }
                        if (vname) {
                            for (int vi = 0; vi < vc; vi++) {
                                Iron_EnumVariant *ev =
                                    (Iron_EnumVariant *)ed->variants[vi];
                                if (ev && strcmp(ev->name, vname) == 0) {
                                    if (covered[vi]) {
                                        char msg[256];
                                        snprintf(msg, sizeof(msg),
                                                 "duplicate match arm for variant '%s'",
                                                 vname);
                                        emit_error(ctx, IRON_ERR_DUPLICATE_MATCH_ARM,
                                                   mc->pattern->span, msg, NULL);
                                    }
                                    covered[vi] = true;
                                    break;
                                }
                            }
                        }
                    }

                    if (!ms->else_body) {
                        char uncovered_names[1024];
                        uncovered_names[0] = '\0';
                        int uncovered_count = 0;
                        for (int vi = 0; vi < vc; vi++) {
                            if (!covered[vi]) {
                                Iron_EnumVariant *ev =
                                    (Iron_EnumVariant *)ed->variants[vi];
                                if (ev) {
                                    if (uncovered_count > 0)
                                        strncat(uncovered_names, ", ",
                                                sizeof(uncovered_names) - strlen(uncovered_names) - 1);
                                    strncat(uncovered_names, ev->name,
                                            sizeof(uncovered_names) - strlen(uncovered_names) - 1);
                                    uncovered_count++;
                                }
                            }
                        }
                        if (uncovered_count > 0) {
                            char msg[1280];
                            snprintf(msg, sizeof(msg),
                                     "non-exhaustive match: uncovered variant(s): %s",
                                     uncovered_names);
                            emit_error(ctx, IRON_ERR_NONEXHAUSTIVE_MATCH,
                                       ms->subject->span, msg,
                                       "add the missing variants or an else clause");
                        }
                    }
                    /* FIX-04 row 13 — release the dynamic covered[] buffer. */
                    free(covered);
                }
            } else if (!ms->else_body) {
                /* Non-enum subject without else clause */
                emit_error(ctx, IRON_ERR_NONEXHAUSTIVE_MATCH,
                           ms->subject->span,
                           "match on non-enum type requires else clause",
                           "add an else clause");
            }
            break;
        }

        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)node;
            tc_push_scope(ctx, IRON_SCOPE_BLOCK);
            if (mc->pattern && mc->pattern->kind == IRON_NODE_PATTERN) {
                /* Recursively define all binding variables (including nested patterns) */
                tc_define_pattern_bindings(ctx, NULL, mc->pattern);
            } else if (mc->pattern) {
                /* Non-pattern (e.g. integer literal) — check as expression */
                check_expr(ctx, mc->pattern);
            }
            if (mc->body) check_stmt(ctx, mc->body);
            tc_pop_scope(ctx);
            break;
        }

        case IRON_NODE_DEFER: {
            Iron_DeferStmt *ds = (Iron_DeferStmt *)node;
            check_expr(ctx, ds->expr);
            /* Phase 21 DEFER-02: only `defer free <ident>` is supported in
             * v3.0-alpha.1; full defer semantics ship in Phase 32.
             * Primary emission site: typecheck.c so `ironc check` surfaces
             * the error (check does not run hir_lower). hir_lower.c retains
             * the structural check as a safety net (Pitfall 4). */
            {
                bool is_defer_free_ident =
                    ds->expr &&
                    ds->expr->kind == IRON_NODE_FREE &&
                    ((Iron_FreeStmt *)ds->expr)->expr &&
                    ((Iron_FreeStmt *)ds->expr)->expr->kind == IRON_NODE_IDENT;
                if (!is_defer_free_ident) {
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_DEFER_FORM_UNSUPPORTED, ds->span,
                                   "only `defer free <binding>` is supported"
                                   " in v3.0-alpha.1",
                                   "full `defer` semantics ship in Phase 32");
                }
            }
            break;
        }

        case IRON_NODE_FREE: {
            Iron_FreeStmt *frs = (Iron_FreeStmt *)node;
            check_expr(ctx, frs->expr);
            /* Phase 21 POL-04: free target must be a bare identifier. */
            if (frs->expr && frs->expr->kind != IRON_NODE_IDENT) {
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                               IRON_ERR_FREE_NOT_BINDING, frs->span,
                               "`free` target must be a binding name,"
                               " not an expression",
                               NULL);
            }
            break;
        }

        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            check_expr(ctx, ls->expr);
            /* Phase 21 POL-05: leak target must be a bare identifier. */
            if (ls->expr && ls->expr->kind != IRON_NODE_IDENT) {
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                               IRON_ERR_LEAK_NOT_BINDING, ls->span,
                               "`leak` target must be a binding name,"
                               " not an expression",
                               NULL);
            }
            break;
        }

        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *ss = (Iron_SpawnStmt *)node;
            if (ss->pool_expr) check_expr(ctx, ss->pool_expr);
            if (ss->body) check_stmt(ctx, ss->body);

            /* Store spawn body return type for downstream await lookup */
            if (ss->handle_name) {
                /* Walk the spawn body to find IRON_NODE_RETURN and use its expr type */
                Iron_Type *body_ret = iron_type_make_primitive(IRON_TYPE_INT);
                /* PROT-03 row 20 (AUDIT-01 M-severity): ss->body is normally an
                 * IRON_NODE_BLOCK but error-recovery paths can leave it as a
                 * non-block expression form; guard before the cast and assert
                 * the kind so a wrong-kind shape aborts in Debug. */
                Iron_Block *blk = NULL;
                if (ss->body && ss->body->kind == IRON_NODE_BLOCK) {
                    IRON_NODE_ASSERT_KIND(ss->body, IRON_NODE_BLOCK);
                    blk = (Iron_Block *)ss->body;
                }
                if (blk) {
                    for (int i = 0; i < blk->stmt_count; i++) {
                        if (blk->stmts[i]->kind == IRON_NODE_RETURN) {
                            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)blk->stmts[i];
                            if (rs->value) {
                                /* PROT-04 rewrite (rank 11a, AUDIT-01): rs->value
                                 * is a generic expression node (any kind). The
                                 * previous code aliased Iron_IntLit solely to read
                                 * resolved_type at the common prefix offset. Use
                                 * Iron_ExprNode from ast.h (layout-locked by
                                 * PROT-01 _Static_asserts) for type-safe prefix
                                 * access. */
                                Iron_ExprNode *expr_node = (Iron_ExprNode *)rs->value;
                                if (expr_node->resolved_type) {
                                    body_ret = expr_node->resolved_type;
                                }
                            }
                            break;
                        }
                    }
                }
                shput(ctx->spawn_result_types, ss->handle_name, body_ret);
            } else {
                /* Fire-and-forget spawn (no handle captured) -- emit warning */
                iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_WARNING,
                               IRON_WARN_SPAWN_NO_HANDLE, ss->span,
                               "spawned task handle not captured; use "
                               "`val h = spawn(...)` and `await h` to wait for completion",
                               NULL);
            }
            break;
        }

        /* HARD-04: graceful no-op on parser ErrorNode — no further analysis. */
        case IRON_NODE_ERROR:
            break;

        /* HARD-04: sentinel — never a real node kind. */
        case IRON_NODE_COUNT:
            break;

        /* -Wswitch-enum opt-out: check_stmt handles every statement kind
         * explicitly; any remaining Iron_NodeKind (expression kinds, helpers
         * like PARAM / FIELD / TYPE_ANNOTATION) is treated as an expression-
         * used-as-statement and routed through check_expr. */
        default:
            /* Expression used as statement */
            check_expr(ctx, node);
            break;
    }
}

static void check_block_stmts(TypeCtx *ctx, Iron_Node **stmts, int count) {
    for (int i = 0; i < count; i++) {
        /* HARD-05: cancel poll at top of block-statement bulk walker. */
        if (iron_cancel_requested(ctx->cancel_flag)) return;
        check_stmt(ctx, stmts[i]);
    }
}

/* ── Phase 24 DROP-06: compute_has_user_copy_transitive cache-populator ──── */

/* True if this Iron_Type (or any field type recursively) has a user-defined
 * copy block. Result is cached into Iron_Type.has_user_copy_transitive +
 * .has_user_copy_cached so codegen can read the cached field directly
 * without a cross-TU helper call (I8 fix). Also caches the resolved
 * Iron_Type* into each Iron_Field.field_type_cached for emit_helpers.c. */
static bool compute_has_user_copy_transitive(Iron_Type *t, TypeCtx *ctx) {
    if (!t) return false;
    if (t->has_user_copy_cached) return t->has_user_copy_transitive;
    bool result = false;
    if (t->kind == IRON_TYPE_OBJECT && t->object.decl && ctx->program) {
        Iron_ObjectDecl *od = t->object.decl;
        /* (a) scan program->decls for MethodDecl nodes whose type_name == od->name
         * and which have is_copy=true. Methods are NOT stored on Iron_ObjectDecl;
         * they are top-level IRON_NODE_METHOD_DECL nodes (Plan 86 layout). */
        for (int i = 0; i < ctx->program->decl_count && !result; i++) {
            Iron_Node *d = ctx->program->decls[i];
            if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
            Iron_MethodDecl *m = (Iron_MethodDecl *)d;
            if (!m->type_name || !od->name) continue;
            if (strcmp(m->type_name, od->name) == 0 && m->is_copy) result = true;
        }
        /* (b) any field transitively has user copy — also populate field_type_cached */
        for (int i = 0; i < od->field_count && !result; i++) {
            Iron_Field *f = (Iron_Field *)od->fields[i];
            if (!f || !f->type_ann) continue;
            Iron_Type *ft = resolve_type_annotation(ctx, f->type_ann);
            /* Cache the field's resolved type for emit_helpers.c codegen use */
            if (!f->field_type_cached) f->field_type_cached = ft;
            if (compute_has_user_copy_transitive(ft, ctx)) result = true;
        }
    }
    t->has_user_copy_transitive = result;
    t->has_user_copy_cached = true;
    return result;
}

/* ── READ-06: is_readonly_compatible_type — closed whitelist helper ─────── */

/* Determines whether a return type is readonly-compatible per spec §12 step 8.
 * Implements RESEARCH Pattern 4 closed whitelist with:
 *   - Pitfall 6 optimistic-cache for self-referential struct types
 *   - Pitfall 5 -Werror=switch-enum protection via explicit default arm
 *   - Pitfall 3 NULL resolved_type treated as INCOMPATIBLE (fail-safe)
 *
 * Compatible types: primitives (Int/Float/Bool/String + width variants + Void),
 *   IRON_TYPE_ARRAY with size >= 0 (fixed-size), IRON_TYPE_NULLABLE recursing
 *   on inner, IRON_TYPE_TUPLE recursing on elements, IRON_TYPE_OBJECT iff every
 *   field's resolved_type is compatible (transitive struct walk with cache).
 *
 * Incompatible: IRON_TYPE_RC, IRON_TYPE_PTR, IRON_TYPE_FUNC, IRON_TYPE_INTERFACE,
 *   IRON_TYPE_ENUM, IRON_TYPE_GENERIC_PARAM, IRON_TYPE_ERROR, IRON_TYPE_NULL,
 *   IRON_TYPE_ARRAY with size == -1 (dynamic = List[T]), and default (unknown).
 */
static bool is_readonly_compatible_type(const Iron_Type *t, TypeCtx *ctx) {
    if (!t) return true;  /* void / NULL — OK; void return is always compatible */

    /* Cache check (Pitfall 6 — break self-referential recursion for OBJECT types) */
    if (t->kind == IRON_TYPE_OBJECT && t->readonly_compat_cached) {
        return t->is_readonly_compatible;
    }

    switch (t->kind) {
        /* ── Whitelisted primitives ─────────────────────────────────────── */
        case IRON_TYPE_INT:    case IRON_TYPE_INT8:   case IRON_TYPE_INT16:
        case IRON_TYPE_INT32:  case IRON_TYPE_INT64:
        case IRON_TYPE_UINT:   case IRON_TYPE_UINT8:  case IRON_TYPE_UINT16:
        case IRON_TYPE_UINT32: case IRON_TYPE_UINT64:
        case IRON_TYPE_FLOAT:  case IRON_TYPE_FLOAT32: case IRON_TYPE_FLOAT64:
        case IRON_TYPE_BOOL:   case IRON_TYPE_STRING:
        case IRON_TYPE_VOID:
            return true;

        /* ── Fixed-size arrays ([T; N]), bounded vectors ([T; <=N]), dynamic lists ([T]) ── */
        case IRON_TYPE_ARRAY:
            /* Dynamic [T] (size == -1): allowed as readonly return because 'readonly'
             * means the method does not mutate self, not that it allocates nothing.
             * Stdlib String.split returns [String] from a readonly method — correct.
             * Fixed [T; N] and bounded [T; <=N] (size >= 0) also pass.
             * Phase 23 Plan 23-02: is_bounded covered by size >= 0 path. */
            return is_readonly_compatible_type(t->array.elem, ctx);

        /* ── Nullable (T?) — recurse on inner ───────────────────────────── */
        case IRON_TYPE_NULLABLE:
            return is_readonly_compatible_type(t->nullable.inner, ctx);

        /* ── Tuples — all elements must be compatible ────────────────────── */
        case IRON_TYPE_TUPLE:
            for (int i = 0; i < t->tuple.elem_count; i++) {
                if (!is_readonly_compatible_type(t->tuple.elem_types[i], ctx))
                    return false;
            }
            return true;

        /* ── Object (struct) — transitive field walk with optimistic cache ─ */
        case IRON_TYPE_OBJECT: {
            if (!t->object.decl) return false;
            Iron_ObjectDecl *od = t->object.decl;
            /* Pitfall 6 optimistic-cache: set BEFORE recursing into fields.
             * If a field back-references T (self-referential struct), the
             * recursive call sees cached=true, compatible=true — breaks the
             * cycle. If any field is later found incompatible, the cache is
             * corrected before return. Worst case: a self-referential struct
             * with an incompatible field at depth > 1 may be transiently
             * misclassified on the first walk; subsequent walks see corrected
             * cache. For Phase 22, this edge case is extremely rare and the
             * optimistic default (safe for pure primitives + nullable back-refs)
             * is the correct choice. */
            ((Iron_Type *)t)->readonly_compat_cached = true;
            ((Iron_Type *)t)->is_readonly_compatible  = true;
            for (int i = 0; i < od->field_count; i++) {
                Iron_Field *f = (Iron_Field *)od->fields[i];
                if (!f) continue;
                /* Pitfall 3: NULL type_ann / resolved field type = fail-safe REJECT.
                 * Iron_Field has no resolved_type field — field types are obtained
                 * via resolve_type_annotation (RESEARCH Pitfall 3 / FIX-03 §1). */
                Iron_Type *ft = f->type_ann
                    ? resolve_type_annotation(ctx, f->type_ann)
                    : NULL;
                if (!ft || !is_readonly_compatible_type(ft, ctx)) {
                    ((Iron_Type *)t)->is_readonly_compatible = false;
                    return false;
                }
            }
            return true;
        }

        /* ── Pointer types — not readonly-compatible ─────────────────────── */
        case IRON_TYPE_PTR:
            return false;

        /* ── All other types — not readonly-compatible ───────────────────── */
        case IRON_TYPE_RC:
        case IRON_TYPE_FUNC:
        case IRON_TYPE_INTERFACE:
        case IRON_TYPE_ENUM:
        case IRON_TYPE_GENERIC_PARAM:
        case IRON_TYPE_ERROR:
        case IRON_TYPE_NULL:
        default:
            /* Phase 23 BVEC: when IRON_TYPE_BVEC lands as a new kind, add
             * an explicit case here before the default arm. */
            return false;
    }
}

/* ── Check function / method declarations ────────────────────────────────── */

static void check_func_decl(TypeCtx *ctx, Iron_FuncDecl *fd) {
    /* Resolve return type */
    Iron_Type *ret_type = NULL;
    if (fd->return_type) {
        ret_type = resolve_type_annotation(ctx, fd->return_type);
    } else {
        ret_type = iron_type_make_primitive(IRON_TYPE_VOID);
    }
    fd->resolved_return_type = ret_type;

    /* Phase 22 READ-06: declaration-site readonly return-type check.
     * Pitfall 2: use fd->is_readonly directly, NOT ctx->in_readonly_method
     * (which is unset for top-level functions at declaration-check time). */
    if (fd->is_readonly && ret_type &&
        ret_type->kind != IRON_TYPE_VOID &&
        !is_readonly_compatible_type(ret_type, ctx)) {
        const char *ts = iron_type_to_string(ret_type, ctx->arena);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "readonly function return type '%s' is not readonly-compatible",
                 ts ? ts : "?");
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_READONLY_RETURN_TYPE, fd->span, msg,
                       "§6: readonly return types: primitives, fixed structs,"
                       " [T; N], [T; <=N], tuples, T?");
    }

    /* Resolve param types */
    Iron_Type **param_types = NULL;
    if (fd->param_count > 0) {
        param_types = (Iron_Type **)iron_arena_alloc(
            ctx->arena, (size_t)fd->param_count * sizeof(Iron_Type *),
            _Alignof(Iron_Type *));
        if (!param_types) { /* HARD-09 REPLACE (typecheck.c:check_func_decl param_types) */ return; }
    }
    for (int i = 0; i < fd->param_count; i++) {
        Iron_Param *p = (Iron_Param *)fd->params[i];
        param_types[i] = resolve_type_annotation(ctx, p->type_ann);
    }

    fd->resolved_param_types = param_types;

    /* Build and assign function type to the global symbol */
    Iron_Symbol *func_sym = iron_scope_lookup(ctx->global_scope, fd->name);
    Iron_Type *func_type = iron_type_make_func(ctx->arena, param_types,
                                                fd->param_count, ret_type);
    if (func_sym) func_sym->type = func_type;

    /* Set return type context and check body */
    Iron_Type *prev_ret = ctx->current_return_type;
    ctx->current_return_type = (ret_type->kind != IRON_TYPE_VOID) ? ret_type : NULL;

    /* Push function scope, define params */
    tc_push_scope(ctx, IRON_SCOPE_FUNCTION);
    for (int i = 0; i < fd->param_count; i++) {
        Iron_Param *p = (Iron_Param *)fd->params[i];
        tc_define(ctx, p->name, IRON_SYM_PARAM, fd->params[i], p->span,
                  p->is_var, param_types[i]);
    }

    if (fd->body && fd->body->kind == IRON_NODE_BLOCK) {
        Iron_Block *body = (Iron_Block *)fd->body;
        check_block_stmts(ctx, body->stmts, body->stmt_count);
    }

    /* Phase 4 Plan 04-01 (EDIT-07): missing-return walker (code 236).
     * Runs AFTER body check so `current_return_type` narrowing and
     * return-expr type-checks have already published diagnostics. */
    check_missing_return(ctx, fd);

    tc_pop_scope(ctx);
    ctx->current_return_type = prev_ret;
}

static void check_method_decl(TypeCtx *ctx, Iron_MethodDecl *md) {
    /* Array extension method stubs: generic type params (T, U) are not real
     * types in scope.  Return type resolution for call sites is handled by
     * resolve_array_ext_method().  Skip full type checking of stubs. */
    if (md->is_array_extension) {
        /* For empty-body stubs, nothing to check. For future methods with
         * real bodies, monomorphization would be needed. */
        return;
    }

    /* Resolve return type.
     *
     * Phase 85 INIT-11: init always returns Self implicitly. The parser
     * rejects explicit return types on init (md->return_type stays NULL).
     * Resolve it here to the enclosing object type so HIR/LIR lowering emits
     * a function returning the object, and the named-init call site
     * (Type.name(args)) recovers the constructed value as the call's result.
     * The anonymous-init path still flows through the v2.2 positional
     * construction fallback (designated-initializer emit), bypassing the
     * init body entirely — the resolved type here is only consumed by the
     * method's own codegen and by named-init dispatch. */
    /* Phase 87-02 SELF-01: set enclosing_type_name before resolving the return
     * type annotation so that resolve_type_annotation can resolve is_self_type
     * to the concrete enclosing type instead of emitting E0259. Saved here
     * and restored at exit (below). */
    const char *prev_enclosing_early = ctx->enclosing_type_name;
    ctx->enclosing_type_name = md->type_name;

    Iron_Type *ret_type = NULL;
    if (md->is_init && md->type_name) {
        Iron_Symbol *type_sym = iron_scope_lookup(ctx->global_scope, md->type_name);
        if (type_sym && type_sym->sym_kind == IRON_SYM_TYPE && type_sym->type) {
            ret_type = type_sym->type;
        }
    }
    if (!ret_type) {
        if (md->return_type) {
            ret_type = resolve_type_annotation(ctx, md->return_type);
        } else {
            ret_type = iron_type_make_primitive(IRON_TYPE_VOID);
        }
    }
    md->resolved_return_type = ret_type;

    /* Phase 22 READ-06: declaration-site readonly return-type check.
     * Pitfall 2: insertion is BEFORE body-walk; use md->is_readonly directly.
     * Skip init methods (their return type is Self, always compatible). */
    if (md->is_readonly && !md->is_init && ret_type &&
        ret_type->kind != IRON_TYPE_VOID &&
        !is_readonly_compatible_type(ret_type, ctx)) {
        const char *ts = iron_type_to_string(ret_type, ctx->arena);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "readonly method '%s.%s' return type '%s' is not readonly-compatible",
                 md->type_name ? md->type_name : "?",
                 md->method_name ? md->method_name : "?",
                 ts ? ts : "?");
        iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                       IRON_ERR_READONLY_RETURN_TYPE, md->span, msg,
                       "§6: readonly return types: primitives, fixed structs,"
                       " [T; N], [T; <=N], tuples, T?");
    }

    /* Resolve param types */
    Iron_Type **param_types = NULL;
    if (md->param_count > 0) {
        param_types = (Iron_Type **)iron_arena_alloc(
            ctx->arena, (size_t)md->param_count * sizeof(Iron_Type *),
            _Alignof(Iron_Type *));
        if (!param_types) { /* HARD-09 REPLACE (typecheck.c:check_method_decl param_types) */ return; }
    }
    for (int i = 0; i < md->param_count; i++) {
        Iron_Param *p = (Iron_Param *)md->params[i];
        param_types[i] = resolve_type_annotation(ctx, p->type_ann);
    }

    Iron_Type *prev_ret = ctx->current_return_type;
    const char *prev_type_name = ctx->current_method_type;
    /* NOTE: enclosing_type_name already saved as prev_enclosing_early above
     * and ctx->enclosing_type_name already set to md->type_name. */
    bool prev_in_synth    = ctx->in_synth_accessor;
    bool prev_in_readonly = ctx->in_readonly_method;
    bool prev_in_pure     = ctx->in_pure_method;
    bool prev_in_init     = ctx->in_init_method;
    bool prev_in_drop     = ctx->in_drop_method;
    bool prev_in_copy     = ctx->in_copy_method;
    /* Save the parent unassigned_fields pointer; we swap in a fresh per-init
     * set on init entry and shfree+restore at exit. For non-init methods we
     * leave the parent pointer in place (value is NULL outside inits). */
    InitUnassignedEntry *prev_unassigned = ctx->unassigned_fields;
    ctx->current_return_type   = (ret_type->kind != IRON_TYPE_VOID) ? ret_type : NULL;
    ctx->current_method_type   = md->type_name;
    /* enclosing_type_name already set to md->type_name above (before return type
     * resolution) so Self is available throughout the entire method check. */
    /* Phase 83-02: accessor bodies skip the pub-dispatch rewrite (see
     * typedef comment on TypeCtx.in_synth_accessor). */
    ctx->in_synth_accessor     = md->is_synth_accessor;
    /* Phase 84 MUTTIER-02/03: readonly/pure flag propagation for the body.
     * pure strictly implies readonly for self-write purposes; both flags
     * saved/restored so nested-method walks (should they appear in future
     * grammar) preserve the enclosing tier. */
    ctx->in_readonly_method    = md->is_readonly || md->is_pure;
    ctx->in_pure_method        = md->is_pure;
    /* Phase 85 INIT: enter init body. Populate unassigned_fields with every
     * field on the enclosing ObjectDecl so the definite-assignment analysis
     * can strike them off on `self.<field> = ...` writes and emit E0247 at
     * exit when any remain. */
    ctx->in_init_method        = md->is_init;
    /* Phase 24 DROP-01/06 (Plan 24-02): set drop/copy body flags */
    ctx->in_drop_method        = md->is_drop;
    ctx->in_copy_method        = md->is_copy;
    /* Phase 24 DROP-01: drop body cannot be marked readonly — drop mutates self */
    if (md->is_drop && md->is_readonly) {
        emit_error(ctx, IRON_ERR_DROP_NOT_READONLY, md->span,
                   "drop body cannot be marked 'readonly' — drop mutates self",
                   "§7: drop modifies the object before deallocation");
    }
    if (md->is_init) {
        ctx->unassigned_fields = NULL;
        sh_new_strdup(ctx->unassigned_fields);
        if (md->type_name) {
            Iron_Symbol *type_sym = iron_scope_lookup(ctx->global_scope, md->type_name);
            if (type_sym && type_sym->sym_kind == IRON_SYM_TYPE &&
                type_sym->decl_node &&
                type_sym->decl_node->kind == IRON_NODE_OBJECT_DECL) {
                Iron_ObjectDecl *od = (Iron_ObjectDecl *)type_sym->decl_node;
                for (int fi = 0; fi < od->field_count; fi++) {
                    Iron_Field *f = (Iron_Field *)od->fields[fi];
                    if (f && f->name) shput(ctx->unassigned_fields, f->name, 1);
                }
            }
        }
    }

    tc_push_scope(ctx, IRON_SCOPE_FUNCTION);

    /* Define 'self' — classic form only. Receiver form has no implicit
     * `self`; the receiver is bound under its declared name via the
     * params[0] entry below. */
    if (md->owner_sym && !md->is_receiver_form) {
        tc_define(ctx, "self", IRON_SYM_VARIABLE, (Iron_Node *)md, md->span,
                  true, md->owner_sym->type);
    }

    for (int i = 0; i < md->param_count; i++) {
        Iron_Param *p = (Iron_Param *)md->params[i];
        tc_define(ctx, p->name, IRON_SYM_PARAM, md->params[i], p->span,
                  p->is_var, param_types[i]);
    }

    if (md->body && md->body->kind == IRON_NODE_BLOCK) {
        Iron_Block *body = (Iron_Block *)md->body;
        check_block_stmts(ctx, body->stmts, body->stmt_count);
    }

    tc_pop_scope(ctx);

    /* Phase 85 INIT-06: if init body returned via the natural exit path
     * without filling every field, emit E0247 citing the first remaining
     * unassigned field. Early-return paths already fired E0250 during body
     * traversal — this guard handles the no-return exit case. */
    if (md->is_init && ctx->unassigned_fields &&
        shlen(ctx->unassigned_fields) > 0) {
        const char *first = ctx->unassigned_fields[0].key;
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "init leaves field '%s' unassigned on some exit path",
                 first ? first : "?");
        emit_error(ctx, IRON_ERR_INIT_UNASSIGNED_EXIT, md->span, msg, NULL);
    }
    if (md->is_init && ctx->unassigned_fields) {
        shfree(ctx->unassigned_fields);
    }
    ctx->unassigned_fields   = prev_unassigned;
    ctx->in_init_method      = prev_in_init;
    ctx->in_drop_method      = prev_in_drop;
    ctx->in_copy_method      = prev_in_copy;
    ctx->current_return_type = prev_ret;
    ctx->current_method_type = prev_type_name;
    ctx->enclosing_type_name  = prev_enclosing_early;
    ctx->in_synth_accessor   = prev_in_synth;
    ctx->in_readonly_method  = prev_in_readonly;
    ctx->in_pure_method      = prev_in_pure;
}

/* ── Interface completeness check ────────────────────────────────────────── */

static void check_interface_completeness(TypeCtx *ctx, Iron_Program *program) {
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl || decl->kind != IRON_NODE_OBJECT_DECL) continue;

        Iron_ObjectDecl *od = (Iron_ObjectDecl *)decl;
        if (od->implements_count == 0) continue;

        for (int j = 0; j < od->implements_count; j++) {
            const char *iface_name = od->implements_names[j];
            Iron_Symbol *iface_sym = iron_scope_lookup(ctx->global_scope, iface_name);
            if (!iface_sym || iface_sym->sym_kind != IRON_SYM_INTERFACE) continue;

            /* PROT-03 row 21 (AUDIT-01 M-severity): iface_sym->decl_node may
             * be NULL or a non-INTERFACE_DECL in error-recovery paths; guard
             * then assert kind before the cast so any wrong-kind decl_node
             * aborts in Debug instead of misreading the interface's vtable. */
            if (!iface_sym->decl_node ||
                iface_sym->decl_node->kind != IRON_NODE_INTERFACE_DECL) continue;
            /* HARD-10 REPLACE (audit row typecheck.c:3414):
             * guard above already rejects IRON_NODE_ERROR / non-INTERFACE_DECL;
             * former assert removed — kind check is the authoritative guard. */
            Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)iface_sym->decl_node;
            if (!iface) continue;

            for (int k = 0; k < iface->method_count; k++) {
                Iron_Node *sig_node = iface->method_sigs[k];
                if (!sig_node) continue;

                const char *method_name = NULL;
                if (sig_node->kind == IRON_NODE_FUNC_DECL) {
                    method_name = ((Iron_FuncDecl *)sig_node)->name;
                }
                if (!method_name) continue;

                bool found = false;
                for (int m = 0; m < program->decl_count; m++) {
                    Iron_Node *d = program->decls[m];
                    if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
                    Iron_MethodDecl *meth = (Iron_MethodDecl *)d;
                    if (strcmp(meth->type_name, od->name) == 0 &&
                        strcmp(meth->method_name, method_name) == 0) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    /* Phase 87-03 IFACE-03: interface methods with a default
                     * body are satisfied by inheritance — skip E0205 when
                     * sig->body != NULL (the implementer inherits the default). */
                    bool has_default_body = false;
                    if (sig_node->kind == IRON_NODE_FUNC_DECL) {
                        has_default_body =
                            (((Iron_FuncDecl *)sig_node)->body != NULL);
                    }
                    if (!has_default_body) {
                        char msg[512];
                        snprintf(msg, sizeof(msg),
                                 "object '%s' implements '%s' but is missing method '%s'",
                                 od->name, iface_name, method_name);
                        emit_error(ctx, IRON_ERR_MISSING_IFACE_METHOD, od->span, msg, NULL);
                    }
                }
            }
        }
    }
}

/* ── Phase 87 IFACE-02: interface method tier-strengthening (E0257) ─────── */

/* Helper: find a MethodDecl for (type_name, method_name) in program->decls.
 * Walks all top-level MethodDecl nodes; returns the first match or NULL.
 * This covers both in-object methods and patched methods (which are emitted
 * as top-level MethodDecls with type_name == patch target per Plan 86-01). */
static Iron_MethodDecl *find_method_for_object(Iron_Program *program,
                                               const char *type_name,
                                               const char *method_name) {
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (!md->type_name || !md->method_name) continue;
        if (strcmp(md->type_name, type_name) == 0 &&
            strcmp(md->method_name, method_name) == 0) {
            return md;
        }
    }
    return NULL;
}

/* Tier-strengthening scan (IFACE-02).
 * For every object decl with implements_count > 0, for every interface sig:
 *   - Look up the MethodDecl implementing it (in-object or patched).
 *   - If no impl exists AND sig has no default body, Plan 87-02 emits E0258.
 *     This plan only handles the tier-matching concern.
 *   - If impl found, compare tiers:
 *       iface pure     => impl must be pure
 *       iface readonly => impl must be readonly OR pure
 *       iface default  => any impl is OK
 *   - Emit E0257 with a locked message on violation. */
static void check_iface_tier_strengthening(TypeCtx *ctx, Iron_Program *program) {
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl || decl->kind != IRON_NODE_OBJECT_DECL) continue;

        Iron_ObjectDecl *od = (Iron_ObjectDecl *)decl;
        if (od->implements_count == 0) continue;

        for (int j = 0; j < od->implements_count; j++) {
            const char *iface_name = od->implements_names[j];
            if (!iface_name) continue;

            Iron_Symbol *iface_sym = iron_scope_lookup(ctx->global_scope, iface_name);
            if (!iface_sym || iface_sym->sym_kind != IRON_SYM_INTERFACE) continue;
            if (!iface_sym->decl_node ||
                iface_sym->decl_node->kind != IRON_NODE_INTERFACE_DECL) continue;

            Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)iface_sym->decl_node;
            if (!iface) continue;

            for (int k = 0; k < iface->method_count; k++) {
                Iron_Node *sig_node = iface->method_sigs[k];
                if (!sig_node || sig_node->kind != IRON_NODE_FUNC_DECL) continue;
                Iron_FuncDecl *sig = (Iron_FuncDecl *)sig_node;
                if (!sig->name) continue;

                /* Find the MethodDecl implementing this interface method.
                 * Covers in-object AND patched methods (both are top-level
                 * MethodDecl nodes with type_name == od->name per Plan 86). */
                Iron_MethodDecl *impl =
                    find_method_for_object(program, od->name, sig->name);

                if (!impl) {
                    /* No impl found. If sig has a default body, the implementer
                     * inherits it — no E0258. Otherwise emit E0258 (PATCH-08). */
                    if (sig->body != NULL) continue;  /* default body inherited */
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "missing interface method '%s.%s' on '%s'",
                             iface_name, sig->name, od->name);
                    const char *msg_copy =
                        iron_arena_strdup(ctx->arena, msg, strlen(msg));
                    if (!msg_copy)
                        iron_oom_abort("typecheck.c:check_iface_tier_strengthening e0258 msg");
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   IRON_ERR_IFACE_CONFORMANCE_MISSING,
                                   od->span, msg_copy, NULL);
                    continue;
                }

                /* Tier comparison:
                 *   iface pure     => impl must be pure
                 *   iface readonly => impl must be readonly OR pure
                 *   iface default  => any impl is fine */
                bool ok = true;
                const char *req_tier = "";
                if (sig->is_pure) {
                    ok       = impl->is_pure;
                    req_tier = "pure";
                } else if (sig->is_readonly) {
                    ok       = impl->is_readonly || impl->is_pure;
                    req_tier = "readonly";
                }

                if (!ok) {
                    const char *impl_tier = impl->is_pure     ? "pure"
                                          : impl->is_readonly ? "readonly"
                                          :                     "mutating";
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "interface method '%s.%s' requires %s; "
                             "implementation is %s",
                             iface_name, sig->name, req_tier, impl_tier);
                    const char *msg_copy =
                        iron_arena_strdup(ctx->arena, msg, strlen(msg));
                    if (!msg_copy)
                        iron_oom_abort("typecheck.c:check_iface_tier_strengthening msg");
                    /* Phase 22 READ-07: use READONLY-specific code for clearer spec
                     * tracing. Pure-sig violations CONTINUE to emit
                     * IRON_ERR_IFACE_METHOD_TIER_MISMATCH (257) to preserve Phase 87
                     * fixture compatibility per RESEARCH Pitfall 7. */
                    int diag_code = sig->is_pure
                        ? IRON_ERR_IFACE_METHOD_TIER_MISMATCH   /* Phase 87 baseline; pure-sig case */
                        : IRON_ERR_READONLY_IFACE_CONFORMANCE;  /* Phase 22 READ-07; readonly-sig case */
                    const char *hint = sig->is_pure
                        ? NULL   /* pure-tier hint conventions out of scope */
                        : "§6: interface readonly method requires readonly or pure implementation";
                    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR,
                                   diag_code, impl->span, msg_copy, hint);
                }
            }
        }
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void iron_typecheck(Iron_Program *program, Iron_Scope *global_scope,
                    Iron_Arena *arena, Iron_DiagList *diags,
                    const _Atomic bool *cancel_flag) {
    if (!program || !global_scope) return;
    /* HARD-05: pre-entry cancel check. */
    if (iron_cancel_requested(cancel_flag)) return;

    TypeCtx ctx;
    ctx.arena               = arena;
    ctx.diags               = diags;
    ctx.global_scope        = global_scope;
    ctx.current_scope       = global_scope;
    ctx.current_return_type = NULL;
    ctx.current_method_type = NULL;
    ctx.enclosing_type_name = NULL;
    ctx.in_synth_accessor   = false;
    ctx.in_readonly_method  = false;
    ctx.in_pure_method      = false;
    ctx.in_init_method      = false;
    ctx.in_drop_method      = false;
    ctx.in_copy_method      = false;
    ctx.cur_assign_target   = NULL;
    ctx.unassigned_fields   = NULL;
    ctx.narrowed            = NULL;
    ctx.program             = program;
    ctx.spawn_result_types  = NULL;
    ctx.mono_registry       = NULL;
    ctx.cancel_flag         = cancel_flag;
    sh_new_strdup(ctx.narrowed);
    sh_new_strdup(ctx.spawn_result_types);
    sh_new_strdup(ctx.mono_registry);

    /* Check top-level val/var declarations first so their init expressions
     * have resolved_type set before function bodies reference them.
     * The resolver already defined these symbols (with type=NULL).
     * We type-check the init and update the existing symbol's type. */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;

        if (decl->kind == IRON_NODE_VAL_DECL) {
            Iron_ValDecl *vd = (Iron_ValDecl *)decl;
            Iron_Type *init_type = NULL;
            if (vd->init) init_type = check_expr(&ctx, vd->init);
            Iron_Type *decl_type = vd->type_ann
                ? resolve_type_annotation(&ctx, vd->type_ann) : init_type;
            vd->declared_type = decl_type;
            /* Update the resolver's existing symbol with the resolved type */
            Iron_Symbol *sym = iron_scope_lookup(ctx.global_scope, vd->name);
            if (sym) sym->type = decl_type;
        } else if (decl->kind == IRON_NODE_VAR_DECL) {
            Iron_VarDecl *vd = (Iron_VarDecl *)decl;
            Iron_Type *init_type = NULL;
            if (vd->init) init_type = check_expr(&ctx, vd->init);
            Iron_Type *decl_type = vd->type_ann
                ? resolve_type_annotation(&ctx, vd->type_ann) : init_type;
            vd->declared_type = decl_type;
            /* Update the resolver's existing symbol with the resolved type */
            Iron_Symbol *sym = iron_scope_lookup(ctx.global_scope, vd->name);
            if (sym) sym->type = decl_type;
        }
    }

    /* Pre-pass: populate variant_payload_types for ADT enums.
     * Must run before function bodies reference IRON_NODE_ENUM_CONSTRUCT or
     * IRON_NODE_MATCH so that variant type information is available. */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl || decl->kind != IRON_NODE_ENUM_DECL) continue;
        Iron_EnumDecl *ed = (Iron_EnumDecl *)decl;
        if (!ed->has_payloads) continue;
        if (ed->generic_param_count > 0) continue; /* monomorphized in resolve_type_annotation */
        /* Look up the enum's type (registered by resolver in global scope) */
        Iron_Symbol *esym = iron_scope_lookup(ctx.global_scope, ed->name);
        if (!esym || !esym->type || esym->type->kind != IRON_TYPE_ENUM) continue;
        Iron_Type *ty = esym->type;
        /* Allocate outer array of Iron_Type** pointers */
        Iron_Type ***vpt = iron_arena_alloc(ctx.arena,
            sizeof(Iron_Type **) * (size_t)ed->variant_count, _Alignof(Iron_Type **));
        if (!vpt) { /* HARD-09 REPLACE (typecheck.c:iron_typecheck enum vpt) */ return; }
        memset(vpt, 0, sizeof(Iron_Type **) * (size_t)ed->variant_count);
        ty->enu.variant_payload_types = vpt;
        /* Allocate payload_is_boxed parallel structure on the type */
        bool **pib_ty = iron_arena_alloc(ctx.arena,
            sizeof(bool *) * (size_t)ed->variant_count, _Alignof(bool *));
        if (!pib_ty) { /* HARD-09 REPLACE (typecheck.c:iron_typecheck enum pib_ty) */ return; }
        memset(pib_ty, 0, sizeof(bool *) * (size_t)ed->variant_count);
        ty->enu.payload_is_boxed = pib_ty;
        for (int j = 0; j < ed->variant_count; j++) {
            Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
            if (ev->payload_count == 0) {
                vpt[j] = NULL;
                continue;
            }
            Iron_Type **row = iron_arena_alloc(ctx.arena,
                sizeof(Iron_Type *) * (size_t)ev->payload_count, _Alignof(Iron_Type *));
            if (!row) { /* HARD-09 REPLACE (typecheck.c:iron_typecheck enum vpt row) */ return; }
            /* Allocate boxing flags for this variant */
            ev->payload_is_boxed = iron_arena_alloc(ctx.arena,
                sizeof(bool) * (size_t)ev->payload_count, _Alignof(bool));
            if (!ev->payload_is_boxed) { /* HARD-09 REPLACE (typecheck.c:iron_typecheck enum ev payload_is_boxed) */ return; }
            memset(ev->payload_is_boxed, 0, sizeof(bool) * (size_t)ev->payload_count);
            bool *pib_row = iron_arena_alloc(ctx.arena,
                sizeof(bool) * (size_t)ev->payload_count, _Alignof(bool));
            if (!pib_row) { /* HARD-09 REPLACE (typecheck.c:iron_typecheck enum pib_row) */ return; }
            memset(pib_row, 0, sizeof(bool) * (size_t)ev->payload_count);
            for (int k = 0; k < ev->payload_count; k++) {
                row[k] = resolve_type_annotation(&ctx, ev->payload_type_anns[k]);
                /* Mark boxed if payload type is the same enum type (recursive) */
                ev->payload_is_boxed[k] = iron_type_equals(row[k], ty);
                pib_row[k] = ev->payload_is_boxed[k];
            }
            vpt[j] = row;
            pib_ty[j] = pib_row;
        }
    }

    /* Pre-pass: build function/method type signatures and set them in the
     * symbol table BEFORE checking bodies.  This enables mutual recursion
     * (e.g. is_even calls is_odd and vice-versa) by ensuring every function
     * symbol already has its type when referenced as a callee. */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        if (decl->kind == IRON_NODE_FUNC_DECL) {
            Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
            Iron_Type *ret_type = fd->return_type
                ? resolve_type_annotation(&ctx, fd->return_type)
                : iron_type_make_primitive(IRON_TYPE_VOID);
            Iron_Type **param_types = NULL;
            if (fd->param_count > 0) {
                param_types = (Iron_Type **)iron_arena_alloc(
                    ctx.arena, (size_t)fd->param_count * sizeof(Iron_Type *),
                    _Alignof(Iron_Type *));
                if (!param_types) { /* HARD-09 REPLACE (typecheck.c:iron_typecheck FUNC_DECL param_types) */ return; }
                for (int j = 0; j < fd->param_count; j++) {
                    Iron_Param *p = (Iron_Param *)fd->params[j];
                    param_types[j] = resolve_type_annotation(&ctx, p->type_ann);
                }
            }
            Iron_Type *func_type = iron_type_make_func(ctx.arena, param_types,
                                                        fd->param_count, ret_type);
            Iron_Symbol *sym = iron_scope_lookup(ctx.global_scope, fd->name);
            if (sym) sym->type = func_type;
        } else if (decl->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
            /* Skip symbol-type resolution for array extension methods: their
             * generic params (T, U) are not real types in the global scope.
             * Call-site type resolution is handled by resolve_array_ext_method. */
            if (md->is_array_extension) continue;
            /* Phase 87-02 SELF-01: set enclosing_type_name so that a method
             * return annotation of `Self` resolves correctly (and does not
             * trigger E0259) during this pre-pass signature building step. */
            ctx.enclosing_type_name = md->type_name;
            Iron_Type *ret_type = md->return_type
                ? resolve_type_annotation(&ctx, md->return_type)
                : iron_type_make_primitive(IRON_TYPE_VOID);
            ctx.enclosing_type_name = NULL;  /* restore after pre-pass sig build */
            /* Method signatures are looked up by mangled name (type_method) */
            char mangled[256];
            snprintf(mangled, sizeof(mangled), "%s_%s", md->type_name, md->method_name);
            Iron_Symbol *sym = iron_scope_lookup(ctx.global_scope, mangled);
            if (sym && !sym->type) {
                Iron_Type **param_types = NULL;
                int pc = md->param_count;
                if (pc > 0) {
                    param_types = (Iron_Type **)iron_arena_alloc(
                        ctx.arena, (size_t)pc * sizeof(Iron_Type *),
                        _Alignof(Iron_Type *));
                    if (!param_types) { /* HARD-09 REPLACE (typecheck.c:iron_typecheck METHOD_DECL param_types) */ return; }
                    for (int j = 0; j < pc; j++) {
                        Iron_Param *p = (Iron_Param *)md->params[j];
                        param_types[j] = resolve_type_annotation(&ctx, p->type_ann);
                    }
                }
                sym->type = iron_type_make_func(ctx.arena, param_types, pc, ret_type);
            }
        }
    }

    /* Phase 86 PATCH-03/06: cross-patch + patch-vs-in-object collision
     * scan. Build the program-global patch registry on the same global
     * scope resolve used, then per-target gather (a) in-object method
     * names on the corresponding ObjectDecl + (b) names contributed by
     * every patch entry. Duplicate names across (a)+(b) or across
     * multiple patch entries emit IRON_ERR_PATCH_CONFLICT at the LATER
     * declaration site. Method name equality is sufficient for Phase 86
     * (generics deferred; signature-tuple dispatch is a Phase 87 IFACE
     * + SELF concern).
     *
     * The registry is built AFTER resolve but before body typechecking
     * so E0255 fires ahead of any dispatch resolution — matches the
     * user expectation that conflicts are a compile-time structural
     * error, not a dispatch ambiguity. */
    Iron_TypePatchRegistry *patch_registry =
        iron_type_patch_registry_build(program, global_scope, arena, diags);
    if (patch_registry) {
        /* Collect the set of target names already seen so we scan each
         * target at most once even when multiple patch entries target it. */
        for (int i = 0; i < program->decl_count; i++) {
            Iron_Node *d = program->decls[i];
            if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
            if (!od->is_patch) continue;
            const char *target = od->target_type_name
                                 ? od->target_type_name : od->name;
            if (!target) continue;

            /* De-dup: if an earlier iteration already scanned this
             * target, skip. Linear O(i) re-scan is fine for realistic
             * patch counts. */
            bool already_scanned = false;
            for (int pj = 0; pj < i; pj++) {
                Iron_Node *dj = program->decls[pj];
                if (!dj || dj->kind != IRON_NODE_OBJECT_DECL) continue;
                Iron_ObjectDecl *odj = (Iron_ObjectDecl *)dj;
                if (!odj->is_patch) continue;
                const char *tj = odj->target_type_name
                                 ? odj->target_type_name : odj->name;
                if (tj && strcmp(tj, target) == 0) {
                    already_scanned = true;
                    break;
                }
            }
            if (already_scanned) continue;

            /* Gather names. Each entry: (name, decl-pointer-for-diagnostic-site). */
            typedef struct {
                const char      *name;
                Iron_MethodDecl *md;
            } NameRef;
            NameRef *names = NULL;

            /* (a) in-object methods: find the ObjectDecl for `target` and
             * collect every MethodDecl in program->decls whose
             * type_name==target AND is NOT contributed by a patch (i.e.
             * not in the registry's entries). Simpler approach: consider
             * every top-level MethodDecl with type_name==target as the
             * unified set, then detect duplicates by walking that set —
             * naturally covers patch-vs-patch AND patch-vs-in-object. */
            for (int mi = 0; mi < program->decl_count; mi++) {
                Iron_Node *md_node = program->decls[mi];
                if (!md_node || md_node->kind != IRON_NODE_METHOD_DECL) continue;
                Iron_MethodDecl *md = (Iron_MethodDecl *)md_node;
                if (!md->type_name || strcmp(md->type_name, target) != 0) continue;
                if (!md->method_name) continue;
                NameRef nr = { .name = md->method_name, .md = md };
                arrput(names, nr);
            }

            /* Detect duplicates. Emit at the LATER decl site; cite the
             * target and conflicting name. */
            for (ptrdiff_t a = 0; a < arrlen(names); a++) {
                for (ptrdiff_t b = a + 1; b < arrlen(names); b++) {
                    if (strcmp(names[a].name, names[b].name) != 0) continue;
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "conflicting patch definitions for '%s.%s'",
                             target, names[a].name);
                    const char *msg_copy = iron_arena_strdup(arena, msg, strlen(msg));
                    if (!msg_copy) iron_oom_abort("typecheck.c:patch conflict msg");
                    iron_diag_emit(diags, arena, IRON_DIAG_ERROR,
                                   IRON_ERR_PATCH_CONFLICT,
                                   names[b].md->span,
                                   msg_copy, NULL);
                }
            }
            arrfree(names);
        }
    }

    /* Phase 24 DROP-01/06 (Plan 24-02): duplicate drop/copy block detection +
     * compute_has_user_copy_transitive cache warming for all object types.
     * Runs before method body typecheck so cache is populated for codegen use. */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl || decl->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)decl;

        /* Duplicate drop/copy detection.
         * Methods are NOT stored on Iron_ObjectDecl (Plan 86 layout) — scan
         * program->decls for IRON_NODE_METHOD_DECL nodes whose type_name == od->name. */
        int drop_count = 0, copy_count = 0;
        for (int mi = 0; mi < program->decl_count; mi++) {
            Iron_Node *mn = program->decls[mi];
            if (!mn || mn->kind != IRON_NODE_METHOD_DECL) continue;
            Iron_MethodDecl *m = (Iron_MethodDecl *)mn;
            if (!m->type_name || !od->name) continue;
            if (strcmp(m->type_name, od->name) != 0) continue;
            if (m->is_drop) {
                if (drop_count > 0) {
                    emit_error(&ctx, IRON_ERR_DROP_DUPLICATE, m->span,
                               "duplicate drop block — at most one drop per object",
                               "§7: at most one drop block per object");
                }
                drop_count++;
            }
            if (m->is_copy) {
                if (copy_count > 0) {
                    emit_error(&ctx, IRON_ERR_COPY_DUPLICATE, m->span,
                               "duplicate copy block — at most one copy per object",
                               "§7: at most one copy block per object");
                }
                copy_count++;
            }
        }

        /* Warm the user-copy transitivity cache for this type */
        Iron_Symbol *type_sym = iron_scope_lookup(ctx.global_scope, od->name);
        if (type_sym && type_sym->type) {
            compute_has_user_copy_transitive(type_sym->type, &ctx);
        }
    }

    /* Check all func and method decls */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (!decl) continue;

        if (decl->kind == IRON_NODE_FUNC_DECL) {
            check_func_decl(&ctx, (Iron_FuncDecl *)decl);
        } else if (decl->kind == IRON_NODE_METHOD_DECL) {
            check_method_decl(&ctx, (Iron_MethodDecl *)decl);
        }
    }

    /* Phase 86: release the patch registry. MethodDecl * elements inside
     * are arena-shared; the registry itself is heap-owned via stb_ds.
     * Must run before arena teardown but after method-body typechecking
     * so any future call-site consumers can still look up patched
     * members via iron_type_patch_lookup within this function. */
    iron_type_patch_registry_free(patch_registry);

    /* Interface completeness */
    check_interface_completeness(&ctx, program);

    /* Phase 87 IFACE-02: tier-strengthening scan. Runs after method bodies
     * are typechecked so is_readonly/is_pure on MethodDecl nodes are confirmed
     * by parse-time assignment (Plan 84-01). Emits E0257 for every (object,
     * iface) pair where an implementation method's tier is weaker than its
     * corresponding interface signature tier. */
    check_iface_tier_strengthening(&ctx, program);

    shfree(ctx.narrowed);
    shfree(ctx.spawn_result_types);
    /* FIX-03 / AUDIT-04 §2: explicit shfree of the mono_registry stb_ds
     * string-keyed hashmap. Pre-Phase-67 the registry was shput-filled in
     * resolve_type_annotation (lines ~737, ~2408) but never freed — every
     * compilation unit leaked the map plus all strdup'd mangled-name keys
     * (sh_new_strdup was called at line ~3471 above). The map's VALUES are
     * arena-allocated Iron_Type*, which the parser arena reclaims later,
     * but the stb_ds backing buffer and the strdup'd keys are heap, and
     * they outlived every consumer in the pre-67-07 codebase. This shfree
     * closes that leak; paired with narrowed/spawn_result_types above it
     * now runs immediately after every consumer of mono_registry has
     * completed (check_func_decl / check_method_decl / check_interface_
     * completeness are the only readers per grep at 715/2386). */
    shfree(ctx.mono_registry);
}
