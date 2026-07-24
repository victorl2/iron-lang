/* emit_structs.c -- Struct body, tagged union, and type declaration emission.
 *
 * Extracted from emit_c.c (Phase 52, Plan 02).
 *
 * Contains:
 *   - Topological sort for type declarations (IrTopoState)
 *   - Object struct body emission (emit_object_struct_body)
 *   - Interface tagged union generation (tag enums, data unions, constructors)
 *   - Split collection struct generation (per-type sub-arrays, push/free)
 *   - Enum and ADT enum layout emission
 *   - emit_type_decls() orchestrator (called from iron_lir_emit_c)
 */

#include "lir/emit_structs.h"
#include "lir/emit_split.h"
#include "parser/ast.h"
#include "vendor/stb_ds.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* PROT-03 layout guard (Phase 66 Plan 05, AUDIT-01 row 27): emit_structs.c
 * casts entries from `Iron_ObjectDecl::fields` (a `void**`) to `Iron_Field *`
 * and from `Iron_Field::type_ann` (a `Iron_Node *`) to `Iron_TypeAnnotation *`.
 * The invariant is established by the parser: every fields[] entry is
 * allocated as an Iron_Field, and every f->type_ann (when non-NULL) is
 * allocated as an Iron_TypeAnnotation. The _Static_asserts below make the
 * layout assumption grep-visible — if a future change alters how fields[]
 * is populated (e.g., heterogeneous entries) or shrinks Iron_Field /
 * Iron_TypeAnnotation to a degenerate empty struct, the cast sites in this
 * file must be revisited. The asserts also pin Iron_TypeAnnotation as a
 * "real" Iron_Node derivative whose first member is Iron_NodeKind kind. */
_Static_assert(sizeof(Iron_Field) > 0, "Iron_Field layout sanity check");
_Static_assert(sizeof(Iron_TypeAnnotation) > 0, "Iron_TypeAnnotation layout sanity check");

/* ── Topological sort for IR type declarations ─────────────────────────────── */

#define IR_TOPO_WHITE 0
#define IR_TOPO_GRAY  1
#define IR_TOPO_BLACK 2

typedef struct {
    IronLIR_TypeDecl **sorted; /* stb_ds array */
    IronLIR_Module    *module;
    int              *colors;
    bool              has_cycle;
} IrTopoState;

/* Phase 33 OQ-02 / box-arena-codegen unblock: a small set of stdlib SURFACE
 * object decls (arena.iron's `object Arena` / `object ArenaSave`, box.iron's
 * `nocopy object Box[T]`) are Iron-side stand-ins for types the C RUNTIME
 * already defines in its own headers (src/util/arena.h `Iron_Arena`,
 * src/runtime/iron_arena_rt.h `Iron_ArenaSave`, and the emit_ensure_box-
 * synthesized Iron_Box storage). Emitting a forward typedef + a struct body
 * for these collides with the runtime-owned definitions the generated C
 * #includes — `typedef redefinition with different types ('struct Iron_Arena'
 * vs 'Iron_Arena')`, which fails clang for EVERY compilation (box.iron +
 * arena.iron are unconditionally prepended). Skip both the forward decl and
 * the struct body for these names so the runtime header owns the layout.
 * Keyed on the surface type NAME (pre-mangle); the matching C typedefs are
 * Iron_Arena / Iron_ArenaSave / Iron_Box. */
static bool ir_is_runtime_provided_type(const char *name) {
    if (!name) return false;
    return strcmp(name, "Arena")     == 0 ||
           strcmp(name, "ArenaSave") == 0 ||
           strcmp(name, "Box")       == 0 ||
           /* Phase 33 STDLIB-07/08/09 (Plan 33-05): the nocopy resource-type
            * surfaces are Iron-side stand-ins. Mutex / Channel collide with the
            * runtime-owned Iron_Mutex / Iron_Channel typedefs in iron_runtime.h;
            * RWLock + the *Guard surfaces have no plain C struct (their real
            * storage is emit_ensure_*-synthesized on the positive path). Skip
            * the forward decl + struct body for all of them so the always-
            * prepended surfaces never break a compilation. */
           strcmp(name, "Mutex")        == 0 ||
           strcmp(name, "MutexGuard")   == 0 ||
           strcmp(name, "RWLock")       == 0 ||
           strcmp(name, "RWReadGuard")  == 0 ||
           strcmp(name, "RWWriteGuard") == 0 ||
           strcmp(name, "Channel")      == 0 ||
           strcmp(name, "FileHandle")   == 0;
}

/* Find object type_decl index by type name */
static int find_ir_type_decl_idx(IronLIR_Module *module, const char *name) {
    for (int i = 0; i < module->type_decl_count; i++) {
        if (module->type_decls[i]->kind == IRON_LIR_TYPE_OBJECT &&
            strcmp(module->type_decls[i]->name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void ir_topo_visit(IrTopoState *state, int idx) {
    if (state->colors[idx] == IR_TOPO_BLACK) return;
    if (state->colors[idx] == IR_TOPO_GRAY) {
        state->has_cycle = true;
        return;
    }

    state->colors[idx] = IR_TOPO_GRAY;

    IronLIR_TypeDecl *td = state->module->type_decls[idx];
    if (td->kind == IRON_LIR_TYPE_OBJECT && td->type &&
        td->type->kind == IRON_TYPE_OBJECT && td->type->object.decl) {
        Iron_ObjectDecl *od = td->type->object.decl;

        /* Visit parent first */
        if (od->extends_name) {
            int dep = find_ir_type_decl_idx(state->module, od->extends_name);
            if (dep >= 0) ir_topo_visit(state, dep);
        }

        /* Visit value-type field dependencies */
        for (int i = 0; i < od->field_count; i++) {
            /* PROT-03 row 27 (AUDIT-01 M-severity): loop-bound + non-NULL
             * assert before the Iron_Field cast from the void** fields array
             * in the topological-sort field walker. */
            assert(i >= 0 && i < od->field_count);
            assert(od->fields[i] != NULL);
            Iron_Field *f = (Iron_Field *)od->fields[i];
            if (!f->type_ann) continue;
            /* PROT-03 row 28 (AUDIT-01 M-severity): assert kind on
             * f->type_ann before the Iron_TypeAnnotation cast — TypeAnnotation
             * IS an Iron_Node-derived sub-struct (first field is kind). */
            IRON_NODE_ASSERT_KIND(f->type_ann, IRON_NODE_TYPE_ANNOTATION);
            Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
            if (ta->is_nullable) continue;
            int dep = find_ir_type_decl_idx(state->module, ta->name);
            if (dep >= 0 && dep != idx) ir_topo_visit(state, dep);
        }
    }

    state->colors[idx] = IR_TOPO_BLACK;
    arrput(state->sorted, td);
}

/* ── Phase 56: Monomorphic list type decl emission ──────────────────────
 * Emit Iron_List_<mangled> struct typedef plus IRON_LIST_DECL and
 * IRON_LIST_IMPL macro expansions for every concrete object type that
 * appears as an array element type somewhere in the module.  Dedups via
 * a file-local stb_ds hash set keyed on the mangled type name.
 *
 * WHY this scan exists: Phase 49 mono collapse (emit_c.c:4797-4814)
 * removes single-type collections from split_collection_ids and lets them
 * fall through to the plain-typed-array codegen path (emit_c.c:3066-3096)
 * which references Iron_List_Iron_<Type> symbols.  Only primitive list
 * types are pre-declared in iron_runtime.h:640-645 (int64_t, int32_t,
 * double, bool, Iron_String, Iron_Closure); concrete object types were
 * never declared, and clang failed with
 *   "use of undeclared identifier 'Iron_List_Iron_Circle'".
 *
 * WHY we scan ARRAY_LIT elem_types directly instead of iterating
 * ctx->monomorphic_collections: the mono detection scan at
 * emit_c.c:5378 only runs against collections that were first added to
 * ctx->split_collection_ids (which requires interface-typed array
 * literals).  When Iron's type inference gives a literal a concrete
 * object type directly — e.g. `val circles = [Circle(1), Circle(2)]`
 * resolves to `[Circle]` (concrete) rather than `[Shape]` (interface) —
 * the ARRAY_LIT is NEVER added to split_collection_ids and therefore
 * NEVER added to monomorphic_collections either, yet the plain-typed-
 * array codegen path still emits `Iron_List_Iron_Circle_create()`.  The
 * robust fix is to scan the module's ARRAY_LIT instructions directly
 * and emit a decl for every concrete object element type we find,
 * regardless of whether mono detection saw it.
 *
 * Must be called at the END of emit_type_decls() so that the Iron_<Type>
 * struct body has already been emitted by emit_object_struct_body;
 * IRON_LIST_DECL and IRON_LIST_IMPL expand into function prototypes and
 * bodies that reference Iron_<Type> as a complete type. */
/* ── Phase 33 STDLIB-02 (Plan 33-04): element-destructor-aware list lifecycle ──
 *
 * Emit the IRON_LIST_DECL + the list IMPL for a mono-list whose mangled name is
 * `mangled` (e.g. "Iron_Tracked") and whose element C type is also `mangled`.
 *
 * `has_drop`  : the element type has a per-element destructor named
 *               "<mangled>_drop(<mangled> *)".  When true, _free iterates and
 *               calls it on every element BEFORE free(items) (Pattern 4).
 * `has_copy`  : the element type has a per-element copy hook named
 *               "<mangled>_copy(<mangled> *dest, const <mangled> *src)".  When
 *               true, _clone deep-copies each element instead of bulk memcpy.
 *
 * When neither flag is set this collapses to IRON_LIST_IMPL (the fast path:
 * free(items) / memcpy — Pitfall 5: no spurious per-element calls for
 * primitives / trivial structs).
 *
 * The DECL + struct typedef are the caller's responsibility (emitted just
 * before calling this); this routine appends only the IMPL bodies. */
static void emit_list_impl_lifecycle(EmitCtx *ctx, const char *mangled,
                                     bool has_drop, bool has_copy) {
    if (!has_drop && !has_copy) {
        iron_strbuf_appendf(&ctx->struct_bodies,
            "IRON_LIST_IMPL(%s, %s)\n\n", mangled, mangled);
        return;
    }

    /* Forward prototypes: the per-element <mangled>_drop / _copy helpers are
     * synthesized into ctx->lifted_funcs (which renders AFTER struct_bodies),
     * but the list _free/_clone bodies below live in struct_bodies and call
     * them — declare them first to avoid implicit-declaration / static-after-
     * nonstatic errors. */
    if (has_drop) {
        iron_strbuf_appendf(&ctx->struct_bodies,
            "static void %s_drop(%s *self);\n", mangled, mangled);
    }
    if (has_copy) {
        iron_strbuf_appendf(&ctx->struct_bodies,
            "static void %s_copy(%s *dest, const %s *src);\n",
            mangled, mangled, mangled);
    }

    /* Core surface (create/push/get/set/pop/len) is always the macro body. */
    iron_strbuf_appendf(&ctx->struct_bodies,
        "IRON_LIST_IMPL_CORE(%s, %s)\n", mangled, mangled);

    /* Custom _clone: deep element copy when the element has a copy hook,
     * else the trivial memcpy body. */
    if (has_copy) {
        iron_strbuf_appendf(&ctx->struct_bodies,
            "/* Phase 33 STDLIB-02: element-copy _clone for %s */\n"
            "Iron_List_%s Iron_List_%s_clone(const Iron_List_%s *src) {\n"
            "    Iron_List_%s dst;\n"
            "    dst.count = src->count;\n"
            "    dst.capacity = src->count;\n"
            "    if (src->count > 0) {\n"
            "        dst.items = (%s *)malloc((size_t)src->count * sizeof(%s));\n"
            "        if (!dst.items) iron_oom_abort(\"Iron_List_%s_clone\");\n"
            "        for (int64_t _i = 0; _i < src->count; _i++) {\n"
            "            %s_copy(&dst.items[_i], &src->items[_i]);\n"
            "        }\n"
            "    } else {\n"
            "        dst.items = NULL;\n"
            "    }\n"
            "    return dst;\n"
            "}\n",
            mangled,
            mangled, mangled, mangled,
            mangled,
            mangled, mangled,
            mangled,
            mangled);
    } else {
        /* nocopy / drop-only element: keep memcpy clone (the binding-copy is a
         * compile error upstream for nocopy types, but the _clone symbol must
         * still exist to satisfy the link). */
        iron_strbuf_appendf(&ctx->struct_bodies,
            "Iron_List_%s Iron_List_%s_clone(const Iron_List_%s *src) {\n"
            "    Iron_List_%s dst;\n"
            "    dst.count = src->count;\n"
            "    dst.capacity = src->count;\n"
            "    if (src->count > 0) {\n"
            "        dst.items = (%s *)malloc((size_t)src->count * sizeof(%s));\n"
            "        if (!dst.items) iron_oom_abort(\"Iron_List_%s_clone\");\n"
            "        memcpy(dst.items, src->items, (size_t)src->count * sizeof(%s));\n"
            "    } else {\n"
            "        dst.items = NULL;\n"
            "    }\n"
            "    return dst;\n"
            "}\n",
            mangled, mangled, mangled,
            mangled,
            mangled, mangled,
            mangled,
            mangled);
    }

    /* Custom _free: per-element destructor loop (Pattern 4) when has_drop,
     * else trivial free(items). */
    if (has_drop) {
        iron_strbuf_appendf(&ctx->struct_bodies,
            "/* Phase 33 STDLIB-02: per-element destructor _free for %s */\n"
            "void Iron_List_%s_free(Iron_List_%s *self) {\n"
            "    if (self->items) {\n"
            "        for (int64_t _i = 0; _i < self->count; _i++) {\n"
            "            %s_drop(&self->items[_i]);\n"
            "        }\n"
            "    }\n"
            "    free(self->items);\n"
            "    self->items = NULL; self->count = 0; self->capacity = 0;\n"
            "}\n\n",
            mangled,
            mangled, mangled,
            mangled);
    } else {
        iron_strbuf_appendf(&ctx->struct_bodies,
            "void Iron_List_%s_free(Iron_List_%s *self) {\n"
            "    free(self->items);\n"
            "    self->items = NULL; self->count = 0; self->capacity = 0;\n"
            "}\n\n",
            mangled, mangled);
    }
}

/* Detect whether a synthesized "<mangled>_drop" / "<mangled>_copy" function
 * applies to the given element object type.  Covers BOTH the user-object path
 * (od_has_drop_lir / has_user_copy) AND the nocopy resource surfaces
 * (FileHandle), whose drop is emit-synthesized by emit_ensure_filehandle
 * rather than lowered through od_has_drop_lir. */
static void elem_lifecycle_flags(EmitCtx *ctx, Iron_Type *et,
                                 const char *bare_name,
                                 bool *out_has_drop, bool *out_has_copy) {
    bool has_drop = false, has_copy = false;
    if (bare_name && strcmp(bare_name, "FileHandle") == 0) {
        /* nocopy fd wrapper: drop closes the fd; no element copy. */
        has_drop = true;
    } else if (et && et->kind == IRON_TYPE_OBJECT && et->object.decl) {
        struct Iron_ObjectDecl *od = et->object.decl;
        if (od_has_drop_lir(ctx, od)) has_drop = true;
        if (!od->is_nocopy && et->has_user_copy_cached &&
            et->has_user_copy_transitive) {
            has_copy = true;
        }
    }
    *out_has_drop = has_drop;
    *out_has_copy = has_copy;
}

static void emit_mono_list_decls(EmitCtx *ctx) {
    IronLIR_Module *module = ctx->module;
    if (!module) return;

    /* Dedup set: keyed by mangled concrete type name (e.g. "Iron_Circle").
     * Per-compilation-unit scope — freed at end of function. */
    struct { char *key; bool value; } *emitted_mono_list_types = NULL;

    /* Helper lambda via loop body: emit decls for a single concrete type. */
    /* We iterate all ARRAY_LIT instructions in every function and collect
     * their elem_type if it's a concrete object type.  The instruction's
     * elem_type field is set during LIR building and matches what the
     * codegen uses at emit_c.c:3069 to compute the Iron_List_<suffix> name
     * via emit_type_to_c(). */
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (!fn || fn->is_extern || fn->block_count == 0) continue;

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in = blk->instrs[ii];
                if (in->kind != IRON_LIR_ARRAY_LIT) continue;
                Iron_Type *et = in->array_lit.elem_type;
                if (!et) continue;
                /* Only concrete object element types need a decl from us.
                 * Interface-typed arrays go through Iron_SplitList_<Iface>
                 * which is already emitted by emit_split_collection_for_iface.
                 * Primitive element types use the pre-declared list types
                 * in iron_runtime.h:640-645. */
                if (et->kind != IRON_TYPE_OBJECT) continue;
                if (!et->object.decl) continue;

                const char *bare_type = et->object.decl->name;
                if (!bare_type) continue;

                /* Mangle "Circle" -> "Iron_Circle".  Arena-allocated,
                 * stable for the lifetime of the stb_ds dedup map. */
                const char *mangled = emit_mangle_name(bare_type, ctx->arena);

                if (shgeti(emitted_mono_list_types, mangled) >= 0) continue;
                shput(emitted_mono_list_types, mangled, true);

                /* Phase 33 STDLIB-09 (Plan 33-04): for the FileHandle nocopy
                 * surface the Iron_FileHandle typedef + Iron_FileHandle_drop are
                 * emit-synthesized lazily — ensure they land in struct_bodies
                 * BEFORE the Iron_List_Iron_FileHandle typedef references them
                 * (Pitfall 5 ordering). */
                if (strcmp(bare_type, "FileHandle") == 0) {
                    emit_ensure_filehandle(ctx);
                }

                /* Phase 33 STDLIB-02 (Plan 33-04): determine element drop/copy
                 * so the list _free/_clone can run per-element destructors. */
                bool elem_has_drop = false, elem_has_copy = false;
                elem_lifecycle_flags(ctx, et, bare_type,
                                     &elem_has_drop, &elem_has_copy);
                /* Synthesize the per-object drop/copy helpers the list bodies
                 * call (no-op for FileHandle whose drop is already emitted). */
                if (elem_has_drop && strcmp(bare_type, "FileHandle") != 0) {
                    emit_ensure_drop(ctx, mangled, et->object.decl);
                }
                if (elem_has_copy) {
                    emit_ensure_copy(ctx, mangled, et->object.decl);
                }

                /* Emit Iron_List_<mangled> struct typedef.  The
                 * IRON_LIST_DECL and the IMPL bodies assume this struct is
                 * already declared with fields
                 *   { T *items; int64_t count; int64_t capacity; }. */
                iron_strbuf_appendf(&ctx->struct_bodies,
                    "/* Phase 56: Iron_List type for mono-collapsed %s */\n"
                    "typedef struct Iron_List_%s {\n"
                    "    %s    *items;\n"
                    "    int64_t count;\n"
                    "    int64_t capacity;\n"
                    "} Iron_List_%s;\n",
                    mangled, mangled, mangled, mangled);

                /* Emit IRON_LIST_DECL(T, suffix) — function prototypes. */
                iron_strbuf_appendf(&ctx->struct_bodies,
                    "IRON_LIST_DECL(%s, %s)\n",
                    mangled, mangled);

                /* Emit the IMPL — element-destructor-aware when the element
                 * type owns a drop/copy, else the fast free(items)/memcpy path
                 * (Pitfall 5).  Safe at TU level: each mangled name is unique
                 * per compilation unit. */
                emit_list_impl_lifecycle(ctx, mangled,
                                         elem_has_drop, elem_has_copy);
            }
        }
    }
    /* Phase 23 OQ-10: handle ARRAY_LIT whose elem_type is a bounded vector
     * ([T; <=N]) — i.e. List[[T; <=N]].  The ARRAY_LIT scan above only
     * handles IRON_TYPE_OBJECT elem types.  For bounded-vector elem types
     * we must (a) call emit_ensure_bvec FIRST so Iron_BVec_T_N typedef
     * lands in struct_bodies before Iron_List_Iron_BVec_T_N references it
     * (Pitfall 5 mitigation), then (b) emit the Iron_List_Iron_BVec_T_N
     * struct typedef + IRON_LIST_DECL + IRON_LIST_IMPL.
     *
     * The dedup key is the mangled bvec name (e.g. "Iron_BVec_int64_t_4")
     * so duplicate ARRAY_LIT instructions for the same (T, N) only emit
     * one set of declarations. */
    for (int fi2 = 0; fi2 < module->func_count; fi2++) {
        IronLIR_Func *fn2 = module->funcs[fi2];
        if (!fn2 || fn2->is_extern || fn2->block_count == 0) continue;

        for (int bi2 = 0; bi2 < fn2->block_count; bi2++) {
            IronLIR_Block *blk2 = fn2->blocks[bi2];
            for (int ii2 = 0; ii2 < blk2->instr_count; ii2++) {
                IronLIR_Instr *in2 = blk2->instrs[ii2];
                if (in2->kind != IRON_LIR_ARRAY_LIT) continue;
                Iron_Type *et2 = in2->array_lit.elem_type;
                if (!et2) continue;
                /* Only bounded-vector element types are handled here;
                 * IRON_TYPE_OBJECT is handled by the scan above. */
                if (et2->kind != IRON_TYPE_ARRAY) continue;
                if (!et2->array.is_bounded || et2->array.size < 0) continue;

                /* Step A: ensure Iron_BVec_T_N typedef is emitted FIRST
                 * (Pitfall 5: typedef must precede Iron_List_Iron_BVec_T_N). */
                emit_ensure_bvec(ctx, et2);

                /* Step B: compute mangled bvec name for the list dedup key.
                 * Must match emit_ensure_bvec naming: Iron_BVec_<elem_c>_<N>
                 * with spaces and * replaced by _. */
                const char *inner_c = emit_type_to_c(et2->array.elem, ctx);
                Iron_StrBuf bvec_sb = iron_strbuf_create(64);
                iron_strbuf_appendf(&bvec_sb, "Iron_BVec_");
                for (const char *cp = inner_c; *cp; cp++) {
                    if (*cp == ' ' || *cp == '*') {
                        iron_strbuf_appendf(&bvec_sb, "_");
                    } else {
                        char cc[2] = { *cp, '\0' };
                        iron_strbuf_appendf(&bvec_sb, "%s", cc);
                    }
                }
                iron_strbuf_appendf(&bvec_sb, "_%d", et2->array.size);
                const char *bvec_name = iron_arena_strdup(ctx->arena,
                    iron_strbuf_get(&bvec_sb), bvec_sb.len);
                iron_strbuf_free(&bvec_sb);
                if (!bvec_name)
                    iron_oom_abort("emit_structs.c:emit_mono_list_decls bvec_name");

                if (shgeti(emitted_mono_list_types, bvec_name) >= 0) continue;
                shput(emitted_mono_list_types, bvec_name, true);

                /* Step C: emit Iron_List_Iron_BVec_T_N struct typedef +
                 * IRON_LIST_DECL + IRON_LIST_IMPL.  The list stores bvec
                 * structs by value (VEC-01 inline-storage guarantee). */
                iron_strbuf_appendf(&ctx->struct_bodies,
                    "/* Phase 23 OQ-10: Iron_List for bounded vector element %s */\n"
                    "typedef struct Iron_List_%s {\n"
                    "    %s    *items;\n"
                    "    int64_t count;\n"
                    "    int64_t capacity;\n"
                    "} Iron_List_%s;\n",
                    bvec_name, bvec_name, bvec_name, bvec_name);

                iron_strbuf_appendf(&ctx->struct_bodies,
                    "IRON_LIST_DECL(%s, %s)\n",
                    bvec_name, bvec_name);

                iron_strbuf_appendf(&ctx->struct_bodies,
                    "IRON_LIST_IMPL(%s, %s)\n\n",
                    bvec_name, bvec_name);
            }
        }
    }

    /* Also iterate ctx->monomorphic_collections to pick up any concrete
     * types that arrived via Phase 49/53 mono collapse (interface-typed
     * ARRAY_LIT that got collapsed to a concrete type at the Phase 49
     * detection scan).  These may not show up in the ARRAY_LIT scan above
     * because their elem_type is still IRON_TYPE_INTERFACE. */
    if (ctx->monomorphic_collections) {
        for (ptrdiff_t i = 0; i < hmlen(ctx->monomorphic_collections); i++) {
            const char *bare_type = ctx->monomorphic_collections[i].value;
            if (!bare_type) continue;

            const char *mangled = emit_mangle_name(bare_type, ctx->arena);

            if (shgeti(emitted_mono_list_types, mangled) >= 0) continue;
            shput(emitted_mono_list_types, mangled, true);

            iron_strbuf_appendf(&ctx->struct_bodies,
                "/* Phase 56: Iron_List type for mono-collapsed %s (via monomorphic_collections) */\n"
                "typedef struct Iron_List_%s {\n"
                "    %s    *items;\n"
                "    int64_t count;\n"
                "    int64_t capacity;\n"
                "} Iron_List_%s;\n",
                mangled, mangled, mangled, mangled);

            iron_strbuf_appendf(&ctx->struct_bodies,
                "IRON_LIST_DECL(%s, %s)\n",
                mangled, mangled);

            iron_strbuf_appendf(&ctx->struct_bodies,
                "IRON_LIST_IMPL(%s, %s)\n\n",
                mangled, mangled);
        }
    }

    /* Plan 63-04: also scan every extern decl AND every foreign-method
     * stub (is_extern funcs without extern_c_name, emitted as
     * prototypes at emit_c.c Phase 3) parameter and return type.
     * When user code never constructs a [T] literal and never triggers
     * mono collapse on it, the previous two scans miss the type, yet
     * the emitted prototype still references Iron_List_Iron_<T> ->
     * undeclared-identifier.  Scan these surfaces so the typedef gets
     * emitted whenever a stdlib foreign-method-stub signature demands
     * it. */
    #define PLAN_63_04_EMIT_LIST_FOR(elem_type_expr, scan_label)                 \
        do {                                                                     \
            Iron_Type *__et = (elem_type_expr);                                  \
            if (__et && __et->kind == IRON_TYPE_OBJECT &&                        \
                __et->object.decl && __et->object.decl->name) {                  \
                const char *__mangled = emit_mangle_name(                        \
                    __et->object.decl->name, ctx->arena);                        \
                if (shgeti(emitted_mono_list_types, __mangled) < 0) {            \
                    shput(emitted_mono_list_types, __mangled, true);             \
                    iron_strbuf_appendf(&ctx->struct_bodies,                     \
                        "/* Phase 56: Iron_List type for mono-collapsed %s ("    \
                        scan_label ") */\n"                                      \
                        "typedef struct Iron_List_%s {\n"                        \
                        "    %s    *items;\n"                                    \
                        "    int64_t count;\n"                                   \
                        "    int64_t capacity;\n"                                \
                        "} Iron_List_%s;\n",                                     \
                        __mangled, __mangled, __mangled, __mangled);             \
                    iron_strbuf_appendf(&ctx->struct_bodies,                     \
                        "IRON_LIST_DECL(%s, %s)\n",                              \
                        __mangled, __mangled);                                   \
                    iron_strbuf_appendf(&ctx->struct_bodies,                     \
                        "IRON_LIST_IMPL(%s, %s)\n\n",                            \
                        __mangled, __mangled);                                   \
                }                                                                \
            }                                                                    \
        } while (0)

    /* Scan A: ctx->module->extern_decls (explicit `extern func` bindings
     * — raylib.iron doesn't use these today but other stdlib modules might). */
    for (int ei = 0; ei < module->extern_decl_count; ei++) {
        IronLIR_ExternDecl *ed = module->extern_decls[ei];
        if (!ed) continue;
        for (int pi = 0; pi < ed->param_count; pi++) {
            Iron_Type *pt = ed->param_types[pi];
            if (!pt || pt->kind != IRON_TYPE_ARRAY) continue;
            PLAN_63_04_EMIT_LIST_FOR(pt->array.elem, "via extern-decl param scan");
        }
        if (ed->return_type && ed->return_type->kind == IRON_TYPE_ARRAY) {
            PLAN_63_04_EMIT_LIST_FOR(ed->return_type->array.elem,
                                      "via extern-decl return scan");
        }
    }

    /* Scan B: ctx->module->funcs where fn->is_extern && !fn->extern_c_name
     * — foreign-method stubs (empty-body `func Draw.triangle_fan(points:
     * [Vector2], ...)` lowered by hir_to_lir.c). These are the prototypes
     * emitted by emit_c.c Phase 3 at line ~6718. */
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (!fn || !fn->is_extern || fn->extern_c_name) continue;
        for (int pi = 0; pi < fn->param_count; pi++) {
            Iron_Type *pt = fn->params[pi].type;
            if (!pt) continue;
            if (pt->kind == IRON_TYPE_ARRAY) {
                PLAN_63_04_EMIT_LIST_FOR(pt->array.elem,
                                          "via foreign-method-stub param scan");
            } else if (pt->kind == IRON_TYPE_TUPLE) {
                /* Plan 67-04: tuple param may contain an array element type
                 * whose Iron_List_<T> typedef must be visible before the
                 * emitted tuple typedef references it. See Font.gen_image_atlas
                 * (Image, [Rectangle]) — same machinery applies to any future
                 * tuple stub with an array element. */
                for (int ti = 0; ti < pt->tuple.elem_count; ti++) {
                    Iron_Type *te = pt->tuple.elem_types[ti];
                    if (te && te->kind == IRON_TYPE_ARRAY) {
                        PLAN_63_04_EMIT_LIST_FOR(te->array.elem,
                            "via foreign-method-stub tuple-param scan");
                    }
                }
            }
        }
        if (fn->return_type) {
            if (fn->return_type->kind == IRON_TYPE_ARRAY) {
                PLAN_63_04_EMIT_LIST_FOR(fn->return_type->array.elem,
                                          "via foreign-method-stub return scan");
            } else if (fn->return_type->kind == IRON_TYPE_TUPLE) {
                /* Plan 67-04: tuple return may contain an array element
                 * type — same as tuple-param case above. */
                for (int ti = 0; ti < fn->return_type->tuple.elem_count; ti++) {
                    Iron_Type *te = fn->return_type->tuple.elem_types[ti];
                    if (te && te->kind == IRON_TYPE_ARRAY) {
                        PLAN_63_04_EMIT_LIST_FOR(te->array.elem,
                            "via foreign-method-stub tuple-return scan");
                    }
                }
            }
        }
    }

    #undef PLAN_63_04_EMIT_LIST_FOR

    shfree(emitted_mono_list_types);
}

/* Check if any type_decl's object extends the given name */
static bool ir_has_subtype(IronLIR_Module *module, const char *name) {
    for (int i = 0; i < module->type_decl_count; i++) {
        IronLIR_TypeDecl *td = module->type_decls[i];
        if (td->kind != IRON_LIR_TYPE_OBJECT) continue;
        if (!td->type || td->type->kind != IRON_TYPE_OBJECT) continue;
        if (!td->type->object.decl) continue;
        if (td->type->object.decl->extends_name &&
            strcmp(td->type->object.decl->extends_name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Object struct body emission ───────────────────────────────────────────── */

static void emit_object_struct_body(EmitCtx *ctx, IronLIR_TypeDecl *td,
                                     int type_tag) {
    /* Phase 33: runtime-provided surface types (Arena/ArenaSave/Box) have their
     * struct layout owned by the runtime headers; skip the body so we don't
     * redefine the runtime's Iron_Arena / Iron_ArenaSave / Iron_Box struct. */
    if (ir_is_runtime_provided_type(td->name)) {
        return;
    }
    const char *mangled = emit_object_type_name(td->name, ctx);
    iron_strbuf_appendf(&ctx->struct_bodies, "struct %s {\n", mangled);

    Iron_ObjectDecl *od = NULL;
    if (td->type && td->type->kind == IRON_TYPE_OBJECT && td->type->object.decl) {
        od = td->type->object.decl;
    }

    if (od) {
        if (od->extends_name) {
            const char *parent_mangled = emit_mangle_name(od->extends_name, ctx->arena);
            iron_strbuf_appendf(&ctx->struct_bodies,
                                 "    %s _base;\n", parent_mangled);
        } else if (ir_has_subtype(ctx->module, td->name)) {
            iron_strbuf_appendf(&ctx->struct_bodies,
                                 "    int32_t iron_type_tag;\n");
        }

        for (int i = 0; i < od->field_count; i++) {
            /* PROT-03 row 29 (AUDIT-01 M-severity): loop-bound + non-NULL
             * assert before the Iron_Field cast in the struct-body emitter. */
            assert(i >= 0 && i < od->field_count);
            assert(od->fields[i] != NULL);
            Iron_Field *f = (Iron_Field *)od->fields[i];
            const char *c_type = "int64_t";
            if (f->type_ann) {
                /* PROT-03 row 29b (AUDIT-01 M-severity): assert kind on
                 * f->type_ann before the Iron_TypeAnnotation cast. */
                IRON_NODE_ASSERT_KIND(f->type_ann, IRON_NODE_TYPE_ANNOTATION);
                Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                if (ta->is_func) {
                    /* func() field: emit as Iron_Closure fat pointer */
                    c_type = "Iron_Closure";
                } else if (ta->is_nullable) {
                    /* Build Optional type name from annotation */
                    const char *inner_c = emit_annotation_to_c(ta->name, ctx);
                    Iron_StrBuf opt_sb = iron_strbuf_create(64);
                    iron_strbuf_appendf(&opt_sb, "Iron_Optional_%s", inner_c);
                    c_type = iron_arena_strdup(ctx->arena,
                                               iron_strbuf_get(&opt_sb),
                                               opt_sb.len);
                    if (!c_type) iron_oom_abort("emit_structs.c:emit_object_struct_body optional_field");
                    iron_strbuf_free(&opt_sb);
                    /* Emit the optional struct if not already done */
                    iron_strbuf_appendf(&ctx->struct_bodies,
                                         "    %s %s;\n", c_type, f->name);
                    continue;
                } else if (ta->is_array) {
                    /* Array field: emit Iron_List_<elem_c_type> */
                    const char *elem_c = emit_annotation_to_c(ta->name, ctx);
                    Iron_StrBuf list_sb = iron_strbuf_create(64);
                    iron_strbuf_appendf(&list_sb, "Iron_List_");
                    for (const char *p = elem_c; *p; p++) {
                        if (*p == ' ' || *p == '*') {
                            iron_strbuf_appendf(&list_sb, "_");
                        } else {
                            char ch[2] = { *p, '\0' };
                            iron_strbuf_appendf(&list_sb, "%s", ch);
                        }
                    }
                    c_type = iron_arena_strdup(ctx->arena,
                                               iron_strbuf_get(&list_sb), list_sb.len);
                    if (!c_type) iron_oom_abort("emit_structs.c:emit_object_struct_body array_field");
                    iron_strbuf_free(&list_sb);
                } else {
                    c_type = emit_annotation_to_c(ta->name, ctx);
                }
            }
            iron_strbuf_appendf(&ctx->struct_bodies,
                                 "    %s %s;\n", c_type, f->name);
        }
    }

    iron_strbuf_appendf(&ctx->struct_bodies, "};\n");
    iron_strbuf_appendf(&ctx->struct_bodies,
                         "#define IRON_TAG_%s %d\n", mangled, type_tag);
}

/* ── Phase 48-03: Estimate size of a concrete type (in bytes) for variant split ── */

int emit_estimate_type_size(Iron_ObjectDecl *od) {
    if (!od) return 8;
    int total = 0;
    for (int i = 0; i < od->field_count; i++) {
        /* PROT-03 row 30 (AUDIT-01 M-severity): loop-bound + non-NULL assert
         * before the Iron_Field cast in emit_estimate_type_size. */
        assert(i >= 0 && i < od->field_count);
        assert(od->fields[i] != NULL);
        Iron_Field *f = (Iron_Field *)od->fields[i];
        if (f->type_ann) {
            /* PROT-03 row 30 (cont.): assert kind on f->type_ann before
             * the Iron_TypeAnnotation cast. */
            IRON_NODE_ASSERT_KIND(f->type_ann, IRON_NODE_TYPE_ANNOTATION);
            Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
            if (ta->is_array)       total += 24;  /* pointer + count + cap */
            else if (ta->is_func)   total += 16;  /* Iron_Closure */
            else if (strcmp(ta->name, "String") == 0) total += 16;  /* Iron_String */
            else total += 8;  /* Int, Bool, Float, etc. */
        } else {
            total += 8;
        }
    }
    return total > 0 ? total : 8;
}

/* ── Type declaration orchestrator ─────────────────────────────────────────── */

void emit_type_decls(EmitCtx *ctx) {
    IronLIR_Module *module = ctx->module;

    /* Forward declarations for all object and interface types */
    for (int i = 0; i < module->type_decl_count; i++) {
        IronLIR_TypeDecl *td = module->type_decls[i];
        /* Phase 33: runtime-provided surface types (Arena/ArenaSave/Box) are
         * already typedef'd by the runtime headers the generated C includes;
         * emitting a forward typedef here causes a redefinition error. */
        if (td->kind == IRON_LIR_TYPE_OBJECT &&
            ir_is_runtime_provided_type(td->name)) {
            continue;
        }
        if (td->kind == IRON_LIR_TYPE_OBJECT ||
            td->kind == IRON_LIR_TYPE_INTERFACE) {
            const char *type_name = (td->kind == IRON_LIR_TYPE_OBJECT)
                ? emit_object_type_name(td->name, ctx)
                : emit_mangle_name(td->name, ctx->arena);
            iron_strbuf_appendf(&ctx->forward_decls,
                                 "typedef struct %s %s;\n", type_name, type_name);
        }
    }
    if (ctx->forward_decls.len > 0) {
        iron_strbuf_appendf(&ctx->forward_decls, "\n");
    }

    /* Topological sort for object struct bodies */
    int obj_count = 0;
    for (int i = 0; i < module->type_decl_count; i++) {
        if (module->type_decls[i]->kind == IRON_LIR_TYPE_OBJECT) obj_count++;
    }

    if (obj_count > 0) {
        int *colors = (int *)iron_arena_alloc(ctx->arena,
                                               sizeof(int) * (size_t)module->type_decl_count,
                                               _Alignof(int));
        if (!colors) iron_oom_abort("emit_structs.c:emit_type_decls topo_colors");
        memset(colors, 0, sizeof(int) * (size_t)module->type_decl_count);

        IrTopoState topo;
        topo.sorted    = NULL;
        topo.module    = module;
        topo.colors    = colors;
        topo.has_cycle = false;

        for (int i = 0; i < module->type_decl_count; i++) {
            if (module->type_decls[i]->kind == IRON_LIR_TYPE_OBJECT &&
                colors[i] == IR_TOPO_WHITE) {
                ir_topo_visit(&topo, i);
            }
        }

        for (int i = 0; i < (int)arrlen(topo.sorted); i++) {
            emit_object_struct_body(ctx, topo.sorted[i], ctx->next_type_tag++);
        }
        if (arrlen(topo.sorted) > 0) {
            iron_strbuf_appendf(&ctx->struct_bodies, "\n");
        }
        arrfree(topo.sorted);
    }

    /* Interface tagged union structs (static dispatch) */
    if (ctx->iface_reg) {
        /* Phase 52-03: Arena-tracked allocation helpers (delegated to emit_split) */
        emit_split_arena_helpers(ctx);
        for (int i = 0; i < shlen(ctx->iface_reg->map); i++) {
            Iron_IfaceEntry *entry = &ctx->iface_reg->map[i].value;
            if (entry->alive_count == 0) continue;

            const char *iface_mangled = emit_mangle_name(entry->iface_name, ctx->arena);
            Iron_StrBuf *sb = &ctx->struct_bodies;

            /* Forward declaration */
            iron_strbuf_appendf(&ctx->forward_decls,
                                 "typedef struct %s %s;\n", iface_mangled, iface_mangled);

            /* Tag enum — canonical alphabetical order */
            iron_strbuf_appendf(sb, "typedef enum {\n");
            for (int j = 0; j < entry->impl_count; j++) {
                Iron_IfaceImpl *impl = &entry->impls[j];
                if (!impl->is_alive) continue;
                iron_strbuf_appendf(sb, "    %s_TAG_%s = %d,\n",
                                     iface_mangled, impl->type_name, impl->tag);
            }
            iron_strbuf_appendf(sb, "} %s_Tag;\n\n", iface_mangled);

            /* Phase 48-03: Variant size analysis for large variant indirection.
             * If the largest variant is >2x the smallest AND >64 bytes, store
             * it via pointer indirection to avoid union padding waste. */
            int smallest_size = 999999, largest_size = 0;
            for (int j = 0; j < entry->impl_count; j++) {
                Iron_IfaceImpl *impl = &entry->impls[j];
                if (!impl->is_alive || !impl->decl) continue;
                int sz = emit_estimate_type_size(impl->decl);
                if (sz < smallest_size) smallest_size = sz;
                if (sz > largest_size)  largest_size = sz;
            }
            bool has_indirect = (largest_size > 2 * smallest_size && largest_size > 64);

            /* Union of concrete types */
            iron_strbuf_appendf(sb, "typedef union {\n");
            iron_strbuf_appendf(sb, "    char _dummy;\n");
            for (int j = 0; j < entry->impl_count; j++) {
                Iron_IfaceImpl *impl = &entry->impls[j];
                if (!impl->is_alive) continue;
                const char *impl_mangled = emit_mangle_name(impl->type_name, ctx->arena);
                bool is_indirect = false;
                if (has_indirect && impl->decl) {
                    int sz = emit_estimate_type_size(impl->decl);
                    if (sz > 2 * smallest_size && sz > 64) {
                        is_indirect = true;
                        /* Track this variant as indirect (arena-alloc key for stb_ds) */
                        char ikey_buf[512];
                        snprintf(ikey_buf, sizeof(ikey_buf), "%s:%s", iface_mangled, impl->type_name);
                        const char *ikey_str = iron_arena_strdup(ctx->arena, ikey_buf, strlen(ikey_buf));
                        if (!ikey_str) iron_oom_abort("emit_structs.c:emit_type_decls indirect_variant_key");
                        shput(ctx->indirect_variants, ikey_str, true);
                    }
                }
                if (is_indirect) {
                    iron_strbuf_appendf(sb, "    %s *%s;\n", impl_mangled, impl->type_name);
                } else {
                    iron_strbuf_appendf(sb, "    %s %s;\n", impl_mangled, impl->type_name);
                }
            }
            iron_strbuf_appendf(sb, "} %s_data_t;\n\n", iface_mangled);

            /* The tagged union struct */
            iron_strbuf_appendf(sb, "struct %s {\n", iface_mangled);
            iron_strbuf_appendf(sb, "    %s_Tag tag;\n", iface_mangled);
            iron_strbuf_appendf(sb, "    %s_data_t data;\n", iface_mangled);
            iron_strbuf_appendf(sb, "};\n\n");

            /* Wrapping constructors: ConcreteType -> Interface tagged union */
            for (int j = 0; j < entry->impl_count; j++) {
                Iron_IfaceImpl *impl = &entry->impls[j];
                if (!impl->is_alive) continue;
                const char *impl_mangled = emit_mangle_name(impl->type_name, ctx->arena);

                /* Check if this variant uses pointer indirection */
                char ikey[512];
                snprintf(ikey, sizeof(ikey), "%s:%s", iface_mangled, impl->type_name);
                bool is_indirect = (shgeti(ctx->indirect_variants, ikey) >= 0);

                if (is_indirect) {
                    /* Large variant: heap-allocate and store pointer */
                    iron_strbuf_appendf(sb,
                        "static inline %s %s_from_%s(%s val) {\n"
                        "    %s result;\n"
                        "    result.tag = %s_TAG_%s;\n"
                        "    result.data.%s = (%s *)malloc(sizeof(%s));\n"
                        "    *result.data.%s = val;\n"
                        "    return result;\n"
                        "}\n\n",
                        iface_mangled, iface_mangled, impl->type_name, impl_mangled,
                        iface_mangled,
                        iface_mangled, impl->type_name,
                        impl->type_name, impl_mangled, impl_mangled,
                        impl->type_name);
                } else {
                    /* Small variant: inline storage (original behavior) */
                    iron_strbuf_appendf(sb,
                        "static inline %s %s_from_%s(%s val) {\n"
                        "    %s result;\n"
                        "    result.tag = %s_TAG_%s;\n"
                        "    result.data.%s = val;\n"
                        "    return result;\n"
                        "}\n\n",
                        iface_mangled, iface_mangled, impl->type_name, impl_mangled,
                        iface_mangled,
                        iface_mangled, impl->type_name,
                        impl->type_name);
                }

            }

            /* Phase 52-03: Split collection emission (delegated to emit_split) */
            emit_split_collection_for_iface(ctx, iface_mangled, entry);

            /* Phase 57: Reduced-storage sibling constructors.
             *
             * These MUST be emitted after emit_split_collection_for_iface()
             * because that helper populates BOTH ctx->reduced_storage_types
             * AND ctx->soa_types, AND emits the Iron_<Type>_Stor typedef that
             * these siblings reference. All three must exist before this loop
             * runs.
             *
             * The sibling is needed whenever the split collection's per-type
             * sub-array stores the reduced Iron_<Type>_Stor variant rather than
             * the full Iron_<Type>. That happens in TWO independent cases:
             *
             *   (a) Phase 48 picked SoA layout for this (iface, impl) pair --
             *       per-field arrays plus a stor view; soa_types is set.
             *
             *   (b) Phase 48 dead-field elimination removed at least one field
             *       even on the AoS path -- emit_split.c emits
             *       <Type>_Stor *items instead of <Type> *items, and
             *       reduced_storage_types is set. SoA need not be active for
             *       this case to trigger.
             *
             * Both cases share the same problem: the fused per-type loop in
             * emit_fusion.c indexes the sub-array and wraps the element into
             * an Iron_<Iface>, but the existing _from_<Type> constructor takes
             * the full Iron_<Type>. So we emit a sibling _from_<Type>_Stor:
             *
             *   - copies every alive (non-dead-eliminated) field from stor
             *     into the matching slot of the full Iron_<Type> inside
             *     u.data.<type>
             *   - widens any Phase 50 VRC-narrowed alive field via an
             *     explicit (int64_t) cast on the copy
             *   - zero-initializes every dead field (safe per Phase 48
             *     dead-field elimination -- those fields are unread through
             *     the interface and never observed by user code)
             *
             * The sibling is skipped for pointer-indirect variants because
             * the indirect path stores full structs through malloc; no
             * Iron_<Type>_Stor exists for them.
             *
             * Note: the plan (57-01) framed the trigger as ctx->soa_types,
             * but the actual storage selection in emit_split.c lines 343-353
             * uses ctx->reduced_storage_types -- which is a SUPERSET of SoA
             * (it's also set on AoS+dead-fields). The bug therefore manifests
             * for AoS+dead-fields fused chains too (e.g. when a fused .map.sum
             * is the only thing accessing the collection so layout_select sees
             * no for_pre loop and falls through to AoS, but dead-field elim
             * still triggers reduced storage). Triggering on
             * reduced_storage_types fixes both the documented SoA case and
             * the previously-undocumented AoS+dead-fields case.
             */
            {
                /* Build iface_collection_vids once for all impls of this iface,
                 * mirroring emit_split.c:125-133. */
                IronLIR_ValueId *iface_collection_vids57 = NULL; /* stb_ds */
                if (ctx->split_collection_ids) {
                    for (ptrdiff_t si = 0;
                         si < hmlen(ctx->split_collection_ids); si++) {
                        if (strcmp(ctx->split_collection_ids[si].value,
                                   iface_mangled) == 0) {
                            arrput(iface_collection_vids57,
                                   ctx->split_collection_ids[si].key);
                        }
                    }
                }

                for (int j57 = 0; j57 < entry->impl_count; j57++) {
                    Iron_IfaceImpl *impl57 = &entry->impls[j57];
                    if (!impl57->is_alive) continue;
                    if (!impl57->decl) continue;

                    const char *impl_mangled57 = emit_mangle_name(
                        impl57->type_name, ctx->arena);

                    /* Skip pointer-indirect variants: no reduced storage exists. */
                    char ikey57[512];
                    snprintf(ikey57, sizeof(ikey57), "%s:%s",
                             iface_mangled, impl57->type_name);
                    bool is_indirect57 =
                        (shgeti(ctx->indirect_variants, ikey57) >= 0);
                    if (is_indirect57) continue;

                    /* Emit sibling for any (iface, impl) pair whose per-type
                     * sub-array stores Iron_<Type>_Stor. That includes both
                     * SoA-selected pairs (soa_types set) and AoS pairs whose
                     * dead-field elimination triggered reduced storage
                     * (reduced_storage_types set). reduced_storage_types is
                     * a SUPERSET of the soa_types case. */
                    bool is_reduced57 =
                        (ctx->reduced_storage_types &&
                         shgeti(ctx->reduced_storage_types,
                                impl57->type_name) >= 0);
                    if (!is_reduced57) continue;

                    Iron_ObjectDecl *od57 = impl57->decl;
                    iron_strbuf_appendf(sb,
                        "static inline %s %s_from_%s_Stor(%s_Stor val) {\n"
                        "    %s u;\n"
                        "    u.tag = %s_TAG_%s;\n",
                        iface_mangled, iface_mangled, impl57->type_name,
                        impl_mangled57,
                        iface_mangled,
                        iface_mangled, impl57->type_name);

                    for (int fi = 0; fi < od57->field_count; fi++) {
                        /* PROT-03 unenumerated bonus (AUDIT-01 M-severity sibling
                         * of rows 27/29/30): loop-bound + non-NULL assert before
                         * the Iron_Field cast in the Phase 57 split-collection
                         * from-Stor constructor emitter. */
                        assert(fi >= 0 && fi < od57->field_count);
                        assert(od57->fields[fi] != NULL);
                        Iron_Field *f57 = (Iron_Field *)od57->fields[fi];

                        /* Alive = any collection vid of this iface uses this field */
                        bool any_used57 = false;
                        if (arrlen(iface_collection_vids57) > 0) {
                            for (int ci = 0;
                                 ci < (int)arrlen(iface_collection_vids57);
                                 ci++) {
                                if (iron_layout_is_field_used(&ctx->layout,
                                        iface_collection_vids57[ci], f57->name)) {
                                    any_used57 = true;
                                    break;
                                }
                            }
                        } else {
                            /* No collections recorded: defensive -- treat as alive */
                            any_used57 = true;
                        }

                        if (any_used57) {
                            /* Phase 50: widen VRC-compressed fields on copy */
                            const char *narrowed57 = iron_vr_get_narrowed_type(
                                &ctx->value_range, impl57->type_name, f57->name);
                            if (narrowed57) {
                                iron_strbuf_appendf(sb,
                                    "    u.data.%s.%s = (int64_t)val.%s;\n",
                                    impl57->type_name, f57->name, f57->name);
                            } else {
                                iron_strbuf_appendf(sb,
                                    "    u.data.%s.%s = val.%s;\n",
                                    impl57->type_name, f57->name, f57->name);
                            }
                        } else {
                            /* Dead field: zero-init (safe per Phase 48 elimination) */
                            iron_strbuf_appendf(sb,
                                "    u.data.%s.%s = 0;\n",
                                impl57->type_name, f57->name);
                        }
                    }

                    iron_strbuf_appendf(sb,
                        "    return u;\n"
                        "}\n\n");
                }

                arrfree(iface_collection_vids57);
            }
        }
    }

    /* Enum definitions */
    for (int i = 0; i < module->type_decl_count; i++) {
        IronLIR_TypeDecl *td = module->type_decls[i];
        if (td->kind != IRON_LIR_TYPE_ENUM) continue;
        if (!td->type || td->type->kind != IRON_TYPE_ENUM) continue;

        Iron_EnumDecl *ed = td->type->enu.decl;
        if (!ed) continue;

        /* Use mangled_name for monomorphized generics (e.g. "Iron_Option_Int"),
         * fall back to the standard mangle for non-generic enums. */
        const char *mangled;
        if (td->type->enu.mangled_name) {
            mangled = td->type->enu.mangled_name;
        } else {
            mangled = emit_mangle_name(ed->name, ctx->arena);
        }

        /* Deduplicate: skip if already emitted (relevant for monomorphized enums
         * that may be registered multiple times from different use sites). */
        if (shgeti(ctx->mono_registry, mangled) >= 0) continue;
        const char *mangled_copy = iron_arena_strdup(ctx->arena, mangled, strlen(mangled));
        if (!mangled_copy) iron_oom_abort("emit_structs.c:emit_type_decls mono_registry_key");
        shput(ctx->mono_registry, mangled_copy, true);

        if (ed->has_payloads) {
            /* ADT enum: emit tagged-union struct layout into struct_bodies */

            /* Forward declaration for the outer struct */
            iron_strbuf_appendf(&ctx->forward_decls,
                                 "typedef struct %s %s;\n", mangled, mangled);

            /* Tag enum */
            iron_strbuf_appendf(&ctx->struct_bodies, "typedef enum {\n");
            for (int j = 0; j < ed->variant_count; j++) {
                Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                iron_strbuf_appendf(&ctx->struct_bodies,
                                     "    %s_TAG_%s = %d,\n", mangled, ev->name, j);
            }
            iron_strbuf_appendf(&ctx->struct_bodies, "} %s_Tag;\n\n", mangled);

            /* Per-variant payload structs (only for variants with payloads) */
            Iron_Type ***vpt = td->type->enu.variant_payload_types;
            for (int j = 0; j < ed->variant_count; j++) {
                Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                if (ev->payload_count <= 0) continue;
                iron_strbuf_appendf(&ctx->struct_bodies,
                                     "typedef struct { ");
                /* Phase 81: Void payload support.
                 * Skip any field whose resolved type is IRON_TYPE_VOID so
                 * generic ADT instantiations like Result[Void, E] lower to
                 * a zero-field payload. If the ENTIRE variant is all-void
                 * (e.g., Result.Ok(T) with T=Void), emit a single
                 * `char _dummy;` placeholder so the struct body remains a
                 * valid C type (zero-field structs are a GNU extension
                 * clang flags under -pedantic). */
                int emitted_fields = 0;
                for (int k = 0; k < ev->payload_count; k++) {
                    const char *pt = "void*";
                    Iron_Type *field_ty = NULL;
                    if (vpt && vpt[j] && vpt[j][k]) {
                        field_ty = vpt[j][k];
                        pt = emit_type_to_c(field_ty, ctx);
                    }
                    if (field_ty && field_ty->kind == IRON_TYPE_VOID) {
                        continue; /* skip Void payload field entirely */
                    }
                    bool is_boxed = false;
                    if (td->type->enu.payload_is_boxed &&
                        td->type->enu.payload_is_boxed[j] &&
                        td->type->enu.payload_is_boxed[j][k]) {
                        is_boxed = true;
                    }
                    if (emitted_fields > 0) iron_strbuf_appendf(&ctx->struct_bodies, " ");
                    if (is_boxed) {
                        iron_strbuf_appendf(&ctx->struct_bodies, "%s *_%d;", pt, k);
                    } else {
                        iron_strbuf_appendf(&ctx->struct_bodies, "%s _%d;", pt, k);
                    }
                    emitted_fields++;
                }
                if (emitted_fields == 0) {
                    iron_strbuf_appendf(&ctx->struct_bodies, "char _dummy;");
                }
                iron_strbuf_appendf(&ctx->struct_bodies,
                                     " } %s_%s_data;\n", mangled, ev->name);
            }
            iron_strbuf_appendf(&ctx->struct_bodies, "\n");

            /* Union of payloads */
            iron_strbuf_appendf(&ctx->struct_bodies, "typedef union {\n");
            iron_strbuf_appendf(&ctx->struct_bodies, "    char _dummy;\n");
            for (int j = 0; j < ed->variant_count; j++) {
                Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                if (ev->payload_count <= 0) continue;
                iron_strbuf_appendf(&ctx->struct_bodies,
                                     "    %s_%s_data %s;\n",
                                     mangled, ev->name, ev->name);
            }
            iron_strbuf_appendf(&ctx->struct_bodies,
                                 "} %s_data_t;\n\n", mangled);

            /* The ADT struct */
            iron_strbuf_appendf(&ctx->struct_bodies,
                                 "struct %s {\n", mangled);
            iron_strbuf_appendf(&ctx->struct_bodies,
                                 "    %s_Tag tag;\n", mangled);
            iron_strbuf_appendf(&ctx->struct_bodies,
                                 "    %s_data_t data;\n", mangled);
            iron_strbuf_appendf(&ctx->struct_bodies, "};\n\n");

            /* Phase 38: Emit a static _free helper if any variant has boxed fields */
            bool has_any_boxed = false;
            if (td->type->enu.payload_is_boxed) {
                for (int j2 = 0; j2 < ed->variant_count && !has_any_boxed; j2++) {
                    if (!td->type->enu.payload_is_boxed[j2]) continue;
                    Iron_EnumVariant *ev2 = (Iron_EnumVariant *)ed->variants[j2];
                    for (int k2 = 0; k2 < ev2->payload_count; k2++) {
                        if (td->type->enu.payload_is_boxed[j2][k2]) {
                            has_any_boxed = true;
                            break;
                        }
                    }
                }
            }
            if (has_any_boxed) {
                iron_strbuf_appendf(&ctx->struct_bodies,
                    "static void %s_free(%s *v) {\n", mangled, mangled);
                iron_strbuf_appendf(&ctx->struct_bodies,
                    "    if (!v) return;\n");
                iron_strbuf_appendf(&ctx->struct_bodies,
                    "    switch (v->tag) {\n");
                for (int j2 = 0; j2 < ed->variant_count; j2++) {
                    Iron_EnumVariant *ev2 = (Iron_EnumVariant *)ed->variants[j2];
                    iron_strbuf_appendf(&ctx->struct_bodies,
                        "    case %s_TAG_%s:", mangled, ev2->name);
                    bool variant_has_boxed = false;
                    if (td->type->enu.payload_is_boxed &&
                        td->type->enu.payload_is_boxed[j2]) {
                        for (int k2 = 0; k2 < ev2->payload_count; k2++) {
                            if (td->type->enu.payload_is_boxed[j2][k2]) {
                                variant_has_boxed = true;
                                break;
                            }
                        }
                    }
                    if (!variant_has_boxed) {
                        iron_strbuf_appendf(&ctx->struct_bodies, " break;\n");
                    } else {
                        iron_strbuf_appendf(&ctx->struct_bodies, "\n");
                        for (int k2 = 0; k2 < ev2->payload_count; k2++) {
                            if (td->type->enu.payload_is_boxed[j2] &&
                                td->type->enu.payload_is_boxed[j2][k2]) {
                                iron_strbuf_appendf(&ctx->struct_bodies,
                                    "        %s_free(v->data.%s._%d);\n",
                                    mangled, ev2->name, k2);
                                iron_strbuf_appendf(&ctx->struct_bodies,
                                    "        free(v->data.%s._%d);\n",
                                    ev2->name, k2);
                            }
                        }
                        iron_strbuf_appendf(&ctx->struct_bodies, "        break;\n");
                    }
                }
                iron_strbuf_appendf(&ctx->struct_bodies, "    }\n}\n\n");
            }
        } else {
            /* Plain enum: emit unchanged typedef enum */
            iron_strbuf_appendf(&ctx->enum_defs, "typedef enum {\n");
            for (int j = 0; j < ed->variant_count; j++) {
                Iron_EnumVariant *ev = (Iron_EnumVariant *)ed->variants[j];
                if (ev->has_explicit_value) {
                    iron_strbuf_appendf(&ctx->enum_defs, "    %s_%s = %d",
                                         mangled, ev->name, ev->explicit_value);
                } else {
                    iron_strbuf_appendf(&ctx->enum_defs, "    %s_%s",
                                         mangled, ev->name);
                }
                if (j < ed->variant_count - 1) {
                    iron_strbuf_appendf(&ctx->enum_defs, ",");
                }
                iron_strbuf_appendf(&ctx->enum_defs, "\n");
            }
            iron_strbuf_appendf(&ctx->enum_defs, "} %s;\n\n", mangled);
        }
    }

    /* ── Phase 56: Mono-collapsed list type decls ──────────────────────────
     * After all object structs, interface tagged unions, split collection
     * structs, and enums are emitted, declare Iron_List_Iron_<Type> plus
     * IRON_LIST_DECL/IRON_LIST_IMPL macro expansions for every concrete
     * type that Phase 49 mono collapse touched.  Fixes the "use of
     * undeclared identifier 'Iron_List_Iron_Circle'" codegen error. */
    emit_mono_list_decls(ctx);
}

/* ── Extern function prototype emission ──────────────────────────────────────
 *
 * Auto-generates C forward declarations for `extern func` bindings whose
 * parameter types are resolved (not void-placeholders). Covers raylib and
 * other C library bindings declared via `extern func` in Iron source.
 *
 * Called by both iron_lir_emit_c (native) and emit_web_module (web) so the
 * generated C declares InitWindow, ClearBackground, and friends regardless of
 * which target the user picked.
 */
/* Arena stub glue: src/stdlib/arena.iron declares Arena.new /
 * new_threadsafe / with_capacity / save / restore / reset / used / capacity
 * as EMPTY-BODY stubs, so their calls mangle to Iron_arena_* symbols that no
 * C file implements — the real API is the iron_arena_rt_* runtime substrate
 * (src/runtime/iron_arena_rt.h, included by every generated TU). Emitting a
 * bare prototype (the generic emit_foreign_method_prototypes path below)
 * would leave the symbols undefined at link time. Instead, synthesize a
 * per-program static bridge definition into ctx->lifted_funcs (which lands
 * after the prototypes and before the function bodies — mirroring the
 * emit_ensure_mutex glue shape). `static inline` so the always-prepended
 * arena.iron stubs don't trip -Wunused-function in arena-free programs.
 *
 * Signature is derived from the LIR stub itself (emit_type_to_c on the
 * stub's param/return types) so it tracks the Arena -> Iron_Arena_RT *
 * mapping in emit_helpers.c. Param 0 is always the implicit `self`
 * receiver (hir_lower stub-method convention) — garbage for the
 * associated-style constructors, which ignore it. Returns true when glue
 * was emitted (caller skips the bare prototype); false falls through to
 * the generic path for unknown names/arities. */
static bool ir_emit_arena_stub_glue(EmitCtx *ctx, IronLIR_Func *fn,
                                    const char *mangled) {
    if (strncmp(mangled, "Iron_arena_", 11) != 0) return false;
    const char *m    = mangled + 11;
    const char *arg1 = NULL;   /* canonical name of the non-self param */
    const char *body = NULL;
    if ((strcmp(m, "new") == 0 || strcmp(m, "with_capacity") == 0) &&
        fn->param_count == 2) {
        arg1 = "size";
        body = "    (void)self;\n"
               "    return iron_arena_rt_new((uint64_t)size, false, \"arena\");\n";
    } else if (strcmp(m, "new_threadsafe") == 0 && fn->param_count == 2) {
        arg1 = "size";
        body = "    (void)self;\n"
               "    return iron_arena_rt_new((uint64_t)size, true, \"arena\");\n";
    } else if (strcmp(m, "save") == 0 && fn->param_count == 1) {
        body = "    return iron_arena_rt_save(self);\n";
    } else if (strcmp(m, "restore") == 0 && fn->param_count == 2) {
        arg1 = "point";
        body = "    iron_arena_rt_restore(self, point);\n";
    } else if (strcmp(m, "reset") == 0 && fn->param_count == 1) {
        body = "    iron_arena_rt_reset(self);\n";
    } else if (strcmp(m, "used") == 0 && fn->param_count == 1) {
        body = "    return (int64_t)iron_arena_rt_used(self);\n";
    } else if (strcmp(m, "capacity") == 0 && fn->param_count == 1) {
        body = "    return (int64_t)iron_arena_rt_capacity(self);\n";
    }
    if (!body) return false;

    const char *ret_c  = fn->return_type
        ? emit_type_to_c(fn->return_type, ctx)
        : "void";
    const char *self_c = emit_type_to_c(fn->params[0].type, ctx);
    iron_strbuf_appendf(&ctx->lifted_funcs,
                        "static inline %s %s(%s self",
                        ret_c, mangled, self_c);
    if (arg1) {
        const char *a1_c = emit_type_to_c(fn->params[1].type, ctx);
        iron_strbuf_appendf(&ctx->lifted_funcs, ", %s %s", a1_c, arg1);
    }
    iron_strbuf_appendf(&ctx->lifted_funcs, ") {\n%s}\n\n", body);
    return true;
}

/* See header: auto-generates prototypes for foreign-method stubs whose C
 * symbols are not declared by an included stdlib header. Extracted from
 * emit_c.c so emit_web.c can share the same logic — otherwise the web
 * emitter would miss declarations for Iron_window_* / Iron_draw_* etc. and
 * emcc would fail with implicit-function-declaration errors. */
void emit_foreign_method_prototypes(EmitCtx *ctx) {
    IronLIR_Module *module = ctx->module;
    static const char *k_header_declared_prefixes[] = {
        "Iron_string_",
        "Iron_list_",
        "Iron_array_",
        "Iron_math_",
        "Iron_io_",
        "Iron_time_",
        /* Iron_timer_* is fully declared in iron_time.h. Auto-gen must
         * skip the whole prefix because update/reset take pointer
         * receivers there while the LIR stubs are value-typed — emitting
         * a conflicting prototype is a hard clang error. Iron_timer_done
         * is value-typed in both places so it's just redundant, not
         * conflicting, but skipping is cleaner.
         *
         * Note: src/runtime/iron_runtime.h also declares ~40 Iron_<lower>_*
         * symbols (Iron_pool_, Iron_channel_, Iron_mutex_, …). None of
         * those are reachable as foreign-method stubs today because no
         * stdlib .iron file declares Pool / Channel / Mutex objects. If
         * any future stdlib adds such stubs, add the matching prefix here
         * or accept duplicate-prototype emission (safe when types match,
         * a hard error when they don't). */
        "Iron_timer_",
        "Iron_log_",
        "Iron_hint_",
        /* Phase 78 FMT: Int/Int32/Float numeric → String runtime shims
         * declared in iron_runtime.h (Iron_int_to_string, Iron_int32_to_string,
         * Iron_float_to_string). The Iron-level stubs in stdlib/int.iron and
         * stdlib/float.iron have zero explicit params (self is implicit for
         * stub methods per hir_lower.c:1627), so emitting a `(void)` prototype
         * here would conflict with the header's real one-arg signature. */
        "Iron_int_",
        "Iron_int32_",
        "Iron_float_",
        NULL
    };
    struct { const char *key; int value; } *emitted_fms = NULL;
    for (int fi = 0; fi < module->func_count; fi++) {
        IronLIR_Func *fn = module->funcs[fi];
        if (!fn || !fn->is_extern || fn->extern_c_name) continue;
        const char *mangled = emit_mangle_func_name(fn->name, ctx->arena);
        if (!mangled) continue;
        if (strncmp(mangled, "Iron_", 5) != 0) continue;
        bool header_declared = false;
        for (int pi = 0; k_header_declared_prefixes[pi]; pi++) {
            size_t plen = strlen(k_header_declared_prefixes[pi]);
            if (strncmp(mangled, k_header_declared_prefixes[pi], plen) == 0) {
                header_declared = true;
                break;
            }
        }
        if (header_declared) continue;
        if (shgeti(emitted_fms, mangled) >= 0) continue;
        shput(emitted_fms, mangled, 1);

        /* Arena stubs get a real bridge DEFINITION (over iron_arena_rt_*)
         * instead of a bare prototype — see ir_emit_arena_stub_glue. */
        if (ir_emit_arena_stub_glue(ctx, fn, mangled)) continue;

        const char *ret_c = fn->return_type
            ? emit_type_to_c(fn->return_type, ctx)
            : "void";
        iron_strbuf_appendf(&ctx->prototypes, "%s %s(", ret_c, mangled);
        if (fn->param_count == 0) {
            iron_strbuf_appendf(&ctx->prototypes, "void");
        } else {
            for (int p = 0; p < fn->param_count; p++) {
                if (p > 0) iron_strbuf_appendf(&ctx->prototypes, ", ");
                const char *pt_c = emit_type_to_c(fn->params[p].type, ctx);
                const char *pname = fn->params[p].name
                    ? fn->params[p].name
                    : "";
                iron_strbuf_appendf(&ctx->prototypes, "%s %s", pt_c, pname);
            }
        }
        iron_strbuf_appendf(&ctx->prototypes, ");\n");
    }
    shfree(emitted_fms);
}

void emit_extern_prototypes(EmitCtx *ctx) {
    IronLIR_Module *module = ctx->module;
    for (int ei = 0; ei < module->extern_decl_count; ei++) {
        IronLIR_ExternDecl *ed = module->extern_decls[ei];
        bool has_real_types = false;
        for (int pi = 0; pi < ed->param_count; pi++) {
            if (ed->param_types[pi] && ed->param_types[pi]->kind != IRON_TYPE_VOID) {
                has_real_types = true;
                break;
            }
        }
        if (!has_real_types && ed->param_count > 0) continue;
        if (ed->c_name && ed->c_name[0] >= 'a' && ed->c_name[0] <= 'z') continue;

        const char *ret_c = "void";
        if (ed->return_type) {
            if (ed->return_type->kind == IRON_TYPE_BOOL) ret_c = "bool";
            else if (ed->return_type->kind == IRON_TYPE_INT) ret_c = "int";
            else if (ed->return_type->kind == IRON_TYPE_FLOAT32) ret_c = "float";
            else ret_c = emit_type_to_c(ed->return_type, ctx);
        }
        iron_strbuf_appendf(&ctx->prototypes, "%s %s(", ret_c, ed->c_name);
        if (ed->param_count == 0) {
            iron_strbuf_appendf(&ctx->prototypes, "void");
        }
        for (int pi = 0; pi < ed->param_count; pi++) {
            if (pi > 0) iron_strbuf_appendf(&ctx->prototypes, ", ");
            Iron_Type *pt = ed->param_types[pi];
            if (pt && pt->kind == IRON_TYPE_STRING) {
                iron_strbuf_appendf(&ctx->prototypes, "const char *_p%d", pi);
            } else if (pt && (pt->kind == IRON_TYPE_INT || pt->kind == IRON_TYPE_INT64)) {
                iron_strbuf_appendf(&ctx->prototypes, "int _p%d", pi);
            } else if (pt && pt->kind == IRON_TYPE_FLOAT32) {
                iron_strbuf_appendf(&ctx->prototypes, "float _p%d", pi);
            } else if (pt && pt->kind == IRON_TYPE_BOOL) {
                iron_strbuf_appendf(&ctx->prototypes, "bool _p%d", pi);
            } else {
                const char *pt_c = emit_type_to_c(pt, ctx);
                iron_strbuf_appendf(&ctx->prototypes, "%s _p%d", pt_c, pi);
            }
        }
        iron_strbuf_appendf(&ctx->prototypes, ");\n");
    }
}
