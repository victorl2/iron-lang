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

/* Phase 33 STDLIB-07/08 (Plan 33-05): forward decl — defined below near the
 * emit_ensure_mutex/channel synthesis. Used by emit_type_to_c's resource arms. */
static const char *emit_elem_c_escaped(EmitCtx *ctx, const Iron_Type *elem);

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
            /* Phase 33 OQ-02 (Plan 33-07): the builtin generic object Box[T]
             * has no plain C struct — it lowers to the per-T synthesized
             * Iron_Box_<elemC> typedef (emit_ensure_box). object.elem carries
             * the concrete element from the by-name Box dispatch in
             * typecheck.c. Trigger synthesis here so the typedef + helpers are
             * available wherever a Box value type appears, then return the
             * mangled name (identical formula to emit_ensure_box). */
            if (t->object.decl && t->object.decl->name &&
                strcmp(t->object.decl->name, "Box") == 0 && t->object.elem) {
                emit_ensure_box(ctx, t->object.elem);
                const char *elem_c = emit_type_to_c(t->object.elem, ctx);
                Iron_StrBuf sb = iron_strbuf_create(64);
                iron_strbuf_appendf(&sb, "Iron_Box_");
                for (const char *p = elem_c; *p; p++) {
                    if (*p == ' ' || *p == '*') iron_strbuf_appendf(&sb, "_");
                    else { char ch[2] = { *p, '\0' }; iron_strbuf_appendf(&sb, "%s", ch); }
                }
                const char *result = iron_arena_strdup(ctx->arena,
                                                        iron_strbuf_get(&sb), sb.len);
                iron_strbuf_free(&sb);
                if (!result) iron_oom_abort("emit_helpers.c:emit_type_to_c Box");
                return result;
            }
            /* Phase 33 STDLIB-07/08/09 (Plan 33-05): the nocopy resource-type
             * surfaces have no plain C struct — they map to the runtime types
             * (Iron_Mutex* / Iron_Channel*) or per-T / non-generic synthesized
             * typedefs. Trigger synthesis here so the typedef + helpers are
             * available wherever the value type appears, then return the C
             * type. Keyed on the surface decl name. */
            if (t->object.decl && t->object.decl->name) {
                const char *on = t->object.decl->name;
                if (strcmp(on, "Arena") == 0) {
                    /* Phase 28/33 arena surface: the stdlib `Arena` object is
                     * an opaque handle to the RUNTIME arena
                     * (src/runtime/iron_arena_rt.h). Mapping it to
                     * `Iron_` + name would collide with the COMPILER-internal
                     * Iron_Arena (src/util/arena.h, pulled into the generated
                     * TU via iron_runtime.h -> diagnostics.h) — a totally
                     * different struct — so every iron_arena_rt_* call site
                     * got an incompatible-type error. Mirror the Mutex
                     * handling below: the C value representation IS the
                     * runtime handle pointer. */
                    return "Iron_Arena_RT *";
                }
                if (strcmp(on, "Mutex") == 0) {
                    if (t->object.elem) emit_ensure_mutex(ctx, t->object.elem);
                    return "Iron_Mutex *";
                }
                if (strcmp(on, "Channel") == 0) {
                    if (t->object.elem) emit_ensure_channel(ctx, t->object.elem);
                    return "Iron_Channel *";
                }
                if (strcmp(on, "MutexGuard") == 0 && t->object.elem) {
                    emit_ensure_mutex(ctx, t->object.elem);
                    const char *esc = emit_elem_c_escaped(ctx, t->object.elem);
                    Iron_StrBuf sb = iron_strbuf_create(48);
                    iron_strbuf_appendf(&sb, "Iron_MutexGuard_%s", esc ? esc : "");
                    const char *r = iron_arena_strdup(ctx->arena,
                                                      iron_strbuf_get(&sb), sb.len);
                    iron_strbuf_free(&sb);
                    if (!r) iron_oom_abort("emit_helpers.c:emit_type_to_c MutexGuard");
                    return r;
                }
                if (strcmp(on, "FileHandle") == 0) {
                    emit_ensure_filehandle(ctx);
                    return "Iron_FileHandle";
                }
                if (strcmp(on, "RWLock") == 0) {
                    if (t->object.elem) emit_ensure_rwlock(ctx, t->object.elem);
                    const char *esc = t->object.elem
                        ? emit_elem_c_escaped(ctx, t->object.elem) : "";
                    Iron_StrBuf sb = iron_strbuf_create(48);
                    iron_strbuf_appendf(&sb, "Iron_RWLock_%s *", esc ? esc : "");
                    const char *r = iron_arena_strdup(ctx->arena,
                                                      iron_strbuf_get(&sb), sb.len);
                    iron_strbuf_free(&sb);
                    if (!r) iron_oom_abort("emit_helpers.c:emit_type_to_c RWLock");
                    return r;
                }
                if ((strcmp(on, "RWReadGuard") == 0 ||
                     strcmp(on, "RWWriteGuard") == 0) && t->object.elem) {
                    emit_ensure_rwlock(ctx, t->object.elem);
                    const char *esc = emit_elem_c_escaped(ctx, t->object.elem);
                    Iron_StrBuf sb = iron_strbuf_create(48);
                    iron_strbuf_appendf(&sb, "Iron_%s_%s", on, esc ? esc : "");
                    const char *r = iron_arena_strdup(ctx->arena,
                                                      iron_strbuf_get(&sb), sb.len);
                    iron_strbuf_free(&sb);
                    if (!r) iron_oom_abort("emit_helpers.c:emit_type_to_c RWGuard");
                    return r;
                }
            }
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

        /* Phase 27 POL-08 (Plan 27-02): weak rc T lowers to the SAME C type
         * as rc T (a payload-typed pointer).  The Plan 27-01 runtime model
         * treats `iron_rc_downgrade(rc)` as returning the same user pointer
         * with the weak_count bumped — the type-system distinction is purely
         * compile-time.  emit_c.c arms for IRON_LIR_WEAK_RC_* opcodes drive
         * the runtime semantics via iron_weak_rc_retain/release. */
        case IRON_TYPE_WEAK_RC: {
            const char *inner_c = emit_type_to_c(t->weak_rc.inner, ctx);
            Iron_StrBuf sb = iron_strbuf_create(64);
            iron_strbuf_appendf(&sb, "%s*", inner_c);
            const char *result = iron_arena_strdup(ctx->arena,
                                                    iron_strbuf_get(&sb),
                                                    sb.len);
            if (!result) iron_oom_abort("emit_helpers.c:emit_type_to_c WEAK_RC");
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

/* Phase 37 M5 (rc-balance handoff): container element drop-need probe.
 * A Box/Mutex/Channel/RWLock element type has drop-need when it is an rc T
 * (refcount release owed at container destroy) or an object type with
 * destructor need — a user drop body, droppable object fields
 * (od_has_rc_drop_need), or the FileHandle fd wrapper whose drop is
 * emit-synthesized rather than lowered. Trivially-destructible element
 * types return false so the historical glue stays byte-identical. */
static bool emit_container_elem_drop_need(EmitCtx *ctx, const Iron_Type *elem) {
    if (!elem) return false;
    if (elem->kind == IRON_TYPE_RC) return true;
    if (elem->kind == IRON_TYPE_OBJECT && elem->object.decl) {
        struct Iron_ObjectDecl *od = elem->object.decl;
        if (od->name && strcmp(od->name, "FileHandle") == 0) return true;
        return od_has_rc_drop_need(ctx, od);
    }
    return false;
}

/* Companion to emit_container_elem_drop_need: make sure the element's
 * underlying `<elemC>_drop` destructor definition lands in lifted_funcs
 * BEFORE any container glue that references it. rc elements need no
 * synthesized dtor (iron_rc_release is a runtime symbol). */
static void emit_container_elem_ensure_dtor(EmitCtx *ctx, const Iron_Type *elem,
                                            const char *elem_c) {
    if (!elem || !elem_c) return;
    if (elem->kind != IRON_TYPE_OBJECT || !elem->object.decl) return;
    if (elem->object.decl->name &&
        strcmp(elem->object.decl->name, "FileHandle") == 0) {
        emit_ensure_filehandle(ctx);
        return;
    }
    emit_ensure_drop(ctx, elem_c, elem->object.decl);
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

    /* Phase 37 (4.3 Box completion): a Box[T] owning a droppable T must run
     * T's destructor before releasing the heap cell — on the explicit
     * .free() path AND the scope-exit Iron_Box_<T>_free path (they share
     * this helper). Ensure the element dtor definition precedes the Box
     * helpers in lifted_funcs. rc Box[T] is rejected upstream (E0286), but
     * the rc arm below stays defensive-correct should that ever loosen. */
    bool box_elem_drop = emit_container_elem_drop_need(ctx, elem_type);
    if (box_elem_drop) emit_container_elem_ensure_dtor(ctx, elem_type, elem_c);

    /* Emit helpers into lifted_funcs (NOT struct_bodies — Pitfall 3). */
    iron_strbuf_appendf(&ctx->lifted_funcs,
        "static %s %s_new(%s value) {\n"
        "    Iron_FatPtr fp = iron_heap_alloc(__FILE__, __LINE__, sizeof(%s));\n"
        "    ((%s *)fp.addr)[0] = value;\n"
        "    %s box; box.inner = fp; return box;\n"
        "}\n"
        "static %s *%s_unwrap(%s *box) {\n"
        "    /* Pitfall 5: returns bare T* (8B), NOT Iron_FatPtr (16B) */\n"
        "    if (!box || !box->inner.addr) {\n"
        "        fprintf(stderr, \"iron: panic: unwrap() on null Box\\n\");\n"
        "        abort();\n"
        "    }\n"
        "    return (%s *)box->inner.addr;\n"
        "}\n"
        "static bool %s_is_null(const %s *box) {\n"
        "    return !box || !box->inner.addr;\n"
        "}\n"
        "static %s %s_null_val(void) {\n"
        "    %s box; box.inner.addr = NULL; box.inner.gen = 0; return box;\n"
        "}\n",
        /* _new */ struct_name, struct_name, elem_c,
        elem_c, elem_c, struct_name,
        /* _unwrap */ elem_c, struct_name, struct_name,
        elem_c,
        /* _is_null */ struct_name, struct_name,
        /* _null_val */ struct_name, struct_name,
        struct_name);
    if (!box_elem_drop) {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void %s_free(%s *box) {\n"
            "    /* Phase 26 (Plan 26-02): rc Box[T] rejected via E0286 -- "
            "see RC-LAYOUT.md section 3.1 */\n"
            "    if (box && box->inner.addr) { iron_heap_free(box->inner); "
            "box->inner.addr = NULL; }\n"
            "}\n\n",
            /* _free */ struct_name, struct_name);
    } else if (elem_type->kind == IRON_TYPE_RC) {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void %s_free(%s *box) {\n"
            "    /* Phase 37: rc element — release the refcount before the cell */\n"
            "    if (box && box->inner.addr) {\n"
            "        iron_rc_release(*(void **)box->inner.addr);\n"
            "        iron_heap_free(box->inner); box->inner.addr = NULL;\n"
            "    }\n"
            "}\n\n",
            /* _free */ struct_name, struct_name);
    } else {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void %s_free(%s *box) {\n"
            "    /* Phase 37: Box[T] owns a droppable T -- run the element\n"
            "     * destructor before releasing the heap cell (UNCK-04). */\n"
            "    if (box && box->inner.addr) {\n"
            "        %s_drop((%s *)box->inner.addr);\n"
            "        iron_heap_free(box->inner); box->inner.addr = NULL;\n"
            "    }\n"
            "}\n\n",
            /* _free */ struct_name, struct_name, elem_c, elem_c);
    }
}

/* Phase 33 STDLIB-07/08 (Plan 33-05): build the escaped element-C suffix shared
 * by the Mutex/Channel glue mangling (mirrors emit_ensure_box's name builder).
 * Returns an arena string like "int64_t" with space/`*` escaped to `_`. */
static const char *emit_elem_c_escaped(EmitCtx *ctx, const Iron_Type *elem) {
    const char *elem_c = emit_type_to_c(elem, ctx);
    if (!elem_c) return NULL;
    Iron_StrBuf sb = iron_strbuf_create(32);
    for (const char *p = elem_c; *p; p++) {
        if (*p == ' ' || *p == '*') iron_strbuf_appendf(&sb, "_");
        else { char ch[2] = { *p, '\0' }; iron_strbuf_appendf(&sb, "%s", ch); }
    }
    const char *result = iron_arena_strdup(ctx->arena, iron_strbuf_get(&sb), sb.len);
    iron_strbuf_free(&sb);
    if (!result) iron_oom_abort("emit_helpers.c:emit_elem_c_escaped");
    return result;
}

void emit_ensure_mutex(EmitCtx *ctx, const Iron_Type *elem_type) {
    if (!elem_type) return;
    const char *elem_c = emit_type_to_c(elem_type, ctx);
    if (!elem_c) return;
    const char *esc = emit_elem_c_escaped(ctx, elem_type);
    if (!esc) return;

    /* Guard typedef name: Iron_MutexGuard_<esc> */
    Iron_StrBuf gsb = iron_strbuf_create(48);
    iron_strbuf_appendf(&gsb, "Iron_MutexGuard_%s", esc);
    const char *guard_name = iron_arena_strdup(ctx->arena, iron_strbuf_get(&gsb), gsb.len);
    iron_strbuf_free(&gsb);
    if (!guard_name) iron_oom_abort("emit_helpers.c:emit_ensure_mutex guard_name");

    /* Dedupe on the guard name (covers the whole Mutex_<T> family). */
    for (int i = 0; i < (int)arrlen(ctx->emitted_mutexes); i++) {
        if (strcmp(ctx->emitted_mutexes[i], guard_name) == 0) return;
    }
    char *name_copy = iron_arena_strdup(ctx->arena, guard_name, strlen(guard_name));
    if (!name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_mutex name_copy");
    arrput(ctx->emitted_mutexes, name_copy);

    /* Guard typedef into struct_bodies (Pitfall 3: typedef before helpers). */
    iron_strbuf_appendf(&ctx->struct_bodies,
        "/* Phase 33 STDLIB-07: Mutex[%s] per-T glue */\n"
        "typedef struct { Iron_Mutex *owner; %s *valptr; } %s;\n",
        elem_c, elem_c, guard_name);

    /* Phase 37 M5: element-drop-aware destroy. When T has drop-need,
     * synthesize a void* trampoline over the element destructor and thread
     * it through Iron_mutex_destroy_with so the parked value's resource is
     * released at Mutex destroy (previously leaked). Trivially-destructible
     * T keeps the historical Iron_mutex_destroy glue byte-identical. */
    bool m_elem_drop = emit_container_elem_drop_need(ctx, elem_type);
    if (m_elem_drop) {
        emit_container_elem_ensure_dtor(ctx, elem_type, elem_c);
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "/* Phase 37 M5: Mutex[%s] element drop trampoline */\n"
            "static void Iron_Mutex_%s_elem_drop(void *p) {\n",
            elem_c, esc);
        if (elem_type->kind == IRON_TYPE_RC) {
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    iron_rc_release(*(void **)p);\n");
        } else {
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    %s_drop((%s *)p);\n", elem_c, elem_c);
        }
        iron_strbuf_appendf(&ctx->lifted_funcs, "}\n");
    }

    /* Helpers into lifted_funcs. */
    iron_strbuf_appendf(&ctx->lifted_funcs,
        "static Iron_Mutex *Iron_Mutex_%s_new(%s value) {\n"
        "    return Iron_mutex_create(&value, sizeof(%s));\n"
        "}\n",
        /* _new */    esc, elem_c, elem_c);
    if (m_elem_drop) {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void Iron_Mutex_%s_destroy(Iron_Mutex **m) {\n"
            "    if (m && *m) { Iron_mutex_destroy_with(*m, Iron_Mutex_%s_elem_drop); *m = NULL; }\n"
            "}\n",
            esc, esc);
    } else {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void Iron_Mutex_%s_destroy(Iron_Mutex **m) {\n"
            "    if (m && *m) { Iron_mutex_destroy(*m); *m = NULL; }\n"
            "}\n",
            /* _destroy */ esc);
    }
    iron_strbuf_appendf(&ctx->lifted_funcs,
        "static %s Iron_MutexGuard_%s_lock(Iron_Mutex **m) {\n"
        "    %s guard; guard.owner = *m;\n"
        "    guard.valptr = (%s *)Iron_mutex_lock(*m);\n"
        "    return guard;\n"
        "}\n"
        "static %s Iron_MutexGuard_%s_get(%s *g) {\n"
        "    return *g->valptr;\n"
        "}\n"
        "static void Iron_MutexGuard_%s_set(%s *g, %s value) {\n"
        "    *g->valptr = value;\n"
        "}\n"
        "static void Iron_MutexGuard_%s_unlock(%s *g) {\n"
        "    if (g && g->owner) { Iron_mutex_unlock(g->owner); g->owner = NULL; }\n"
        "}\n\n",
        /* _lock */   guard_name, esc, guard_name, elem_c,
        /* _get */    elem_c, esc, guard_name,
        /* _set */    esc, guard_name, elem_c,
        /* _unlock */ esc, guard_name);
}

void emit_ensure_channel(EmitCtx *ctx, const Iron_Type *elem_type) {
    if (!elem_type) return;
    const char *elem_c = emit_type_to_c(elem_type, ctx);
    if (!elem_c) return;
    const char *esc = emit_elem_c_escaped(ctx, elem_type);
    if (!esc) return;

    /* Dedupe key: the escaped element suffix. */
    for (int i = 0; i < (int)arrlen(ctx->emitted_channels); i++) {
        if (strcmp(ctx->emitted_channels[i], esc) == 0) return;
    }
    char *esc_copy = iron_arena_strdup(ctx->arena, esc, strlen(esc));
    if (!esc_copy) iron_oom_abort("emit_helpers.c:emit_ensure_channel esc_copy");
    arrput(ctx->emitted_channels, esc_copy);

    /* send heap-boxes the value (the runtime ring stores void*); recv unboxes
     * and frees the box. The element-agnostic int64->int capacity wrapper is
     * emitted once via the emitted_channels guard (first instantiation wins). */
    if (arrlen(ctx->emitted_channels) == 1) {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static Iron_Channel *Iron_channel_create_i64(int64_t capacity) {\n"
            "    return Iron_channel_create((int)capacity);\n"
            "}\n");
    }
    /* Phase 37 M5: element-drop-aware destroy. When T has drop-need,
     * synthesize a void* trampoline over the element destructor and thread
     * it through Iron_channel_destroy_with so still-queued (undelivered)
     * elements release their resources at Channel destroy (previously the
     * boxes were freed without running T's drop). recv keeps move-out
     * semantics: the receiver owns the value, no drop there. Trivially-
     * destructible T keeps the historical glue byte-identical. */
    bool ch_elem_drop = emit_container_elem_drop_need(ctx, elem_type);
    if (ch_elem_drop) {
        emit_container_elem_ensure_dtor(ctx, elem_type, elem_c);
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "/* Phase 37 M5: Channel[%s] element drop trampoline */\n"
            "static void Iron_Channel_%s_elem_drop(void *p) {\n",
            elem_c, esc);
        if (elem_type->kind == IRON_TYPE_RC) {
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    iron_rc_release(*(void **)p);\n");
        } else {
            iron_strbuf_appendf(&ctx->lifted_funcs,
                "    %s_drop((%s *)p);\n", elem_c, elem_c);
        }
        iron_strbuf_appendf(&ctx->lifted_funcs, "}\n");
    }

    iron_strbuf_appendf(&ctx->lifted_funcs,
        "/* Phase 33 STDLIB-08: Channel[%s] per-T glue */\n"
        "static void Iron_Channel_%s_send(Iron_Channel **ch, %s value) {\n"
        "    %s *box = (%s *)malloc(sizeof(%s));\n"
        "    if (!box) iron_oom_abort(\"Channel send\");\n"
        "    *box = value;\n"
        "    Iron_channel_send(*ch, box);\n"
        "}\n"
        "static %s Iron_Channel_%s_recv(Iron_Channel **ch) {\n"
        "    %s *box = (%s *)Iron_channel_recv(*ch);\n"
        "    %s out; memset(&out, 0, sizeof(out));\n"
        "    if (box) { out = *box; free(box); }\n"
        "    return out;\n"
        "}\n",
        /* comment */ elem_c,
        /* _send */   esc, elem_c, elem_c, elem_c, elem_c,
        /* _recv */   elem_c, esc, elem_c, elem_c, elem_c);
    if (ch_elem_drop) {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void Iron_Channel_%s_destroy(Iron_Channel **ch) {\n"
            "    if (ch && *ch) { Iron_channel_destroy_with(*ch, Iron_Channel_%s_elem_drop); *ch = NULL; }\n"
            "}\n\n",
            esc, esc);
    } else {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void Iron_Channel_%s_destroy(Iron_Channel **ch) {\n"
            "    if (ch && *ch) { Iron_channel_destroy(*ch); *ch = NULL; }\n"
            "}\n\n",
            /* _destroy */ esc);
    }
}

void emit_ensure_rwlock(EmitCtx *ctx, const Iron_Type *elem_type) {
    if (!elem_type) return;
    const char *elem_c = emit_type_to_c(elem_type, ctx);
    if (!elem_c) return;
    const char *esc = emit_elem_c_escaped(ctx, elem_type);
    if (!esc) return;

    /* Lock typedef name: Iron_RWLock_<esc> */
    Iron_StrBuf lsb = iron_strbuf_create(48);
    iron_strbuf_appendf(&lsb, "Iron_RWLock_%s", esc);
    const char *lock_name = iron_arena_strdup(ctx->arena, iron_strbuf_get(&lsb), lsb.len);
    iron_strbuf_free(&lsb);
    if (!lock_name) iron_oom_abort("emit_helpers.c:emit_ensure_rwlock lock_name");

    for (int i = 0; i < (int)arrlen(ctx->emitted_rwlocks); i++) {
        if (strcmp(ctx->emitted_rwlocks[i], lock_name) == 0) return;
    }
    char *name_copy = iron_arena_strdup(ctx->arena, lock_name, strlen(lock_name));
    if (!name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_rwlock name_copy");
    arrput(ctx->emitted_rwlocks, name_copy);

    /* Typedefs (lock + read/write guards) into struct_bodies. */
    iron_strbuf_appendf(&ctx->struct_bodies,
        "/* Phase 33 STDLIB-07: RWLock[%s] per-T glue */\n"
        "typedef struct { iron_rwlock_t lk; %s value; } %s;\n"
        "typedef struct { %s *owner; } Iron_RWReadGuard_%s;\n"
        "typedef struct { %s *owner; } Iron_RWWriteGuard_%s;\n",
        elem_c, elem_c, lock_name,
        lock_name, esc,
        lock_name, esc);

    /* Phase 37 M5: element-drop-aware destroy. RWLock is glue-only (the
     * value is embedded in the per-T lock struct, no runtime destroy fn),
     * so the destroy glue itself runs T's drop on the value before free.
     * Trivially-destructible T keeps the historical glue byte-identical. */
    bool rw_elem_drop = emit_container_elem_drop_need(ctx, elem_type);
    if (rw_elem_drop) emit_container_elem_ensure_dtor(ctx, elem_type, elem_c);

    iron_strbuf_appendf(&ctx->lifted_funcs,
        "static %s *Iron_RWLock_%s_new(%s value) {\n"
        "    %s *l = (%s *)malloc(sizeof(%s));\n"
        "    if (!l) iron_oom_abort(\"RWLock new\");\n"
        "    IRON_RWLOCK_INIT(l->lk); l->value = value; return l;\n"
        "}\n",
        /* _new */      lock_name, esc, elem_c, lock_name, lock_name, lock_name);
    if (rw_elem_drop && elem_type->kind == IRON_TYPE_RC) {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void Iron_RWLock_%s_destroy(%s **l) {\n"
            "    /* Phase 37 M5: release the parked rc element before free */\n"
            "    if (l && *l) {\n"
            "        iron_rc_release((void *)(*l)->value);\n"
            "        IRON_RWLOCK_DESTROY((*l)->lk); free(*l); *l = NULL;\n"
            "    }\n"
            "}\n",
            esc, lock_name);
    } else if (rw_elem_drop) {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void Iron_RWLock_%s_destroy(%s **l) {\n"
            "    /* Phase 37 M5: run the element drop on the parked value before free */\n"
            "    if (l && *l) {\n"
            "        %s_drop(&(*l)->value);\n"
            "        IRON_RWLOCK_DESTROY((*l)->lk); free(*l); *l = NULL;\n"
            "    }\n"
            "}\n",
            esc, lock_name, elem_c);
    } else {
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void Iron_RWLock_%s_destroy(%s **l) {\n"
            "    if (l && *l) { IRON_RWLOCK_DESTROY((*l)->lk); free(*l); *l = NULL; }\n"
            "}\n",
            /* _destroy */  esc, lock_name);
    }
    iron_strbuf_appendf(&ctx->lifted_funcs,
        "static Iron_RWReadGuard_%s Iron_RWReadGuard_%s_read(%s **l) {\n"
        "    Iron_RWReadGuard_%s g; g.owner = *l;\n"
        "    IRON_RWLOCK_RDLOCK((*l)->lk); return g;\n"
        "}\n"
        "static Iron_RWWriteGuard_%s Iron_RWWriteGuard_%s_write(%s **l) {\n"
        "    Iron_RWWriteGuard_%s g; g.owner = *l;\n"
        "    IRON_RWLOCK_WRLOCK((*l)->lk); return g;\n"
        "}\n"
        "static %s Iron_RWReadGuard_%s_get(Iron_RWReadGuard_%s *g) {\n"
        "    return g->owner->value;\n"
        "}\n"
        "static %s Iron_RWWriteGuard_%s_get(Iron_RWWriteGuard_%s *g) {\n"
        "    return g->owner->value;\n"
        "}\n"
        "static void Iron_RWWriteGuard_%s_set(Iron_RWWriteGuard_%s *g, %s value) {\n"
        "    g->owner->value = value;\n"
        "}\n"
        "static void Iron_RWReadGuard_%s_rdunlock(Iron_RWReadGuard_%s *g) {\n"
        "    if (g && g->owner) { IRON_RWLOCK_RDUNLOCK(g->owner->lk); g->owner = NULL; }\n"
        "}\n"
        "static void Iron_RWWriteGuard_%s_wrunlock(Iron_RWWriteGuard_%s *g) {\n"
        "    if (g && g->owner) { IRON_RWLOCK_WRUNLOCK(g->owner->lk); g->owner = NULL; }\n"
        "}\n\n",
        /* _read */     esc, esc, lock_name, esc,
        /* _write */    esc, esc, lock_name, esc,
        /* read_get */  elem_c, esc, esc,
        /* write_get */ elem_c, esc, esc,
        /* write_set */ esc, esc, elem_c,
        /* rdunlock */  esc, esc,
        /* wrunlock */  esc, esc);
}

void emit_ensure_filehandle(EmitCtx *ctx) {
    if (ctx->emitted_filehandle) return;
    ctx->emitted_filehandle = true;

    iron_strbuf_appendf(&ctx->struct_bodies,
        "/* Phase 33 STDLIB-09: FileHandle nocopy fd wrapper */\n"
        "typedef struct { int fd; } Iron_FileHandle;\n"
        /* Phase 33 STDLIB-02 (Plan 33-04): forward prototypes so a
         * Iron_List_Iron_FileHandle _free emitted into struct_bodies (which
         * renders before lifted_funcs) can call the per-element drop without
         * an implicit-declaration error. */
        "static Iron_FileHandle Iron_FileHandle_open(Iron_String path);\n"
        "static void Iron_FileHandle_close(Iron_FileHandle *fh);\n"
        "static void Iron_FileHandle_drop(Iron_FileHandle *fh);\n");

    /* open creates/truncates the file; close prints "closed fd" then closes.
     * Uses fopen/fileno (portable, no <fcntl.h> needed) — the value is the
     * underlying fd so the drop path matches the surface contract. */
    iron_strbuf_appendf(&ctx->lifted_funcs,
        "static Iron_FileHandle Iron_FileHandle_open(Iron_String path) {\n"
        "    Iron_FileHandle fh; fh.fd = -1;\n"
        "    const char *p = iron_string_cstr(&path);\n"
        "    FILE *f = fopen(p ? p : \"\", \"w\");\n"
        "    if (f) fh.fd = fileno(f);\n"
        "    return fh;\n"
        "}\n"
        "static void Iron_FileHandle_close(Iron_FileHandle *fh) {\n"
        "    if (fh && fh->fd >= 0) {\n"
        "        printf(\"closed fd\\n\");\n"
        "        close(fh->fd);\n"
        "        fh->fd = -1;\n"
        "    }\n"
        "}\n"
        "static void Iron_FileHandle_drop(Iron_FileHandle *fh) {\n"
        "    Iron_FileHandle_close(fh);\n"
        "}\n\n");
}

/* Phase 33 STDLIB-10 (Plan 33-06): per-T RawPtr.of helper synthesis.
 * Emits Iron_RawPtr_of_<elemC>(<elemC>*) -> int64_t* into lifted_funcs.
 * Body is a single cast: returns its argument re-typed as int64_t*. RawPtr
 * is the type-erased member of the *unchecked T regime; internally we
 * represent it as IRON_TYPE_PTR { is_unchecked=true, pointee=Int } (a bare
 * 8B int64_t* in C). The per-T helper exists so the SIGNATURE matches the
 * call site (the call site passes &x typed as <elemC>*, not void*, which
 * keeps -Wpedantic / -Werror clean across element types). Idempotent via
 * emitted_rawptrs (mirrors emit_ensure_box's dedup pattern). */
void emit_ensure_rawptr(EmitCtx *ctx, const Iron_Type *elem_type) {
    if (!elem_type) return;

    const char *elem_c = emit_type_to_c(elem_type, ctx);
    if (!elem_c) return;

    /* Build escaped suffix: replace ` ` and `*` with `_`. */
    Iron_StrBuf sb = iron_strbuf_create(48);
    for (const char *p = elem_c; *p; p++) {
        if (*p == ' ' || *p == '*') iron_strbuf_appendf(&sb, "_");
        else { char ch[2] = { *p, '\0' }; iron_strbuf_appendf(&sb, "%s", ch); }
    }
    const char *esc = iron_arena_strdup(ctx->arena,
                                         iron_strbuf_get(&sb), sb.len);
    iron_strbuf_free(&sb);
    if (!esc) iron_oom_abort("emit_helpers.c:emit_ensure_rawptr esc");

    /* Dedupe — Phase 25 emitted_boxes precedent. */
    for (int i = 0; i < (int)arrlen(ctx->emitted_rawptrs); i++) {
        if (strcmp(ctx->emitted_rawptrs[i], esc) == 0) return;
    }
    char *name_copy = iron_arena_strdup(ctx->arena, esc, strlen(esc));
    if (!name_copy) iron_oom_abort("emit_helpers.c:emit_ensure_rawptr name_copy");
    arrput(ctx->emitted_rawptrs, name_copy);

    /* The function pre-declaration lands in struct_bodies so call sites
     * compiled earlier in the .c output resolve cleanly (mirrors the
     * FileHandle/Box forward-prototype pattern). The body lands in
     * lifted_funcs (renders after struct_bodies). */
    iron_strbuf_appendf(&ctx->struct_bodies,
        "/* Phase 33 STDLIB-10: RawPtr.of[%s] per-T type-erasure helper */\n"
        "static int64_t *Iron_RawPtr_of_%s(%s *p);\n",
        elem_c, esc, elem_c);

    iron_strbuf_appendf(&ctx->lifted_funcs,
        "static int64_t *Iron_RawPtr_of_%s(%s *p) {\n"
        "    /* Type-erased cast — RawPtr is the unchecked-regime void*. */\n"
        "    return (int64_t *)(void *)p;\n"
        "}\n\n",
        esc, elem_c);
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
    /* Phase 28 ARENA-03 (Plan 28-04): IRON_LIR_ARENA_ALLOC also produces an
     * Iron_FatPtr local whose .addr points at the arena-allocated object, so
     * ADDR_OF / field-access through it uses the same ((T *)_vN.addr) form. */
    return instr->kind == IRON_LIR_HEAP_ALLOC ||
           instr->kind == IRON_LIR_ARENA_ALLOC;
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
    /* Phase 28 ARENA-03 (Plan 28-04): IRON_LIR_ARENA_ALLOC joins HEAP_ALLOC /
     * ADDR_OF as an Iron_FatPtr-producing opcode. */
    return instr->kind == IRON_LIR_HEAP_ALLOC ||
           instr->kind == IRON_LIR_ARENA_ALLOC ||
           instr->kind == IRON_LIR_ADDR_OF;
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
    /* Phase 28 ARENA-03 (Plan 28-04): arena alloc pointee type is instr->type,
     * exactly like a heap alloc. */
    if (instr->kind == IRON_LIR_HEAP_ALLOC ||
        instr->kind == IRON_LIR_ARENA_ALLOC) {
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
