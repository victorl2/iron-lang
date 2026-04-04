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
#include "lexer/lexer.h"
#include "util/strbuf.h"
#include "vendor/stb_ds.h"

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>

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
    Iron_Arena        *arena;
    Iron_DiagList     *diags;
    Iron_Scope        *global_scope;
    Iron_Scope        *current_scope;   /* type-checker's own scope chain */
    Iron_Type         *current_return_type;  /* expected return type; NULL outside funcs */
    const char        *current_method_type;  /* owning type name if in method */
    NarrowEntry       *narrowed;             /* stb_ds map: sym name -> narrowed type */
    Iron_Program      *program;              /* for method return type lookup */
    SpawnResultEntry  *spawn_result_types;   /* stb_ds map: handle_name -> body return type */
} TypeCtx;

/* ── Forward declarations ────────────────────────────────────────────────── */

static Iron_Type *check_expr(TypeCtx *ctx, Iron_Node *node);
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
    switch (t->kind) {
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
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, code, span,
                   iron_arena_strdup(ctx->arena, msg, strlen(msg)),
                   suggestion ? iron_arena_strdup(ctx->arena, suggestion,
                                                   strlen(suggestion)) : NULL);
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
    if (iron_type_equals(decl_t, init_t)) return true;
    /* Int32 -> Int: implicit widening (always safe) */
    if (decl_t->kind == IRON_TYPE_INT && init_t->kind == IRON_TYPE_INT32) return true;
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

/* Allow integer literals to implicitly narrow to Int32.
 * `val x: Int32 = 42` is safe because the literal is statically known.
 * `val x: Int32 = someIntVar` is NOT allowed -- use Int32(someIntVar).
 */
static bool is_int_literal_narrowing(const Iron_Type *decl_t, const Iron_Type *init_t,
                                     const Iron_Node *init_node) {
    if (!decl_t || !init_t || !init_node) return false;
    return (decl_t->kind == IRON_TYPE_INT32 &&
            init_t->kind == IRON_TYPE_INT &&
            init_node->kind == IRON_NODE_INT_LIT);
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
static Iron_Type *substitute_generic_type(TypeCtx *ctx, Iron_Type *t,
                                           Iron_EnumDecl *ed,
                                           Iron_Type **type_args,
                                           int type_arg_count) {
    if (!t) return t;
    switch (t->kind) {
        case IRON_TYPE_GENERIC_PARAM: {
            /* Find the param index by name */
            for (int i = 0; i < type_arg_count && i < ed->generic_param_count; i++) {
                Iron_Ident *param = (Iron_Ident *)ed->generic_params[i];
                if (param && t->generic_param.name &&
                    strcmp(param->name, t->generic_param.name) == 0) {
                    return type_args[i];
                }
            }
            return t; /* unmatched param, return as-is */
        }
        case IRON_TYPE_NULLABLE: {
            Iron_Type *inner = substitute_generic_type(ctx, t->nullable.inner,
                                                        ed, type_args, type_arg_count);
            if (inner == t->nullable.inner) return t;
            return iron_type_make_nullable(ctx->arena, inner);
        }
        case IRON_TYPE_ARRAY: {
            Iron_Type *elem = substitute_generic_type(ctx, t->array.elem,
                                                       ed, type_args, type_arg_count);
            if (elem == t->array.elem) return t;
            return iron_type_make_array(ctx->arena, elem, t->array.size);
        }
        case IRON_TYPE_ENUM: {
            /* Nested generic enum: no deep substitution in this phase */
            if (t->enu.type_arg_count == 0) return t;
            bool changed = false;
            Iron_Type **new_args = iron_arena_alloc(ctx->arena,
                sizeof(Iron_Type *) * (size_t)t->enu.type_arg_count,
                _Alignof(Iron_Type *));
            for (int i = 0; i < t->enu.type_arg_count; i++) {
                new_args[i] = substitute_generic_type(ctx, t->enu.type_args[i],
                                                       ed, type_args, type_arg_count);
                if (new_args[i] != t->enu.type_args[i]) changed = true;
            }
            if (!changed) return t;
            return t; /* nested generic substitution deferred */
        }
        default:
            return t; /* primitive types, etc. — no substitution needed */
    }
}

/* ── Type annotation resolution ─────────────────────────────────────────── */

static Iron_Type *resolve_type_annotation(TypeCtx *ctx, Iron_Node *ann_node) {
    if (!ann_node) return iron_type_make_primitive(IRON_TYPE_VOID);

    if (ann_node->kind != IRON_NODE_TYPE_ANNOTATION) {
        return iron_type_make_primitive(IRON_TYPE_ERROR);
    }

    Iron_TypeAnnotation *ann = (Iron_TypeAnnotation *)ann_node;
    const char *name = ann->name;
    Iron_Type *base = NULL;

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
                    iron_strbuf_free(&sb);

                    /* Build monomorphized Iron_Type */
                    Iron_Type *mono = iron_arena_alloc(ctx->arena, sizeof(Iron_Type),
                        _Alignof(Iron_Type));
                    memset(mono, 0, sizeof(*mono));
                    mono->kind = IRON_TYPE_ENUM;
                    mono->enu.decl = ed;
                    mono->enu.type_args = type_args;
                    mono->enu.type_arg_count = ann->generic_arg_count;
                    mono->enu.mangled_name = mangled;

                    /* Substitute variant_payload_types:
                     * Push generic param names as GENERIC_PARAM types in a temporary scope,
                     * resolve each variant's payload type annotations, then substitute. */
                    Iron_Scope *saved_scope = ctx->global_scope;
                    Iron_Scope *gen_scope = iron_scope_create(ctx->arena,
                        ctx->global_scope, IRON_SCOPE_BLOCK);
                    for (int i = 0; i < ed->generic_param_count; i++) {
                        Iron_Ident *param = (Iron_Ident *)ed->generic_params[i];
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

                    Iron_Type ***vpt = iron_arena_alloc(ctx->arena,
                        sizeof(Iron_Type **) * (size_t)ed->variant_count,
                        _Alignof(Iron_Type **));
                    memset(vpt, 0, sizeof(Iron_Type **) * (size_t)ed->variant_count);
                    for (int j = 0; j < ed->variant_count; j++) {
                        Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                        if (ev->payload_count == 0) { vpt[j] = NULL; continue; }
                        Iron_Type **row = iron_arena_alloc(ctx->arena,
                            sizeof(Iron_Type *) * (size_t)ev->payload_count,
                            _Alignof(Iron_Type *));
                        for (int k = 0; k < ev->payload_count; k++) {
                            Iron_Type *pt = resolve_type_annotation(
                                ctx, ev->payload_type_anns[k]);
                            row[k] = substitute_generic_type(ctx, pt, ed,
                                type_args, ann->generic_arg_count);
                        }
                        vpt[j] = row;
                    }
                    ctx->global_scope = saved_scope;

                    mono->enu.variant_payload_types = vpt;
                    base = mono;
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
        base = iron_type_make_nullable(ctx->arena, base);
    }

    /* Wrap in array if needed */
    if (ann->is_array) {
        int size = -1;  /* dynamic by default */
        if (ann->array_size && ann->array_size->kind == IRON_NODE_INT_LIT) {
            Iron_IntLit *il = (Iron_IntLit *)ann->array_size;
            if (il->value) size = (int)strtol(il->value, NULL, 10);
        }
        base = iron_type_make_array(ctx->arena, base, size);
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

/* ── Expression type inference ───────────────────────────────────────────── */

static Iron_Type *check_expr(TypeCtx *ctx, Iron_Node *node) {
    if (!node) return iron_type_make_primitive(IRON_TYPE_VOID);

    Iron_Type *result = NULL;

    switch (node->kind) {
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
                check_expr(ctx, n->parts[i]);
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

            if (lt && rt && lt->kind != IRON_TYPE_ERROR && rt->kind != IRON_TYPE_ERROR) {
                bool lt_is_int   = (lt->kind == IRON_TYPE_INT);
                bool lt_is_float = (lt->kind == IRON_TYPE_FLOAT ||
                                    lt->kind == IRON_TYPE_FLOAT32 ||
                                    lt->kind == IRON_TYPE_FLOAT64);
                bool rt_is_int   = (rt->kind == IRON_TYPE_INT);
                bool rt_is_float = (rt->kind == IRON_TYPE_FLOAT ||
                                    rt->kind == IRON_TYPE_FLOAT32 ||
                                    rt->kind == IRON_TYPE_FLOAT64);

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
                    if (!iron_type_equals(lt, rt)) {
                        emit_type_mismatch(ctx, be->span, lt, rt);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    } else if (!iron_type_is_numeric(lt)) {
                        emit_error(ctx, IRON_ERR_TYPE_MISMATCH, be->span,
                                   "arithmetic operator requires numeric operands", NULL);
                        result = iron_type_make_primitive(IRON_TYPE_ERROR);
                    } else {
                        result = lt;
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
            } else {
                result = ot ? ot : iron_type_make_primitive(IRON_TYPE_ERROR);
            }
            ue->resolved_type = result;
            break;
        }

        case IRON_NODE_CALL: {
            Iron_CallExpr *ce = (Iron_CallExpr *)node;

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
                        switch (target_t->kind) {
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
                            default:
                                break;
                        }
                        if (is_numeric_or_bool) {
                            /* Type-check the argument */
                            check_expr(ctx, ce->args[0]);
                            /* Mark as primitive cast for the lowerer */
                            ce->is_primitive_cast = true;
                            result = target_t;
                            ce->resolved_type = result;
                            callee_id->resolved_type = result;
                            break;
                        }
                    }
                    /* Treat as construction: validate args against fields */
                    Iron_ObjectDecl *od = (Iron_ObjectDecl *)callee_sym->decl_node;
                    int field_count = od ? od->field_count : 0;

                    if (ce->arg_count != field_count) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "object '%s' has %d field(s), but %d argument(s) given",
                                 callee_id->name, field_count, ce->arg_count);
                        emit_error(ctx, IRON_ERR_ARG_COUNT, ce->span, msg, NULL);
                        for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                    } else if (od) {
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
                        result = iron_type_make_array(ctx->arena, val_t, -1);
                    } else {
                        result = iron_type_make_array(ctx->arena,
                                   iron_type_make_primitive(IRON_TYPE_INT), -1);
                    }
                    ce->resolved_type = result;
                    break;
                }
            }

            /* Check arg count */
            int expected_count = callee_type->func.param_count;
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
                    Iron_Type *arg_type = check_expr(ctx, ce->args[i]);
                    Iron_Type *param_type = callee_type->func.param_types[i];
                    if (param_type && arg_type &&
                        param_type->kind != IRON_TYPE_ERROR &&
                        arg_type->kind   != IRON_TYPE_ERROR &&
                        !types_assignable(param_type, arg_type) &&
                        !is_int_literal_narrowing(param_type, arg_type, ce->args[i])) {
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "argument %d type mismatch: expected '%s', got '%s'",
                                 i + 1,
                                 iron_type_to_string(param_type, ctx->arena),
                                 iron_type_to_string(arg_type, ctx->arena));
                        emit_error(ctx, IRON_ERR_ARG_TYPE, ce->args[i]->span, msg, NULL);
                    }
                    /* Narrow literal args to match parameter type */
                    if (is_int_literal_narrowing(param_type, arg_type, ce->args[i])) {
                        ((Iron_IntLit *)ce->args[i])->resolved_type = param_type;
                    }
                }
            }

            result = callee_type->func.return_type
                     ? callee_type->func.return_type
                     : iron_type_make_primitive(IRON_TYPE_VOID);
            ce->resolved_type = result;
            break;
        }

        case IRON_NODE_METHOD_CALL: {
            Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
            check_expr(ctx, mc->object);
            for (int i = 0; i < mc->arg_count; i++) check_expr(ctx, mc->args[i]);

            /* Try to resolve the return type by finding the matching method decl.
             * This handles both auto-static (Math.sin) and instance method calls. */
            result = iron_type_make_primitive(IRON_TYPE_VOID);
            if (mc->object->kind == IRON_NODE_IDENT) {
                Iron_Ident *obj_id = (Iron_Ident *)mc->object;
                const char *type_name_mc = NULL;
                if (obj_id->resolved_sym &&
                    obj_id->resolved_sym->sym_kind == IRON_SYM_TYPE) {
                    /* Auto-static: receiver is the type itself */
                    type_name_mc = obj_id->name;
                } else if (obj_id->resolved_type &&
                           obj_id->resolved_type->kind == IRON_TYPE_OBJECT) {
                    /* Instance method: receiver has object type */
                    type_name_mc = obj_id->resolved_type->object.decl->name;
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
                            break;
                        }
                    }
                }
            }
            mc->resolved_type = result;
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

            /* Unwrap rc pointer types to access the inner object type.
             * heap types already expose the inner object type directly (IRON_NODE_HEAP
             * sets resolved_type to the inner construct type, not an RC wrapper). */
            if (obj_type->kind == IRON_TYPE_RC) {
                obj_type = obj_type->rc.inner;
            }

            if (obj_type->kind != IRON_TYPE_OBJECT) {
                if (obj_type->kind == IRON_TYPE_NULLABLE) {
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
            for (int i = 0; i < od->field_count; i++) {
                Iron_Field *f = (Iron_Field *)od->fields[i];
                if (strcmp(f->name, fa->field) == 0) {
                    field_type = resolve_type_annotation(ctx, f->type_ann);
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
            break;
        }

        case IRON_NODE_CONSTRUCT: {
            Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;

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
                Iron_ObjectDecl *od = (Iron_ObjectDecl *)sym->decl_node;
                int field_count = od ? od->field_count : 0;

                if (ce->arg_count != field_count) {
                    char msg[256];
                    snprintf(msg, sizeof(msg),
                             "object '%s' has %d field(s), but %d argument(s) given",
                             ce->type_name, field_count, ce->arg_count);
                    emit_error(ctx, IRON_ERR_ARG_COUNT, ce->span, msg, NULL);
                    for (int i = 0; i < ce->arg_count; i++) check_expr(ctx, ce->args[i]);
                } else if (od) {
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
            check_expr(ctx, idx_e->index);
            if (obj_type && obj_type->kind == IRON_TYPE_ARRAY) {
                result = obj_type->array.elem;
            } else {
                result = iron_type_make_primitive(IRON_TYPE_ERROR);
            }
            idx_e->resolved_type = result;
            break;
        }

        case IRON_NODE_SLICE: {
            Iron_SliceExpr *se = (Iron_SliceExpr *)node;
            Iron_Type *obj_type = check_expr(ctx, se->object);
            check_expr(ctx, se->start);
            check_expr(ctx, se->end);
            result = obj_type ? obj_type : iron_type_make_primitive(IRON_TYPE_ERROR);
            se->resolved_type = result;
            break;
        }

        case IRON_NODE_HEAP: {
            Iron_HeapExpr *he = (Iron_HeapExpr *)node;
            result = check_expr(ctx, he->inner);
            he->resolved_type = result;
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
            if (al->size) check_expr(ctx, al->size);
            Iron_Type *elem_type = NULL;
            for (int i = 0; i < al->element_count; i++) {
                Iron_Type *et = check_expr(ctx, al->elements[i]);
                if (!elem_type && et) elem_type = et;
            }
            if (!elem_type) elem_type = iron_type_make_primitive(IRON_TYPE_ERROR);
            result = iron_type_make_array(ctx->arena, elem_type, -1);
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
                memset(inferred_args, 0,
                    sizeof(Iron_Type *) * (size_t)ed->generic_param_count);

                /* Push generic param scope for resolving payload type annotations */
                Iron_Scope *saved_gen = ctx->global_scope;
                Iron_Scope *gen_scope = iron_scope_create(ctx->arena,
                    ctx->global_scope, IRON_SCOPE_BLOCK);
                for (int gi = 0; gi < ed->generic_param_count; gi++) {
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
                            Iron_Ident *param = (Iron_Ident *)ed->generic_params[gi];
                            if (param && expected->generic_param.name &&
                                strcmp(param->name, expected->generic_param.name) == 0) {
                                inferred_args[gi] = arg_t;
                                break;
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
                iron_strbuf_free(&sb);

                /* Create monomorphized type */
                Iron_Type *mono = iron_arena_alloc(ctx->arena, sizeof(Iron_Type),
                    _Alignof(Iron_Type));
                memset(mono, 0, sizeof(*mono));
                mono->kind = IRON_TYPE_ENUM;
                mono->enu.decl = ed;
                mono->enu.type_args = inferred_args;
                mono->enu.type_arg_count = ed->generic_param_count;
                mono->enu.mangled_name = mangled;

                /* Substitute variant_payload_types */
                Iron_Type ***vpt = iron_arena_alloc(ctx->arena,
                    sizeof(Iron_Type **) * (size_t)ed->variant_count,
                    _Alignof(Iron_Type **));
                memset(vpt, 0, sizeof(Iron_Type **) * (size_t)ed->variant_count);
                Iron_Scope *saved_gen2 = ctx->global_scope;
                Iron_Scope *gen2 = iron_scope_create(ctx->arena,
                    ctx->global_scope, IRON_SCOPE_BLOCK);
                for (int gi = 0; gi < ed->generic_param_count; gi++) {
                    Iron_Ident *param = (Iron_Ident *)ed->generic_params[gi];
                    if (param) {
                        Iron_Type *gpt = iron_type_make_generic_param(
                            ctx->arena, param->name, NULL);
                        Iron_Symbol *gsym = iron_symbol_create(ctx->arena,
                            param->name, IRON_SYM_TYPE, NULL,
                            (Iron_Span){0, 0, 0, 0, 0});
                        gsym->type = gpt;
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
                    for (int kk = 0; kk < vev->payload_count; kk++) {
                        Iron_Type *pt = resolve_type_annotation(
                            ctx, vev->payload_type_anns[kk]);
                        row[kk] = substitute_generic_type(ctx, pt, ed,
                            inferred_args, ed->generic_param_count);
                    }
                    vpt[vj] = row;
                }
                ctx->global_scope = saved_gen2;
                mono->enu.variant_payload_types = vpt;

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

        default:
            result = iron_type_make_primitive(IRON_TYPE_ERROR);
            break;
    }

    if (!result) result = iron_type_make_primitive(IRON_TYPE_ERROR);
    return result;
}

/* ── Statement type checking ─────────────────────────────────────────────── */

static void check_stmt(TypeCtx *ctx, Iron_Node *node) {
    if (!node) return;

    switch (node->kind) {
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            tc_push_scope(ctx, IRON_SCOPE_BLOCK);
            check_block_stmts(ctx, b->stmts, b->stmt_count);
            tc_pop_scope(ctx);
            break;
        }

        case IRON_NODE_VAL_DECL: {
            Iron_ValDecl *vd = (Iron_ValDecl *)node;
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
                if (init_type->kind != IRON_TYPE_ERROR &&
                    decl_type->kind != IRON_TYPE_ERROR &&
                    !types_assignable(decl_type, init_type) &&
                    !is_int_literal_narrowing(decl_type, init_type, vd->init)) {
                    emit_type_mismatch(ctx, vd->span, decl_type, init_type);
                }
                /* Narrow literal type to match declaration (e.g., Int literal -> Int32) */
                if (is_int_literal_narrowing(decl_type, init_type, vd->init)) {
                    ((Iron_IntLit *)vd->init)->resolved_type = decl_type;
                }
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
                if (init_type->kind != IRON_TYPE_ERROR &&
                    decl_type->kind != IRON_TYPE_ERROR &&
                    !types_assignable(decl_type, init_type) &&
                    !is_int_literal_narrowing(decl_type, init_type, vd->init)) {
                    emit_type_mismatch(ctx, vd->span, decl_type, init_type);
                }
                /* Narrow literal type to match declaration (e.g., Int literal -> Int32) */
                if (is_int_literal_narrowing(decl_type, init_type, vd->init)) {
                    ((Iron_IntLit *)vd->init)->resolved_type = decl_type;
                }
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

            if (as->target && as->target->kind == IRON_NODE_IDENT) {
                Iron_Ident *tid = (Iron_Ident *)as->target;
                target_name = tid->name;

                /* Check type-checker scope first */
                Iron_Symbol *tc_sym = tc_lookup(ctx, target_name);
                if (tc_sym) {
                    is_immutable = !tc_sym->is_mutable;
                } else if (tid->resolved_sym) {
                    /* Fall back to resolver's symbol */
                    is_immutable = !tid->resolved_sym->is_mutable;
                }
            }

            Iron_Type *target_type = check_expr(ctx, as->target);
            Iron_Type *value_type  = check_expr(ctx, as->value);

            if (is_immutable) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "cannot assign to val '%s' — val is immutable",
                         target_name ? target_name : "");
                emit_error(ctx, IRON_ERR_VAL_REASSIGN, as->span, msg, NULL);
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
            break;
        }

        case IRON_NODE_RETURN: {
            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
            Iron_Type *ret_type = NULL;

            if (rs->value) {
                ret_type = check_expr(ctx, rs->value);
            } else {
                ret_type = iron_type_make_primitive(IRON_TYPE_VOID);
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
                        char msg[256];
                        snprintf(msg, sizeof(msg),
                                 "return type mismatch: function returns '%s', got '%s'",
                                 iron_type_to_string(ctx->current_return_type, ctx->arena),
                                 iron_type_to_string(ret_type, ctx->arena));
                        emit_error(ctx, IRON_ERR_RETURN_TYPE, rs->span, msg, NULL);
                    }
                    /* Narrow literal in return (e.g., return 42 in Int32 func) */
                    if (is_int_literal_narrowing(ctx->current_return_type, ret_type, rs->value)) {
                        ((Iron_IntLit *)rs->value)->resolved_type = ctx->current_return_type;
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
                if (is_s->body) check_stmt(ctx, is_s->body);
                for (int i = 0; i < is_s->elif_count; i++) {
                    check_expr(ctx, is_s->elif_conds[i]);
                    if (is_s->elif_bodies[i]) check_stmt(ctx, is_s->elif_bodies[i]);
                }
                if (is_s->else_body) check_stmt(ctx, is_s->else_body);
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
            if (ws->body) check_stmt(ctx, ws->body);
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
            if (fs->body) check_stmt(ctx, fs->body);
            tc_pop_scope(ctx);
            break;
        }

        case IRON_NODE_MATCH: {
            Iron_MatchStmt *ms = (Iron_MatchStmt *)node;
            Iron_Type *subject_type = check_expr(ctx, ms->subject);
            /* Process each case arm */
            for (int i = 0; i < ms->case_count; i++) {
                if (ms->cases[i]) check_stmt(ctx, ms->cases[i]);
            }
            if (ms->else_body) check_stmt(ctx, ms->else_body);
            /* Exhaustiveness check — only for enum subjects with payloads */
            if (subject_type && subject_type->kind == IRON_TYPE_ENUM) {
                Iron_EnumDecl *ed = subject_type->enu.decl;
                if (ed && ed->has_payloads) {
                    bool *covered = iron_arena_alloc(ctx->arena,
                        sizeof(bool) * (size_t)ed->variant_count, _Alignof(bool));
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
                            iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_NOTE, 0,
                                           ms->span,
                                           iron_arena_strdup(ctx->arena, msg, strlen(msg)),
                                           NULL);
                        }
                    }
                }
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
            break;
        }

        case IRON_NODE_FREE: {
            Iron_FreeStmt *frs = (Iron_FreeStmt *)node;
            check_expr(ctx, frs->expr);
            break;
        }

        case IRON_NODE_LEAK: {
            Iron_LeakStmt *ls = (Iron_LeakStmt *)node;
            check_expr(ctx, ls->expr);
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
                Iron_Block *blk = (Iron_Block *)ss->body;
                if (blk) {
                    for (int i = 0; i < blk->stmt_count; i++) {
                        if (blk->stmts[i]->kind == IRON_NODE_RETURN) {
                            Iron_ReturnStmt *rs = (Iron_ReturnStmt *)blk->stmts[i];
                            if (rs->value) {
                                /* All expr nodes share the layout:
                                 * { span, kind, resolved_type, ... } */
                                Iron_IntLit *expr_node = (Iron_IntLit *)rs->value;
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

        default:
            /* Expression used as statement */
            check_expr(ctx, node);
            break;
    }
}

static void check_block_stmts(TypeCtx *ctx, Iron_Node **stmts, int count) {
    for (int i = 0; i < count; i++) {
        check_stmt(ctx, stmts[i]);
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

    /* Resolve param types */
    Iron_Type **param_types = NULL;
    if (fd->param_count > 0) {
        param_types = (Iron_Type **)iron_arena_alloc(
            ctx->arena, (size_t)fd->param_count * sizeof(Iron_Type *),
            _Alignof(Iron_Type *));
    }
    for (int i = 0; i < fd->param_count; i++) {
        Iron_Param *p = (Iron_Param *)fd->params[i];
        param_types[i] = resolve_type_annotation(ctx, p->type_ann);
    }

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

    tc_pop_scope(ctx);
    ctx->current_return_type = prev_ret;
}

static void check_method_decl(TypeCtx *ctx, Iron_MethodDecl *md) {
    /* Resolve return type */
    Iron_Type *ret_type = NULL;
    if (md->return_type) {
        ret_type = resolve_type_annotation(ctx, md->return_type);
    } else {
        ret_type = iron_type_make_primitive(IRON_TYPE_VOID);
    }
    md->resolved_return_type = ret_type;

    /* Resolve param types */
    Iron_Type **param_types = NULL;
    if (md->param_count > 0) {
        param_types = (Iron_Type **)iron_arena_alloc(
            ctx->arena, (size_t)md->param_count * sizeof(Iron_Type *),
            _Alignof(Iron_Type *));
    }
    for (int i = 0; i < md->param_count; i++) {
        Iron_Param *p = (Iron_Param *)md->params[i];
        param_types[i] = resolve_type_annotation(ctx, p->type_ann);
    }

    Iron_Type *prev_ret = ctx->current_return_type;
    const char *prev_type_name = ctx->current_method_type;
    ctx->current_return_type   = (ret_type->kind != IRON_TYPE_VOID) ? ret_type : NULL;
    ctx->current_method_type   = md->type_name;

    tc_push_scope(ctx, IRON_SCOPE_FUNCTION);

    /* Define 'self' */
    if (md->owner_sym) {
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

    ctx->current_return_type = prev_ret;
    ctx->current_method_type = prev_type_name;
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

/* ── Entry point ─────────────────────────────────────────────────────────── */

void iron_typecheck(Iron_Program *program, Iron_Scope *global_scope,
                    Iron_Arena *arena, Iron_DiagList *diags) {
    if (!program || !global_scope) return;

    TypeCtx ctx;
    ctx.arena               = arena;
    ctx.diags               = diags;
    ctx.global_scope        = global_scope;
    ctx.current_scope       = global_scope;
    ctx.current_return_type = NULL;
    ctx.current_method_type = NULL;
    ctx.narrowed            = NULL;
    ctx.program             = program;
    ctx.spawn_result_types  = NULL;
    sh_new_strdup(ctx.narrowed);
    sh_new_strdup(ctx.spawn_result_types);

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
        memset(vpt, 0, sizeof(Iron_Type **) * (size_t)ed->variant_count);
        ty->enu.variant_payload_types = vpt;
        for (int j = 0; j < ed->variant_count; j++) {
            Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
            if (ev->payload_count == 0) {
                vpt[j] = NULL;
                continue;
            }
            Iron_Type **row = iron_arena_alloc(ctx.arena,
                sizeof(Iron_Type *) * (size_t)ev->payload_count, _Alignof(Iron_Type *));
            for (int k = 0; k < ev->payload_count; k++) {
                row[k] = resolve_type_annotation(&ctx, ev->payload_type_anns[k]);
            }
            vpt[j] = row;
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
            Iron_Type *ret_type = md->return_type
                ? resolve_type_annotation(&ctx, md->return_type)
                : iron_type_make_primitive(IRON_TYPE_VOID);
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
                    for (int j = 0; j < pc; j++) {
                        Iron_Param *p = (Iron_Param *)md->params[j];
                        param_types[j] = resolve_type_annotation(&ctx, p->type_ann);
                    }
                }
                sym->type = iron_type_make_func(ctx.arena, param_types, pc, ret_type);
            }
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

    /* Interface completeness */
    check_interface_completeness(&ctx, program);

    shfree(ctx.narrowed);
    shfree(ctx.spawn_result_types);
}
