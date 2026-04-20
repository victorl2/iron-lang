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
#include <errno.h>

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
    MonoRegistryEntry *mono_registry;        /* stb_ds map: mangled_name -> mono Iron_Type* (cycle detection + caching) */
} TypeCtx;

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
    if (!msg_copy) iron_oom_abort("typecheck.c:emit_error msg");
    const char *sug_copy = NULL;
    if (suggestion) {
        sug_copy = iron_arena_strdup(ctx->arena, suggestion, strlen(suggestion));
        if (!sug_copy) iron_oom_abort("typecheck.c:emit_error suggestion");
    }
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_ERROR, code, span,
                   msg_copy, sug_copy);
}

static void emit_warning(TypeCtx *ctx, int code, Iron_Span span,
                         const char *msg, const char *suggestion) {
    const char *msg_copy = iron_arena_strdup(ctx->arena, msg, strlen(msg));
    if (!msg_copy) iron_oom_abort("typecheck.c:emit_warning msg");
    const char *sug_copy = NULL;
    if (suggestion) {
        sug_copy = iron_arena_strdup(ctx->arena, suggestion, strlen(suggestion));
        if (!sug_copy) iron_oom_abort("typecheck.c:emit_warning suggestion");
    }
    iron_diag_emit(ctx->diags, ctx->arena, IRON_DIAG_WARNING, code, span,
                   msg_copy, sug_copy);
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

    if (ann_node->kind != IRON_NODE_TYPE_ANNOTATION) {
        return iron_type_make_primitive(IRON_TYPE_ERROR);
    }

    Iron_TypeAnnotation *ann = (Iron_TypeAnnotation *)ann_node;
    const char *name = ann->name;
    Iron_Type *base = NULL;

    /* Phase 59 01d: tuple-type annotation — (T0, T1, ...) */
    if (ann->is_tuple) {
        int n = ann->tuple_elem_count;
        Iron_Type **elem_types = (Iron_Type **)iron_arena_alloc(
            ctx->arena, sizeof(Iron_Type *) * (size_t)n,
            _Alignof(Iron_Type *));
        if (!elem_types) iron_oom_abort("typecheck.c:resolve_type_annotation tuple_elems");
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
            if (!param_types) iron_oom_abort("typecheck.c:resolve_type_annotation func_params");
            for (int i = 0; i < param_count; i++) {
                param_types[i] = resolve_type_annotation(ctx, ann->func_params[i]);
            }
        }

        /* Resolve return type (NULL means void) */
        Iron_Type *ret = ann->func_return
            ? resolve_type_annotation(ctx, ann->func_return)
            : iron_type_make_primitive(IRON_TYPE_VOID);

        base = iron_type_make_func(ctx->arena, param_types, param_count, ret);

        /* If this is an array-of-func, wrap in array type */
        if (ann->is_array) {
            int size = -1;
            if (ann->array_size && ann->array_size->kind == IRON_NODE_INT_LIT) {
                Iron_IntLit *il = (Iron_IntLit *)ann->array_size;
                if (il->value) size = (int)strtol(il->value, NULL, 10);
            }
            base = iron_type_make_array(ctx->arena, base, size);
            /* Phase 48: propagate layout annotations */
            base->array.layout_hint  = ann->layout_hint;
            base->array.is_unordered = ann->is_unordered;
        }

        if (ann->is_nullable) {
            base = iron_type_make_nullable(ctx->arena, base);
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
                    if (!type_args) iron_oom_abort("typecheck.c:resolve_type_annotation type_args");
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
                    if (!mangled) iron_oom_abort("typecheck.c:resolve_type_annotation mangled");
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
                    if (!mono) iron_oom_abort("typecheck.c:resolve_type_annotation mono");
                    memset(mono, 0, sizeof(*mono));
                    mono->kind = IRON_TYPE_ENUM;
                    mono->enu.decl = ed;
                    mono->enu.type_args = type_args;
                    mono->enu.type_arg_count = ann->generic_arg_count;
                    mono->enu.mangled_name = mangled;

                    /* Register mono BEFORE resolving payloads to break recursive cycles
                     * (e.g. Tree[T] → Branch(Tree[T], Tree[T]) → Tree[T] again). */
                    const char *mono_key = iron_arena_strdup(ctx->arena, mangled, strlen(mangled));
                    if (!mono_key) iron_oom_abort("typecheck.c:resolve_type_annotation mono_key");
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
                    if (!vpt) iron_oom_abort("typecheck.c:resolve_type_annotation vpt");
                    memset(vpt, 0, sizeof(Iron_Type **) * (size_t)ed->variant_count);
                    for (int j = 0; j < ed->variant_count; j++) {
                        Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                        if (ev->payload_count == 0) { vpt[j] = NULL; continue; }
                        Iron_Type **row = iron_arena_alloc(ctx->arena,
                            sizeof(Iron_Type *) * (size_t)ev->payload_count,
                            _Alignof(Iron_Type *));
                        if (!row) iron_oom_abort("typecheck.c:resolve_type_annotation vpt row");
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
                    if (!pib) iron_oom_abort("typecheck.c:resolve_type_annotation pib");
                    memset(pib, 0, sizeof(bool *) * (size_t)ed->variant_count);
                    for (int j = 0; j < ed->variant_count; j++) {
                        Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                        if (ev->payload_count == 0) continue;
                        bool *pib_row = iron_arena_alloc(ctx->arena,
                            sizeof(bool) * (size_t)ev->payload_count, _Alignof(bool));
                        if (!pib_row) iron_oom_abort("typecheck.c:resolve_type_annotation pib row");
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
        /* Phase 48: propagate layout annotations */
        base->array.layout_hint  = ann->layout_hint;
        base->array.is_unordered = ann->is_unordered;
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
                    return iron_type_make_array(ctx->arena, inferred_u, -1);
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
                            }

                            /* Mark as primitive cast for the lowerer */
                            ce->is_primitive_cast = true;
                            result = target_t;
                            ce->resolved_type = result;
                            callee_id->resolved_type = result;
                            break;
                        }
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
                    IRON_NODE_ASSERT_KIND(callee_sym->decl_node, IRON_NODE_OBJECT_DECL);
                    Iron_ObjectDecl *od = (Iron_ObjectDecl *)callee_sym->decl_node;
                    int field_count = od->field_count;

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
                    Iron_Type *param_type = callee_type->func.param_types[i];
                    Iron_Type *arg_type = check_expr_with_expected(ctx, ce->args[i], param_type);
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

            /* Check generic constraints if callee is a generic function */
            if (ce->callee && ce->callee->kind == IRON_NODE_IDENT) {
                Iron_Ident *fn_id = (Iron_Ident *)ce->callee;
                Iron_Symbol *fn_sym = iron_scope_lookup(ctx->global_scope, fn_id->name);
                if (fn_sym && fn_sym->sym_kind == IRON_SYM_FUNCTION && fn_sym->decl_node) {
                    /* PROT-03 row 14 (AUDIT-01 M-severity): IRON_SYM_FUNCTION's
                     * decl_node should always be IRON_NODE_FUNC_DECL; assert it. */
                    IRON_NODE_ASSERT_KIND(fn_sym->decl_node, IRON_NODE_FUNC_DECL);
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
                IRON_NODE_ASSERT_KIND(sym->decl_node, IRON_NODE_OBJECT_DECL);
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
                if (!param_types) iron_oom_abort("typecheck.c:check_expr LAMBDA param_types");
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
                    if (!tup_elems) iron_oom_abort("typecheck.c:check_expr ARRAY_LIT tuple");
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
                if (!inferred_args) iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT inferred_args");
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
                if (!mangled) iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT mangled");
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
                if (!mono) iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT mono");
                memset(mono, 0, sizeof(*mono));
                mono->kind = IRON_TYPE_ENUM;
                mono->enu.decl = ed;
                mono->enu.type_args = inferred_args;
                mono->enu.type_arg_count = ed->generic_param_count;
                mono->enu.mangled_name = mangled;

                /* Register in mono_registry before payload resolution (cycle detection). */
                const char *mono2_key = iron_arena_strdup(ctx->arena, mangled, strlen(mangled));
                if (!mono2_key) iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT mono2_key");
                shput(ctx->mono_registry, mono2_key, mono);

                /* Substitute variant_payload_types:
                 * Bind concrete inferred_args in gen_scope (not GENERIC_PARAMs)
                 * so recursive payload resolution sees Tree[Int] not Tree[T]. */
                Iron_Type ***vpt = iron_arena_alloc(ctx->arena,
                    sizeof(Iron_Type **) * (size_t)ed->variant_count,
                    _Alignof(Iron_Type **));
                if (!vpt) iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT vpt");
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
                    if (!row) iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT vpt row");
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
                if (!pib2) iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT pib2");
                memset(pib2, 0, sizeof(bool *) * (size_t)ed->variant_count);
                for (int vj2 = 0; vj2 < ed->variant_count; vj2++) {
                    Iron_EnumVariant *vev2 = (Iron_EnumVariant *)ed->variants[vj2];
                    if (vev2->payload_count == 0) continue;
                    bool *pib2_row = iron_arena_alloc(ctx->arena,
                        sizeof(bool) * (size_t)vev2->payload_count, _Alignof(bool));
                    if (!pib2_row) iron_oom_abort("typecheck.c:check_expr ENUM_CONSTRUCT pib2 row");
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

    switch ((int)(node->kind)) {
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)node;
            tc_push_scope(ctx, IRON_SCOPE_BLOCK);
            check_block_stmts(ctx, b->stmts, b->stmt_count);
            tc_pop_scope(ctx);
            break;
        }

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
                    if (!msg_copy) iron_oom_abort("typecheck.c:check_stmt VAL_DECL tuple-mismatch msg");
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
            bool is_field_target_immut = false;
            const char *field_root_name = NULL;
            if (as->target && as->target->kind == IRON_NODE_FIELD_ACCESS) {
                Iron_Node *cur = as->target;
                while (cur && cur->kind == IRON_NODE_FIELD_ACCESS) {
                    cur = ((Iron_FieldAccess *)cur)->object;
                }
                if (cur && cur->kind == IRON_NODE_IDENT) {
                    Iron_Ident *root_id = (Iron_Ident *)cur;
                    if (root_id->resolved_sym) {
                        is_field_target_immut = !root_id->resolved_sym->is_mutable;
                        field_root_name = root_id->name;
                    }
                }
            }

            Iron_Type *target_type = check_expr(ctx, as->target);
            Iron_Type *value_type  = check_expr_with_expected(ctx, as->value, target_type);

            if (is_immutable) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "cannot assign to val '%s' — val is immutable",
                         target_name ? target_name : "");
                emit_error(ctx, IRON_ERR_VAL_REASSIGN, as->span, msg, NULL);
            }

            if (is_field_target_immut) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "cannot mutate field on immutable receiver");
                emit_error(ctx, IRON_ERR_MUT_FIELD_IMMUT_RECV, as->span, msg, NULL);
                (void)field_root_name;  /* reserved for future hint; silence unused warn */
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

            if (rs->value) {
                ret_type = check_expr_with_expected(ctx, rs->value, ctx->current_return_type);
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
                    if (!covered) iron_oom_abort("typecheck.c:check_stmt MATCH covered payload");
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
                            if (!note_copy) iron_oom_abort("typecheck.c:check_stmt MATCH else-note");
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
                     * calloc + iron_oom_abort follows the FIX-01 Phase 67-02
                     * pattern so OOM aborts are reportable via stderr grep.
                     * vc is bounded by int so the cast to size_t is safe;
                     * the max(1, vc) guard keeps calloc(0) well-defined. */
                    size_t covered_n = (size_t)(vc > 0 ? vc : 1);
                    bool *covered = (bool *)calloc(covered_n, sizeof(bool));
                    if (!covered) {
                        iron_oom_abort("typecheck.c match-exhaustiveness covered[]");
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
        if (!param_types) iron_oom_abort("typecheck.c:check_func_decl param_types");
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
        if (!param_types) iron_oom_abort("typecheck.c:check_method_decl param_types");
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

            /* PROT-03 row 21 (AUDIT-01 M-severity): iface_sym->decl_node may
             * be NULL or a non-INTERFACE_DECL in error-recovery paths; guard
             * then assert kind before the cast so any wrong-kind decl_node
             * aborts in Debug instead of misreading the interface's vtable. */
            if (!iface_sym->decl_node ||
                iface_sym->decl_node->kind != IRON_NODE_INTERFACE_DECL) continue;
            IRON_NODE_ASSERT_KIND(iface_sym->decl_node, IRON_NODE_INTERFACE_DECL);
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
    ctx.mono_registry       = NULL;
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
        if (!vpt) iron_oom_abort("typecheck.c:iron_typecheck enum vpt");
        memset(vpt, 0, sizeof(Iron_Type **) * (size_t)ed->variant_count);
        ty->enu.variant_payload_types = vpt;
        /* Allocate payload_is_boxed parallel structure on the type */
        bool **pib_ty = iron_arena_alloc(ctx.arena,
            sizeof(bool *) * (size_t)ed->variant_count, _Alignof(bool *));
        if (!pib_ty) iron_oom_abort("typecheck.c:iron_typecheck enum pib_ty");
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
            if (!row) iron_oom_abort("typecheck.c:iron_typecheck enum vpt row");
            /* Allocate boxing flags for this variant */
            ev->payload_is_boxed = iron_arena_alloc(ctx.arena,
                sizeof(bool) * (size_t)ev->payload_count, _Alignof(bool));
            if (!ev->payload_is_boxed) iron_oom_abort("typecheck.c:iron_typecheck enum ev payload_is_boxed");
            memset(ev->payload_is_boxed, 0, sizeof(bool) * (size_t)ev->payload_count);
            bool *pib_row = iron_arena_alloc(ctx.arena,
                sizeof(bool) * (size_t)ev->payload_count, _Alignof(bool));
            if (!pib_row) iron_oom_abort("typecheck.c:iron_typecheck enum pib_row");
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
                if (!param_types) iron_oom_abort("typecheck.c:iron_typecheck FUNC_DECL param_types");
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
                    if (!param_types) iron_oom_abort("typecheck.c:iron_typecheck METHOD_DECL param_types");
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
