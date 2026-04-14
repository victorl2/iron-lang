/* emit_helpers.c -- Shared helper implementations for the Iron C emitter.
 *
 * Contains name mangling, type mapping, emit utilities, value helpers,
 * and the consolidated emit_ctx_cleanup() function.
 *
 * These were extracted from emit_c.c to form the foundation layer that
 * all emitter sub-modules depend on.
 */

#include "lir/emit_helpers.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

/* ── Name mangling helpers ────────────────────────────────────────────────── */

const char *emit_mangle_name(const char *name, Iron_Arena *arena) {
    size_t len   = strlen(name);
    size_t total = 5 + len + 1;
    char  *buf   = (char *)iron_arena_alloc(arena, total, 1);
    if (!buf) iron_oom_abort("emit_helpers.c:emit_mangle_name");
    memcpy(buf, "Iron_", 5);
    memcpy(buf + 5, name, len + 1);
    return buf;
}

const char *emit_object_type_name(const char *name, EmitCtx *ctx) {
    if (ctx->module) {
        for (int ei = 0; ei < ctx->module->extern_decl_count; ei++) {
            IronLIR_ExternDecl *ed = ctx->module->extern_decls[ei];
            for (int pi = 0; pi < ed->param_count; pi++) {
                if (ed->param_types[pi] &&
                    ed->param_types[pi]->kind == IRON_TYPE_OBJECT &&
                    ed->param_types[pi]->object.decl &&
                    strcmp(ed->param_types[pi]->object.decl->name, name) == 0)
                    return name;
            }
            if (ed->return_type &&
                ed->return_type->kind == IRON_TYPE_OBJECT &&
                ed->return_type->object.decl &&
                strcmp(ed->return_type->object.decl->name, name) == 0)
                return name;
        }
    }
    return emit_mangle_name(name, ctx->arena);
}

/* Map an Iron function name to the C symbol name.
 * - Lifted functions (lambda_, spawn_, parallel_) are kept as-is.
 * - Built-in function names (println, print, len, etc.) map to Iron_XXX.
 * - All other user-defined functions map to Iron_XXX.
 * The returned string is arena-allocated or a static literal. */
const char *emit_mangle_func_name(const char *name, Iron_Arena *arena) {
    if (!name) return "NULL";

    /* Lifted functions are already internal C identifiers -- keep as-is.
     * Lifted names start with __ (e.g. __pfor_0, __spawn_task1_0, __lambda_0)
     * or with the prefix directly (lambda_, spawn_, parallel_) for legacy names. */
    if (strncmp(name, "__", 2) == 0) return name;  /* internal lifted names */
    if (strncmp(name, "lambda_", 7)   == 0 ||
        strncmp(name, "spawn_",   6)  == 0 ||
        strncmp(name, "parallel_", 9) == 0) {
        return name;
    }

    /* Already mangled (shouldn't normally happen, but guard anyway) */
    if (strncmp(name, "Iron_", 5) == 0) return name;

    /* All other names: apply Iron_ prefix */
    return emit_mangle_name(name, arena);
}

/* Resolve a function IR name to its C symbol, honoring extern_c_name.
 * Looks up the function in the module; if found and is_extern, uses extern_c_name.
 * Otherwise falls back to emit_mangle_func_name(). */
const char *emit_resolve_func_c_name(EmitCtx *ctx, const char *ir_name) {
    if (!ir_name) return "NULL";
    for (int fi = 0; fi < ctx->module->func_count; fi++) {
        IronLIR_Func *f = ctx->module->funcs[fi];
        if (strcmp(f->name, ir_name) == 0 && f->is_extern) {
            if (f->extern_c_name) return f->extern_c_name;
            /* No explicit extern_c_name -- fall through to emit_mangle_func_name
             * (handles empty-body stubs that are marked extern internally) */
            break;
        }
    }
    return emit_mangle_func_name(ir_name, ctx->arena);
}

/* Sanitize a block label for use as a C identifier: replace dots with underscores. */
const char *emit_sanitize_label(const char *label, Iron_Arena *arena) {
    if (!label) return "unknown_block";
    /* Check if any dot exists; if not, return label unchanged */
    const char *p = label;
    while (*p) {
        if (*p == '.') break;
        p++;
    }
    if (!*p) return label; /* no dots, fast path */

    size_t len = strlen(label);
    char *buf = (char *)iron_arena_alloc(arena, len + 1, 1);
    if (!buf) iron_oom_abort("emit_helpers.c:emit_sanitize_label");
    for (size_t i = 0; i <= len; i++) {
        buf[i] = (label[i] == '.') ? '_' : label[i];
    }
    return buf;
}

/* ── Type-to-C mapping ───────────────────────────────────────────────────── */

const char *emit_optional_struct_name(const Iron_Type *inner,
                                       EmitCtx *ctx) {
    const char *c_inner = emit_type_to_c(inner, ctx);
    Iron_StrBuf sb = iron_strbuf_create(64);
    iron_strbuf_appendf(&sb, "Iron_Optional_");
    for (const char *p = c_inner; *p; p++) {
        if (*p == ' ' || *p == '*' || *p == '[' || *p == ']') {
            iron_strbuf_appendf(&sb, "_");
        } else {
            char ch[2] = { *p, '\0' };
            iron_strbuf_appendf(&sb, "%s", ch);
        }
    }
    const char *result = iron_arena_strdup(ctx->arena, iron_strbuf_get(&sb),
                                           sb.len);
    if (!result) iron_oom_abort("emit_helpers.c:emit_optional_struct_name");
    iron_strbuf_free(&sb);
    return result;
}

const char *emit_type_to_c(const Iron_Type *t, EmitCtx *ctx) {
    if (!t) return "void";

    switch (t->kind) {
        case IRON_TYPE_INT:     return "int64_t";
        case IRON_TYPE_INT8:    return "int8_t";
        case IRON_TYPE_INT16:   return "int16_t";
        case IRON_TYPE_INT32:   return "int32_t";
        case IRON_TYPE_INT64:   return "int64_t";
        case IRON_TYPE_UINT:    return "uint64_t";
        case IRON_TYPE_UINT8:   return "uint8_t";
        case IRON_TYPE_UINT16:  return "uint16_t";
        case IRON_TYPE_UINT32:  return "uint32_t";
        case IRON_TYPE_UINT64:  return "uint64_t";
        case IRON_TYPE_FLOAT:   return "double";
        case IRON_TYPE_FLOAT32: return "float";
        case IRON_TYPE_FLOAT64: return "double";
        case IRON_TYPE_BOOL:    return "bool";
        case IRON_TYPE_STRING:  return "Iron_String";
        case IRON_TYPE_VOID:    return "void";
        case IRON_TYPE_NULL:    return "void*";
        case IRON_TYPE_ERROR:   return "int";

        case IRON_TYPE_OBJECT:
            return emit_object_type_name(t->object.decl->name, ctx);

        case IRON_TYPE_ENUM:
            if (t->enu.mangled_name) {
                return t->enu.mangled_name; /* already "Iron_Option_Int" */
            }
            return emit_mangle_name(t->enu.decl->name, ctx->arena);

        case IRON_TYPE_INTERFACE:
            /* Tagged union struct -- same mangled name as the interface */
            return emit_mangle_name(t->interface.decl->name, ctx->arena);

        case IRON_TYPE_NULLABLE: {
            emit_ensure_optional(ctx, t->nullable.inner);
            return emit_optional_struct_name(t->nullable.inner, ctx);
        }

        case IRON_TYPE_RC: {
            const char *inner_c = emit_type_to_c(t->rc.inner, ctx);
            Iron_StrBuf sb = iron_strbuf_create(64);
            iron_strbuf_appendf(&sb, "%s*", inner_c);
            const char *result = iron_arena_strdup(ctx->arena,
                                                    iron_strbuf_get(&sb),
                                                    sb.len);
            if (!result) iron_oom_abort("emit_helpers.c:emit_type_to_c RC");
            iron_strbuf_free(&sb);
            return result;
        }

        case IRON_TYPE_FUNC:
            return "Iron_Closure";

        case IRON_TYPE_ARRAY: {
            /* Arrays are represented as Iron_List_<elem_c_type> in C.
             * e.g. [Int] -> Iron_List_int64_t, [Float] -> Iron_List_double
             * Phase 53: Interface-typed arrays use Iron_SplitList_<Iface> since
             * they are always emitted as split collections in the emitter.
             * e.g. [Shape] -> Iron_SplitList_Iron_Shape */
            const char *elem_c = emit_type_to_c(t->array.elem, ctx);
            Iron_StrBuf sb = iron_strbuf_create(64);
            bool is_iface_elem = t->array.elem &&
                                 t->array.elem->kind == IRON_TYPE_INTERFACE &&
                                 t->array.elem->interface.decl &&
                                 ctx->iface_reg;
            iron_strbuf_appendf(&sb, "%s",
                                is_iface_elem ? "Iron_SplitList_" : "Iron_List_");
            for (const char *p = elem_c; *p; p++) {
                if (*p == ' ' || *p == '*') {
                    iron_strbuf_appendf(&sb, "_");
                } else {
                    char ch[2] = { *p, '\0' };
                    iron_strbuf_appendf(&sb, "%s", ch);
                }
            }
            const char *result = iron_arena_strdup(ctx->arena,
                                                    iron_strbuf_get(&sb), sb.len);
            if (!result) iron_oom_abort("emit_helpers.c:emit_type_to_c ARRAY");
            iron_strbuf_free(&sb);
            return result;
        }

        case IRON_TYPE_GENERIC_PARAM:
            return "void*";

        case IRON_TYPE_TUPLE:
            /* Phase 59 01d: ensure the tuple typedef is emitted and return
             * its mangled struct name. Recurses through emit_ensure_tuple
             * so nested tuples get their inner typedefs first. */
            emit_ensure_tuple(ctx, t);
            return t->tuple.mangled_name ? t->tuple.mangled_name : "void";
    }
    return "int"; /* unreachable fallback */
}

void emit_ensure_optional(EmitCtx *ctx, const Iron_Type *inner) {
    const char *struct_name = emit_optional_struct_name(inner, ctx);

    for (int i = 0; i < (int)arrlen(ctx->emitted_optionals); i++) {
        if (strcmp(ctx->emitted_optionals[i], struct_name) == 0) return;
    }

    char *struct_name_copy = iron_arena_strdup(ctx->arena, struct_name, strlen(struct_name));
    if (!struct_name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_optional struct_name");
    arrput(ctx->emitted_optionals, struct_name_copy);

    const char *c_inner = emit_type_to_c(inner, ctx);
    iron_strbuf_appendf(&ctx->struct_bodies,
                         "typedef struct { %s value; bool has_value; } %s;\n",
                         c_inner, struct_name);
}

/* Phase 59 01d: synthesise a C typedef for a tuple on demand.
 *
 *   typedef struct { T0 v0; T1 v1; ... Tn vN; } Iron_Tuple_<mangled>;
 *
 * Dedupes via ctx->emitted_tuples so the same mangled name is only
 * emitted once across the whole translation unit. Recurses into
 * element types so nested tuples (e.g. (Int, (String, Bool))) get
 * their inner typedefs ensured first. No-op when the type isn't
 * a tuple. */
void emit_ensure_tuple(EmitCtx *ctx, const Iron_Type *tuple_ty) {
    if (!tuple_ty || tuple_ty->kind != IRON_TYPE_TUPLE) return;
    const char *struct_name = tuple_ty->tuple.mangled_name;
    if (!struct_name) return;

    /* Dedupe */
    for (int i = 0; i < (int)arrlen(ctx->emitted_tuples); i++) {
        if (strcmp(ctx->emitted_tuples[i], struct_name) == 0) return;
    }

    /* Register BEFORE recursing / appending so a recursive tuple that
     * somehow references itself (not currently possible in the type
     * system, but cheap defense) breaks via the dedupe check. */
    char *struct_name_copy = iron_arena_strdup(ctx->arena, struct_name, strlen(struct_name));
    if (!struct_name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_tuple struct_name");
    arrput(ctx->emitted_tuples, struct_name_copy);

    /* Recurse into nested tuple element types so their typedefs land
     * in struct_bodies FIRST. Non-tuple element typedefs are ensured
     * lazily via the emit_type_to_c calls below (which may in turn
     * trigger emit_ensure_tuple for deeper nesting). */
    for (int i = 0; i < tuple_ty->tuple.elem_count; i++) {
        const Iron_Type *elem = tuple_ty->tuple.elem_types[i];
        if (elem && elem->kind == IRON_TYPE_TUPLE) {
            emit_ensure_tuple(ctx, elem);
        }
    }

    iron_strbuf_appendf(&ctx->struct_bodies, "typedef struct { ");
    for (int i = 0; i < tuple_ty->tuple.elem_count; i++) {
        const char *c_elem = emit_type_to_c(tuple_ty->tuple.elem_types[i], ctx);
        iron_strbuf_appendf(&ctx->struct_bodies, "%s v%d; ", c_elem, i);
    }
    iron_strbuf_appendf(&ctx->struct_bodies, "} %s;\n", struct_name);
}

/* Map a type annotation name to a C type string without needing Iron_Codegen */
const char *emit_annotation_to_c(const char *name, EmitCtx *ctx) {
    if (strcmp(name, "Int") == 0)     return "int64_t";
    if (strcmp(name, "Int8") == 0)    return "int8_t";
    if (strcmp(name, "Int16") == 0)   return "int16_t";
    if (strcmp(name, "Int32") == 0)   return "int32_t";
    if (strcmp(name, "Int64") == 0)   return "int64_t";
    if (strcmp(name, "UInt") == 0)    return "uint64_t";
    if (strcmp(name, "UInt8") == 0)   return "uint8_t";
    if (strcmp(name, "UInt16") == 0)  return "uint16_t";
    if (strcmp(name, "UInt32") == 0)  return "uint32_t";
    if (strcmp(name, "UInt64") == 0)  return "uint64_t";
    if (strcmp(name, "Float") == 0)   return "double";
    if (strcmp(name, "Float32") == 0) return "float";
    if (strcmp(name, "Float64") == 0) return "double";
    if (strcmp(name, "Bool") == 0)    return "bool";
    if (strcmp(name, "String") == 0)  return "Iron_String";
    return emit_mangle_name(name, ctx->arena);
}

/* ── Array parameter mode helpers ────────────────────────────────────────── */

/* Look up the ArrayParamMode for a given function + param index. */
ArrayParamMode emit_get_array_param_mode(EmitCtx *ctx, const char *func_name,
                                          int param_index) {
    return iron_lir_get_array_param_mode(ctx->opt_info, func_name, param_index,
                                         ctx->arena);
}

/* Find an IronLIR_Func in the module by IR name. */
IronLIR_Func *emit_find_ir_func(EmitCtx *ctx, const char *ir_name) {
    if (!ir_name) return NULL;
    for (int i = 0; i < ctx->module->func_count; i++) {
        if (strcmp(ctx->module->funcs[i]->name, ir_name) == 0)
            return ctx->module->funcs[i];
    }
    return NULL;
}

/* ── Block label resolution ──────────────────────────────────────────────── */

/* Build a unique C label for a block: "<sanitized_label>_b<id>".
 * This avoids duplicate-label errors when nested control flow reuses
 * the same label string (e.g., multiple "if_merge" blocks in one function). */
const char *emit_make_block_label(IronLIR_BlockId id, const char *raw_label,
                                   Iron_Arena *arena) {
    /* Sanitize dots first */
    const char *san = emit_sanitize_label(raw_label, arena);
    /* Allocate "label_b<id>\0" */
    size_t san_len = strlen(san);
    /* Max digits for a 32-bit int = 10 + "b" prefix + "_" + NUL = 14 extra */
    char *buf = (char *)iron_arena_alloc(arena, san_len + 16, 1);
    if (!buf) iron_oom_abort("emit_helpers.c:emit_make_block_label");
    snprintf(buf, san_len + 16, "%s_b%d", san, (int)id);
    return buf;
}

const char *emit_resolve_label(IronLIR_Func *fn, IronLIR_BlockId id,
                                Iron_Arena *arena) {
    for (int i = 0; i < fn->block_count; i++) {
        if (fn->blocks[i]->id == id) {
            return emit_make_block_label(id, fn->blocks[i]->label, arena);
        }
    }
    return "unknown_block";
}

/* ── Instruction emission utilities ──────────────────────────────────────── */

void emit_indent(Iron_StrBuf *sb, int level) {
    for (int i = 0; i < level * 4; i++) {
        iron_strbuf_append(sb, " ", 1);
    }
}

/* Emit the C name for a value: _v{id} */
void emit_val(Iron_StrBuf *sb, IronLIR_ValueId id) {
    iron_strbuf_appendf(sb, "_v%u", id);
}

/* ── Value helpers ───────────────────────────────────────────────────────── */

/* Determine whether the target object type should use -> (pointer) or . */
bool emit_type_is_pointer(const Iron_Type *t) {
    if (!t) return false;
    if (t->kind == IRON_TYPE_RC) return true;
    if (t->kind == IRON_TYPE_NULLABLE) {
        /* nullable pointers */
        return false;
    }
    return false;
}

/* Determine if a LIR value represents a heap/rc pointer -- used for GET_FIELD / SET_FIELD
 * to decide whether to emit `->` or `.`.
 *
 * Returns true when:
 *   - The producing instruction is HEAP_ALLOC or RC_ALLOC (direct heap pointer), OR
 *   - The producing instruction is LOAD, and the alloca it reads holds a pointer (its
 *     alloc_type is IRON_TYPE_RC, which hir_to_lir.c sets when the init is heap/rc).
 */
bool emit_val_is_heap_ptr(IronLIR_Func *fn, IronLIR_ValueId vid) {
    if (vid == IRON_LIR_VALUE_INVALID) return false;
    if (vid >= (IronLIR_ValueId)arrlen(fn->value_table)) return false;
    IronLIR_Instr *instr = fn->value_table[vid];
    if (!instr) return false;
    if (instr->kind == IRON_LIR_HEAP_ALLOC || instr->kind == IRON_LIR_RC_ALLOC)
        return true;
    /* LOAD from an RC-typed alloca: the alloca was declared as T* (via RC wrapper).
     * hir_to_lir.c sets the alloca type to IRON_TYPE_RC when the init is heap/rc. */
    if (instr->kind == IRON_LIR_LOAD) {
        IronLIR_ValueId ptr = instr->load.ptr;
        if (ptr != IRON_LIR_VALUE_INVALID &&
            ptr < (IronLIR_ValueId)arrlen(fn->value_table) &&
            fn->value_table[ptr] &&
            fn->value_table[ptr]->kind == IRON_LIR_ALLOCA &&
            fn->value_table[ptr]->alloca.alloc_type &&
            fn->value_table[ptr]->alloca.alloc_type->kind == IRON_TYPE_RC) {
            return true;
        }
    }
    return false;
}

/* Determine if a LIR value is a FUNC_REF used as a type-name namespace
 * (e.g. "Math" in Math.PI, "Log" in Log.DEBUG).  When a field is accessed
 * on such a value the object is not a runtime struct instance but a type
 * name -- emitting "Iron_Math.PI" produces invalid C.  Instead we emit the
 * constant macro name "Iron_Math_PI" so that the corresponding #define in
 * the module header resolves to the correct value. */
bool emit_val_is_type_ref(IronLIR_Func *fn, IronLIR_ValueId vid) {
    if (vid == IRON_LIR_VALUE_INVALID) return false;
    if (vid >= (IronLIR_ValueId)arrlen(fn->value_table)) return false;
    IronLIR_Instr *instr = fn->value_table[vid];
    if (!instr) return false;
    return instr->kind == IRON_LIR_FUNC_REF;
}

/* Return the Iron_Type* for a value ID, including for parameter values (which
 * have NULL entries in value_table).  Parameter value IDs are 1..param_count;
 * fn->params[vid-1].type holds their type.  Returns NULL if unknown. */
Iron_Type *emit_get_value_type(IronLIR_Func *fn, IronLIR_ValueId vid) {
    if (vid == IRON_LIR_VALUE_INVALID) return NULL;
    if (vid < (IronLIR_ValueId)arrlen(fn->value_table) && fn->value_table[vid])
        return fn->value_table[vid]->type;
    /* Parameter value: IDs 1..param_count map to fn->params[vid-1] */
    if (vid >= 1 && vid <= (IronLIR_ValueId)fn->param_count)
        return fn->params[vid - 1].type;
    return NULL;
}

/* ── Cleanup ─────────────────────────────────────────────────────────────── */

void emit_ctx_cleanup(EmitCtx *ctx) {
    /* Free StrBuf sections */
    iron_strbuf_free(&ctx->includes);
    iron_strbuf_free(&ctx->forward_decls);
    iron_strbuf_free(&ctx->struct_bodies);
    iron_strbuf_free(&ctx->enum_defs);
    iron_strbuf_free(&ctx->global_consts);
    iron_strbuf_free(&ctx->prototypes);
    iron_strbuf_free(&ctx->lifted_funcs);
    iron_strbuf_free(&ctx->implementations);
    iron_strbuf_free(&ctx->main_wrapper);

    /* Free stb_ds maps and arrays */
    arrfree(ctx->emitted_optionals);
    arrfree(ctx->emitted_tuples);
    shfree(ctx->mono_registry);
    hmfree(ctx->param_alias_ids);
    hmfree(ctx->split_collection_ids);
    shfree(ctx->indirect_variants);
    hmfree(ctx->layout_overrides);
    hmfree(ctx->unordered_collections);
    iron_layout_free(&ctx->layout);
    shfree(ctx->reduced_storage_types);
    shfree(ctx->soa_types);

    /* Fusion chain cleanup */
    if (ctx->fusion_chains) {
        for (int fci = 0; fci < (int)arrlen(ctx->fusion_chains); fci++) {
            arrfree(ctx->fusion_chains[fci].nodes);
        }
        arrfree(ctx->fusion_chains);
    }
    hmfree(ctx->fusion_chain_member);
    hmfree(ctx->fusion_chain_position);
    hmfree(ctx->monomorphic_collections);
    shfree(ctx->specialization_registry);
    iron_vr_free(&ctx->value_range);

    /* Per-function residuals (may already be freed, but safe to call on NULL) */
    hmfree(ctx->adt_boxed_allocas);
}
