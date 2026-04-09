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

#include <stdio.h>
#include <string.h>

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
            Iron_Field *f = (Iron_Field *)od->fields[i];
            if (!f->type_ann) continue;
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

                /* Emit Iron_List_<mangled> struct typedef.  The
                 * IRON_LIST_DECL and IRON_LIST_IMPL macros assume this
                 * struct is already declared with fields
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

                /* Emit IRON_LIST_IMPL(T, suffix) — non-static function
                 * bodies.  Safe at translation-unit level because each
                 * mangled name is unique per compilation unit (Iron
                 * compiles to a single .c file per program). */
                iron_strbuf_appendf(&ctx->struct_bodies,
                    "IRON_LIST_IMPL(%s, %s)\n\n",
                    mangled, mangled);
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
    const char *mangled = emit_mangle_name(td->name, ctx->arena);
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
            Iron_Field *f = (Iron_Field *)od->fields[i];
            const char *c_type = "int64_t";
            if (f->type_ann) {
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
        Iron_Field *f = (Iron_Field *)od->fields[i];
        if (f->type_ann) {
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
        if (td->kind == IRON_LIR_TYPE_OBJECT ||
            td->kind == IRON_LIR_TYPE_INTERFACE) {
            const char *mangled = emit_mangle_name(td->name, ctx->arena);
            iron_strbuf_appendf(&ctx->forward_decls,
                                 "typedef struct %s %s;\n", mangled, mangled);
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
        shput(ctx->mono_registry,
              iron_arena_strdup(ctx->arena, mangled, strlen(mangled)), true);

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
                for (int k = 0; k < ev->payload_count; k++) {
                    const char *pt = "void*";
                    if (vpt && vpt[j] && vpt[j][k]) {
                        pt = emit_type_to_c(vpt[j][k], ctx);
                    }
                    bool is_boxed = false;
                    if (td->type->enu.payload_is_boxed &&
                        td->type->enu.payload_is_boxed[j] &&
                        td->type->enu.payload_is_boxed[j][k]) {
                        is_boxed = true;
                    }
                    if (k > 0) iron_strbuf_appendf(&ctx->struct_bodies, " ");
                    if (is_boxed) {
                        iron_strbuf_appendf(&ctx->struct_bodies, "%s *_%d;", pt, k);
                    } else {
                        iron_strbuf_appendf(&ctx->struct_bodies, "%s _%d;", pt, k);
                    }
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
