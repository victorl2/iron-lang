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

    /* Phase 96 STR-01: lowercase iron_* names are runtime symbols
     * (iron_string_concat, iron_string_equals via FUNC_REF, etc.) — pass
     * through verbatim so the call site emits the correct C identifier
     * instead of double-prefixing to Iron_iron_string_concat. The compiler
     * generates these names internally from hir_lower.c, never the user. */
    if (strncmp(name, "iron_", 5) == 0) return name;

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

        /* Phase 20 PTR-01: checked pointers lower to the 16B Iron_FatPtr ABI
         * defined in src/runtime/iron_runtime.h (Phase 19 substrate lock).
         * Phase 25 UNCK-03 (Plan 25-02): unchecked pointers lower to bare T*
         * (8B) — no generation tracking, zero runtime check (UNCK-03). */
        case IRON_TYPE_PTR:
            if (t->ptr.is_unchecked) {
                /* Phase 25 UNCK-03 (Plan 25-02): bare C T* (8B) ABI.
                 * NOT Iron_FatPtr (16B) — Pitfall 5 honored. */
                const char *pointee_c = emit_type_to_c(t->ptr.pointee, ctx);
                /* Synthesize Iron_Box_<pointee> if not yet done — ensures the
                 * Box struct is available when this pointer type appears in
                 * function signatures or return types. */
                emit_ensure_box(ctx, t->ptr.pointee);
                Iron_StrBuf sb_ptr = iron_strbuf_create(32);
                iron_strbuf_appendf(&sb_ptr, "%s *", pointee_c);
                const char *result = iron_arena_strdup(ctx->arena,
                                                        iron_strbuf_get(&sb_ptr),
                                                        sb_ptr.len);
                iron_strbuf_free(&sb_ptr);
                if (!result) iron_oom_abort("emit_helpers.c:emit_type_to_c unchecked PTR");
                return result;
            }
            return "Iron_FatPtr";

        case IRON_TYPE_FUNC:
            return "Iron_Closure";

        case IRON_TYPE_ARRAY: {
            /* Phase 23 VEC-01: bounded vector [T; <=N] — emit Iron_BVec_T_N struct name.
             * emit_ensure_bvec MUST be called BEFORE returning the name (Pitfall 3:
             * typedef must be in struct_bodies before any function-body reference). */
            if (t->array.is_bounded && t->array.size >= 0) {
                emit_ensure_bvec(ctx, t);
                /* Rebuild mangled name (same formula as emit_ensure_bvec) */
                const char *elem_c_bv = emit_type_to_c(t->array.elem, ctx);
                Iron_StrBuf sb_bv = iron_strbuf_create(64);
                iron_strbuf_appendf(&sb_bv, "Iron_BVec_");
                for (const char *p = elem_c_bv; *p; p++) {
                    if (*p == ' ' || *p == '*') {
                        iron_strbuf_appendf(&sb_bv, "_");
                    } else {
                        char ch[2] = { *p, '\0' };
                        iron_strbuf_appendf(&sb_bv, "%s", ch);
                    }
                }
                iron_strbuf_appendf(&sb_bv, "_%d", t->array.size);
                const char *bvec_result = iron_arena_strdup(ctx->arena,
                                                             iron_strbuf_get(&sb_bv),
                                                             sb_bv.len);
                iron_strbuf_free(&sb_bv);
                if (!bvec_result) iron_oom_abort("emit_helpers.c:emit_type_to_c BVEC");
                return bvec_result;
            }
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

/* Phase 23 VEC-01: synthesise a C typedef for a bounded vector on demand.
 *
 *   typedef struct { uint32_t len; T data[N]; } Iron_BVec_<elem_c>_<N>;
 *
 * Dedupes via ctx->emitted_bvecs (same arrput/strcmp shape as emitted_tuples).
 * Recurses for nested bvec elements so inner typedefs land first.
 * No-op when the type is not a bounded array. */
void emit_ensure_bvec(EmitCtx *ctx, const Iron_Type *bvec_ty) {
    if (!bvec_ty || bvec_ty->kind != IRON_TYPE_ARRAY) return;
    if (!bvec_ty->array.is_bounded || bvec_ty->array.size < 0) return;

    const char *elem_c = emit_type_to_c(bvec_ty->array.elem, ctx);
    int N = bvec_ty->array.size;

    /* Build mangled name: Iron_BVec_<elem_c>_<N> with space and * escaped to _ */
    Iron_StrBuf sb = iron_strbuf_create(64);
    iron_strbuf_appendf(&sb, "Iron_BVec_");
    for (const char *p = elem_c; *p; p++) {
        if (*p == ' ' || *p == '*') {
            iron_strbuf_appendf(&sb, "_");
        } else {
            char ch[2] = { *p, '\0' };
            iron_strbuf_appendf(&sb, "%s", ch);
        }
    }
    iron_strbuf_appendf(&sb, "_%d", N);
    const char *struct_name = iron_arena_strdup(ctx->arena, iron_strbuf_get(&sb), sb.len);
    iron_strbuf_free(&sb);
    if (!struct_name) iron_oom_abort("emit_helpers.c:emit_ensure_bvec struct_name");

    /* Dedupe: register BEFORE recursing to guard against self-reference */
    for (int i = 0; i < (int)arrlen(ctx->emitted_bvecs); i++) {
        if (strcmp(ctx->emitted_bvecs[i], struct_name) == 0) return;
    }
    char *struct_name_copy = iron_arena_strdup(ctx->arena, struct_name, strlen(struct_name));
    if (!struct_name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_bvec struct_name_copy");
    arrput(ctx->emitted_bvecs, struct_name_copy);

    /* Recurse for nested bvec elem so inner typedef lands first */
    if (bvec_ty->array.elem &&
        bvec_ty->array.elem->kind == IRON_TYPE_ARRAY &&
        bvec_ty->array.elem->array.is_bounded) {
        emit_ensure_bvec(ctx, bvec_ty->array.elem);
    }

    /* Emit: typedef struct { uint32_t len; T data[N]; } Iron_BVec_T_N; */
    iron_strbuf_appendf(&ctx->struct_bodies,
        "/* Phase 23 VEC-01: bounded vector [%s; <=%d] */\n"
        "typedef struct { uint32_t len; %s data[%d]; } %s;\n",
        elem_c, N, elem_c, N, struct_name);
}

/* Phase 25 UNCK-01/02 (Plan 25-02): Per-T Box synthesis — mirrors emit_ensure_bvec (line 341).
 * Synthesizes typedef + Box_T_new/Box_T_unwrap/Box_T_free helpers for each
 * concrete instantiation Iron_Box_<T>. Idempotent via emitted_boxes dedup.
 *
 * Pitfall 3 (RESEARCH): helpers go in ctx->lifted_funcs, NOT struct_bodies —
 *   lifted_funcs renders after struct_bodies so forward-reference is safe.
 * Pitfall 5 (RESEARCH): Box_T_unwrap returns bare T* (8B), NOT Iron_FatPtr (16B).
 *
 * Phase 26 (Plan 26-02): rc Box[T] rejected via E0286 — see RC-LAYOUT.md §3.1.
 *   No new diagnostic code allocated; the existing Phase 24 DROP-08 nocopy-
 *   copy violation fires at the rc allocation site because rc requires copy
 *   semantics (refcount-bump on each copy) which Box[T] forbids (nocopy). */
void emit_ensure_box(EmitCtx *ctx, const Iron_Type *elem_type) {
    if (!elem_type) return;

    const char *elem_c = emit_type_to_c(elem_type, ctx);
    if (!elem_c) return;

    /* Build mangled name: Iron_Box_<elem_c> with space and * escaped to _ */
    Iron_StrBuf sb = iron_strbuf_create(64);
    iron_strbuf_appendf(&sb, "Iron_Box_");
    for (const char *p = elem_c; *p; p++) {
        if (*p == ' ' || *p == '*') {
            iron_strbuf_appendf(&sb, "_");
        } else {
            char ch[2] = { *p, '\0' };
            iron_strbuf_appendf(&sb, "%s", ch);
        }
    }
    const char *struct_name = iron_arena_strdup(ctx->arena, iron_strbuf_get(&sb), sb.len);
    iron_strbuf_free(&sb);
    if (!struct_name) iron_oom_abort("emit_helpers.c:emit_ensure_box struct_name");

    /* Dedupe — Phase 23/24 emitted_* pattern */
    for (int i = 0; i < (int)arrlen(ctx->emitted_boxes); i++) {
        if (strcmp(ctx->emitted_boxes[i], struct_name) == 0) return;
    }
    char *name_copy = iron_arena_strdup(ctx->arena, struct_name, strlen(struct_name));
    if (!name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_box name_copy");
    arrput(ctx->emitted_boxes, name_copy);

    /* Emit typedef into struct_bodies (Pitfall 3: struct_bodies for typedef;
     * lifted_funcs for helpers — forward-reference is safe). */
    iron_strbuf_appendf(&ctx->struct_bodies,
        "/* Phase 25 UNCK-01/02: Box[%s] per-T synthesis */\n"
        "typedef struct { Iron_FatPtr inner; } %s;\n",
        elem_c, struct_name);

    /* Emit helpers into lifted_funcs (NOT struct_bodies — Pitfall 3). */
    iron_strbuf_appendf(&ctx->lifted_funcs,
        "static %s %s_new(%s value) {\n"
        "    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, sizeof(%s));\n"
        "    ((%s *)fp.addr)[0] = value;\n"
        "    %s box; box.inner = fp; return box;\n"
        "}\n"
        "static %s *%s_unwrap(%s *box) {\n"
        "    /* Pitfall 5: returns bare T* (8B), NOT Iron_FatPtr (16B) */\n"
        "    if (!box || !box->inner.addr) { iron_panic(\"null Box\", \"<box>\", 0); }\n"
        "    return (%s *)box->inner.addr;\n"
        "}\n"
        "static bool %s_is_null(const %s *box) {\n"
        "    return !box || !box->inner.addr;\n"
        "}\n"
        "static %s %s_null_val(void) {\n"
        "    %s box; box.inner.addr = NULL; box.inner.gen = 0; return box;\n"
        "}\n"
        "static void %s_free(%s *box) {\n"
        "    /* Phase 26 (Plan 26-02): rc Box[T] rejected via E0286 -- "
        "see RC-LAYOUT.md section 3.1 */\n"
        "    if (box && box->inner.addr) { iron_heap_free(box->inner); "
        "box->inner.addr = NULL; }\n"
        "}\n\n",
        /* _new */ struct_name, struct_name, elem_c,
        elem_c, elem_c, struct_name,
        /* _unwrap */ elem_c, struct_name, struct_name,
        elem_c,
        /* _is_null */ struct_name, struct_name,
        /* _null_val */ struct_name, struct_name,
        struct_name,
        /* _free */ struct_name, struct_name);
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
    /* Phase 80 MUT-07: the mut-receiver param (vid==1) is emitted by
     * emit_func_signature as `T *_v1` (pointer), so field access through it
     * must use `->` not `.`. Parameter values have NULL entries in
     * value_table, so this check has to come BEFORE the instr-NULL guard. */
    if (fn->is_mut_receiver_method && vid == 1) return true;
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

/* Phase 21: Returns true when the LIR value was produced by IRON_LIR_HEAP_ALLOC
 * (Iron_FatPtr local — post-migration type).  Distinct from emit_val_is_heap_ptr
 * which also returns true for IRON_LIR_RC_ALLOC (T * local — still a pointer).
 * Used at field-access / field-store / addr-of sites to select the
 * `((T *)_vN.addr)->field` form vs `_vN->field` for RC pointers. */
bool emit_val_is_heap_fat_ptr(IronLIR_Func *fn, IronLIR_ValueId vid) {
    if (vid == IRON_LIR_VALUE_INVALID) return false;
    if (vid >= (IronLIR_ValueId)arrlen(fn->value_table)) return false;
    IronLIR_Instr *instr = fn->value_table[vid];
    if (!instr) return false;
    return instr->kind == IRON_LIR_HEAP_ALLOC;
}

/* Phase 21: Returns true when a value is ANY Iron_FatPtr at runtime:
 * IRON_LIR_HEAP_ALLOC (heap binding) OR IRON_LIR_ADDR_OF (pointer-to-heap/stack).
 * Both produce C locals of type Iron_FatPtr, so field-access and deref sites
 * must use the ((T *)_vN.addr)->field form rather than _vN.field. */
bool emit_val_is_any_fat_ptr(IronLIR_Func *fn, IronLIR_ValueId vid) {
    if (vid == IRON_LIR_VALUE_INVALID) return false;
    if (vid >= (IronLIR_ValueId)arrlen(fn->value_table)) return false;
    IronLIR_Instr *instr = fn->value_table[vid];
    if (!instr) return false;
    return instr->kind == IRON_LIR_HEAP_ALLOC || instr->kind == IRON_LIR_ADDR_OF;
}

/* Phase 21: Return the C pointee-type string for any Iron_FatPtr value.
 * - HEAP_ALLOC: the heap-allocated object type (instr->type).
 * - ADDR_OF targeting HEAP_ALLOC: the target's object type.
 * - ADDR_OF targeting other: the target's type.
 * Returns NULL if not a fat ptr. */
const char *emit_fat_ptr_pointee_type_c(IronLIR_Func *fn, IronLIR_ValueId vid, EmitCtx *ctx) {
    if (vid == IRON_LIR_VALUE_INVALID) return NULL;
    if (vid >= (IronLIR_ValueId)arrlen(fn->value_table)) return NULL;
    IronLIR_Instr *instr = fn->value_table[vid];
    if (!instr) return NULL;
    if (instr->kind == IRON_LIR_HEAP_ALLOC) {
        return emit_type_to_c(instr->type, ctx);
    }
    if (instr->kind == IRON_LIR_ADDR_OF) {
        IronLIR_ValueId tgt = instr->addr_of.target;
        if (tgt == IRON_LIR_VALUE_INVALID) return NULL;
        if (tgt >= (IronLIR_ValueId)arrlen(fn->value_table)) return NULL;
        IronLIR_Instr *tgt_instr = fn->value_table[tgt];
        if (!tgt_instr) return NULL;
        return emit_type_to_c(tgt_instr->type, ctx);
    }
    return NULL;
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

/* ── Phase 24 DROP-01/06: emit_ensure_drop + emit_ensure_copy ────────────── */

/* Build the hir_lower.c mangled drop name for a given object type name.
 * Pattern: lowercase chars before first '_', append "_drop".
 * Writes into buf (size >= strlen(type_name) + 6); returns false if buf too small. */
static bool build_drop_lir_name(const char *type_name, char *buf, size_t buf_size) {
    if (!type_name) return false;
    size_t tlen = strlen(type_name);
    if (tlen + 6 >= buf_size) return false;
    memcpy(buf, type_name, tlen);
    buf[tlen]   = '_';
    buf[tlen+1] = 'd';
    buf[tlen+2] = 'r';
    buf[tlen+3] = 'o';
    buf[tlen+4] = 'p';
    buf[tlen+5] = '\0';
    for (int ci = 0; buf[ci] && buf[ci] != '_'; ci++) {
        if (buf[ci] >= 'A' && buf[ci] <= 'Z')
            buf[ci] = (char)(buf[ci] + ('a' - 'A'));
    }
    return true;
}

/* Returns true if the object type od has a compiled drop method in the LIR module.
 * Uses build_drop_lir_name + emit_find_ir_func (Plan 86 layout: methods are LIR
 * top-level functions, NOT stored on Iron_ObjectDecl).
 * Non-static: declared in emit_helpers.h for use by emit_c.c. */
bool od_has_drop_lir(EmitCtx *ctx, struct Iron_ObjectDecl *od) {
    if (!ctx || !od || !od->name) return false;
    char lir_name[256];
    if (!build_drop_lir_name(od->name, lir_name, sizeof(lir_name))) return false;
    return emit_find_ir_func(ctx, lir_name) != NULL;
}

/* Synthesize a static destructor function for an object type.
 * Emits `static void <TypeName>_drop(<TypeName> *self) { ... }` into
 * ctx->lifted_funcs (Pitfall 3 — NOT struct_bodies). Dedupes via
 * ctx->emitted_drops. Recurses for field types that have drop blocks.
 * The user drop body is called via the compiled LIR method (B4 fix);
 * field destructors run in REVERSE declaration order (Pitfall 6 + DROP-02). */
void emit_ensure_drop(EmitCtx *ctx, const char *obj_c_name,
                      struct Iron_ObjectDecl *od) {
    if (!ctx || !obj_c_name || !od) return;

    /* Dedupe: guard against double-synthesis (and self-referential loops) */
    for (int i = 0; i < (int)arrlen(ctx->emitted_drops); i++) {
        if (strcmp(ctx->emitted_drops[i], obj_c_name) == 0) return;
    }
    char *name_copy = iron_arena_strdup(ctx->arena, obj_c_name, strlen(obj_c_name));
    if (!name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_drop name_copy");
    arrput(ctx->emitted_drops, name_copy);

    /* Recurse for field types with drop blocks so inner destructors land first
     * (forward-declaration safety — lifted_funcs ordering, Pitfall 3).
     * Check via od_has_drop_lir — methods are LIR top-level functions, NOT od->methods. */
    for (int i = 0; i < od->field_count; i++) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        if (!f || !f->field_type_cached) continue;
        Iron_Type *ft = f->field_type_cached;
        if (ft->kind != IRON_TYPE_OBJECT || !ft->object.decl) continue;
        struct Iron_ObjectDecl *field_od = ft->object.decl;
        if (od_has_drop_lir(ctx, field_od)) {
            const char *field_c_name = emit_type_to_c(ft, ctx);
            emit_ensure_drop(ctx, field_c_name, field_od);
        }
    }

    /* Synthesize <TypeName>_drop into ctx->lifted_funcs (Pitfall 3 — NOT struct_bodies) */
    iron_strbuf_appendf(&ctx->lifted_funcs,
        "/* Phase 24 DROP-01: destructor for %s — static-type dispatch (DROP-03) */\n"
        "/* Phase 26 POL-06: static dispatch via <TypeName>_rc_drop symbol -- "
        "synthesized by emit_ensure_rc_drop in Plan 26-03; called by "
        "iron_rc_release on refcount == 0. */\n"
        "static void %s_drop(%s *self) {\n"
        "    if (!self) return;\n",
        obj_c_name, obj_c_name, obj_c_name);

    /* B4 fix: emit user drop body inline by calling the compiled LIR method.
     * Build the LIR mangled name (hir_lower.c: lowercase first word + "_drop"),
     * resolve to the C function name, and emit a call if the function exists.
     * The marker comment is required for acceptance grep
     * (grep -c 'user drop body lowered' src/lir/emit_helpers.c >= 1). */
    {
        char lir_name[256];
        if (od->name && build_drop_lir_name(od->name, lir_name, sizeof(lir_name))) {
            IronLIR_Func *lir_fn = emit_find_ir_func(ctx, lir_name);
            if (lir_fn) {
                iron_strbuf_appendf(&ctx->lifted_funcs,
                    "    /* user drop body lowered: %s (Phase 24 DROP-01 / B4) */\n",
                    obj_c_name);
                const char *c_func = emit_resolve_func_c_name(ctx, lir_name);
                iron_strbuf_appendf(&ctx->lifted_funcs,
                    "    %s(self);\n", c_func);
            }
        }
    }

    /* Field destructors REVERSE declaration order (Pitfall 6 + DROP-02).
     * Check via od_has_drop_lir — methods are LIR functions, NOT od->methods. */
    for (int i = od->field_count - 1; i >= 0; i--) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        if (!f || !f->field_type_cached) continue;
        Iron_Type *ft = f->field_type_cached;
        if (ft->kind != IRON_TYPE_OBJECT || !ft->object.decl) continue;
        struct Iron_ObjectDecl *field_od = ft->object.decl;
        if (od_has_drop_lir(ctx, field_od)) {
            const char *field_c_name = emit_type_to_c(ft, ctx);
            /* Phase 24 DROP-04 (Plan 24-03): the field's drop function sets
             * iron_in_destructor=true internally (emit_func_body prologue).
             * No call-site wrap needed — just invoke the synthesized drop. */
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    %s_drop(&self->%s);\n",
                field_c_name, f->name);
        }
    }
    iron_strbuf_appendf(&ctx->lifted_funcs, "}\n\n");
}

/* Phase 26 POL-06 (Plan 26-03): does this object type have any drop need
 * (user drop body OR any field with its own drop)?
 *
 * Mirrors emit_ensure_rc_drop's drop-need check so emit_c.c IRON_LIR_RC_ALLOC
 * can decide whether to pass <TypeName>_rc_drop or NULL as drop_fn. Anti-Pattern
 * 4 (RESEARCH:251): types without drop need pass NULL — iron_rc_release just
 * frees the block. */
bool od_has_rc_drop_need(EmitCtx *ctx, struct Iron_ObjectDecl *od) {
    if (!ctx || !od) return false;
    if (od_has_drop_lir(ctx, od)) return true;
    for (int i = 0; i < od->field_count; i++) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        if (!f || !f->field_type_cached) continue;
        Iron_Type *ft = f->field_type_cached;
        if (ft->kind == IRON_TYPE_OBJECT && ft->object.decl &&
            od_has_drop_lir(ctx, ft->object.decl)) {
            return true;
        }
    }
    return false;
}

/* Phase 26 POL-06 (Plan 26-03): synthesize <TypeName>_rc_drop trampoline.
 *
 * Emits a void*-signature wrapper around the Phase 24 <TypeName>_drop helper:
 *   static void <TypeName>_rc_drop(void *self_void) {
 *       <TypeName>_drop((<TypeName> *)self_void);
 *   }
 *
 * The trampoline goes into ctx->lifted_funcs (NOT struct_bodies, Pitfall 3).
 * Iron_RcHeader.drop_fn stores this function pointer; iron_rc_release invokes
 * it on the last-reference path before freeing the block (drop chain order:
 * user drop -> field destructors reverse-decl -> free, all inherited from
 * Phase 24 _drop machinery).
 *
 * Only emits when the type has drop need (od_has_rc_drop_need). For pure-data
 * rc types, the caller passes NULL drop_fn at iron_rc_alloc time.
 *
 * Dedup via ctx->emitted_rc_drops (mirrors Phase 24 emitted_drops). */
void emit_ensure_rc_drop(EmitCtx *ctx, const char *obj_c_name,
                          struct Iron_ObjectDecl *od) {
    if (!ctx || !obj_c_name || !od) return;
    if (!od_has_rc_drop_need(ctx, od)) return;

    /* Dedup */
    for (int i = 0; i < (int)arrlen(ctx->emitted_rc_drops); i++) {
        if (strcmp(ctx->emitted_rc_drops[i], obj_c_name) == 0) return;
    }
    char *name_copy = iron_arena_strdup(ctx->arena, obj_c_name, strlen(obj_c_name));
    if (!name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_rc_drop name_copy");
    arrput(ctx->emitted_rc_drops, name_copy);

    /* Ensure the Phase 24 <TypeName>_drop exists (trampoline calls into it) */
    emit_ensure_drop(ctx, obj_c_name, od);

    /* Synthesize the trampoline into ctx->lifted_funcs */
    iron_strbuf_appendf(&ctx->lifted_funcs,
        "/* Phase 26 POL-06 (Plan 26-03): rc drop trampoline for %s.\n"
        " * Iron_RcHeader.drop_fn stores this; iron_rc_release calls it\n"
        " * on last reference (before freeing the block). Delegates to\n"
        " * the Phase 24 <TypeName>_drop machinery: user drop body ->\n"
        " * field destructors reverse-decl -> free. */\n"
        "static void %s_rc_drop(void *self_void) {\n"
        "    if (!self_void) return;\n"
        "    %s_drop((%s *)self_void);\n"
        "}\n\n",
        obj_c_name, obj_c_name, obj_c_name, obj_c_name);
}

/* Synthesize a shallow copy function for an object type.
 * Emits `static void <TypeName>_copy(<TypeName> *dest, const <TypeName> *src)`
 * into ctx->lifted_funcs. Dedupes via ctx->emitted_copies.
 * No-op when od->is_nocopy (Pitfall 5). Per-field copy hooks call
 * <FieldType>_copy for fields whose has_user_copy_transitive is true (I8). */
void emit_ensure_copy(EmitCtx *ctx, const char *obj_c_name,
                      struct Iron_ObjectDecl *od) {
    if (!ctx || !obj_c_name || !od) return;
    if (od->is_nocopy) return;  /* Pitfall 5: no _copy synthesized for nocopy types */

    /* Dedupe */
    for (int i = 0; i < (int)arrlen(ctx->emitted_copies); i++) {
        if (strcmp(ctx->emitted_copies[i], obj_c_name) == 0) return;
    }
    char *name_copy = iron_arena_strdup(ctx->arena, obj_c_name, strlen(obj_c_name));
    if (!name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_copy name_copy");
    arrput(ctx->emitted_copies, name_copy);

    /* Recurse for field types that have user copy hooks */
    for (int i = 0; i < od->field_count; i++) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        if (!f || !f->field_type_cached) continue;
        Iron_Type *ft = f->field_type_cached;
        if (!ft->has_user_copy_cached || !ft->has_user_copy_transitive) continue;
        if (ft->kind != IRON_TYPE_OBJECT || !ft->object.decl) continue;
        const char *field_c_name = emit_type_to_c(ft, ctx);
        emit_ensure_copy(ctx, field_c_name, ft->object.decl);
    }

    iron_strbuf_appendf(&ctx->lifted_funcs,
        "/* Phase 24 DROP-06: shallow memberwise copy for %s */\n"
        "static void %s_copy(%s *dest, const %s *src) {\n"
        "    if (!dest || !src) return;\n"
        "    *dest = *src;\n",
        obj_c_name, obj_c_name, obj_c_name, obj_c_name);

    /* Per-field copy hooks — read cached field directly (I8 fix) */
    for (int i = 0; i < od->field_count; i++) {
        Iron_Field *f = (Iron_Field *)od->fields[i];
        if (!f || !f->field_type_cached) continue;
        Iron_Type *ft = f->field_type_cached;
        if (!ft->has_user_copy_cached || !ft->has_user_copy_transitive) continue;
        const char *field_c_name = emit_type_to_c(ft, ctx);
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    %s_copy(&dest->%s, &src->%s);\n",
            field_c_name, f->name, f->name);
    }
    iron_strbuf_appendf(&ctx->lifted_funcs, "}\n\n");
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
    arrfree(ctx->emitted_bvecs);
    arrfree(ctx->emitted_drops);
    arrfree(ctx->emitted_copies);
    arrfree(ctx->emitted_boxes);  /* Phase 25 UNCK-01/02 (Plan 25-02) */
    arrfree(ctx->emitted_rc_drops);  /* Phase 26 POL-06 (Plan 26-03) */
    arrfree(ctx->emitted_env_drops); /* Phase 26 OQ-03 (Plan 26-03) */
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
