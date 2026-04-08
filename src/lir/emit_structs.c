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
        /* Phase 50: Emit arena-tracked allocation helpers for split collections.
         * These are static inline functions used by generated push/free functions
         * to track all sub-array allocations for bulk deallocation.
         * Emitted whenever interfaces exist since split list structs are always generated. */
        {
            iron_strbuf_appendf(&ctx->struct_bodies,
                "/* Phase 50: Arena-tracked allocation helpers for split collections */\n"
                "static inline void *_iron_sl_track(void ***tracked_arr, int *count, int *cap, void *ptr) {\n"
                "    if (!ptr) return NULL;\n"
                "    if (*count >= *cap) {\n"
                "        *cap = *cap ? *cap * 2 : 8;\n"
                "        *tracked_arr = (void **)realloc(*tracked_arr, (size_t)*cap * sizeof(void *));\n"
                "    }\n"
                "    (*tracked_arr)[(*count)++] = ptr;\n"
                "    return ptr;\n"
                "}\n"
                "static inline void *_iron_sl_realloc_tracked(void ***tracked_arr, int *count, int *cap, void *old, size_t sz) {\n"
                "    void *p = realloc(old, sz);\n"
                "    if (!p) return NULL;\n"
                "    if (old) {\n"
                "        for (int i = 0; i < *count; i++) {\n"
                "            if ((*tracked_arr)[i] == old) { (*tracked_arr)[i] = p; return p; }\n"
                "        }\n"
                "    }\n"
                "    return _iron_sl_track(tracked_arr, count, cap, p);\n"
                "}\n"
                "static inline void _iron_sl_free_all(void **tracked, int count) {\n"
                "    for (int i = 0; i < count; i++) free(tracked[i]);\n"
                "    free(tracked);\n"
                "}\n\n");
        }
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

            /* ── Split Collection struct (Phase 41+48: Collection Splitting + Dead Field Elim)
             * For each interface, generate a split collection struct with per-type
             * sub-arrays + order index for ordered iteration.
             *
             * Phase 48: If layout analysis shows some fields are never accessed through
             * the split collection, emit reduced storage typedefs (Iron_<Type>_Stor)
             * that exclude dead fields, reducing memory footprint.
             */
            {
                /* Build lowercase interface name for C identifiers */
                char iface_lower[256];
                {
                    size_t nl = strlen(entry->iface_name);
                    if (nl >= sizeof(iface_lower)) nl = sizeof(iface_lower) - 1;
                    for (size_t ci2 = 0; ci2 < nl; ci2++)
                        iface_lower[ci2] = (char)((entry->iface_name[ci2] >= 'A' &&
                                                    entry->iface_name[ci2] <= 'Z')
                            ? entry->iface_name[ci2] + 32
                            : entry->iface_name[ci2]);
                    iface_lower[nl] = '\0';
                }

                /* Phase 48: Collect all split collection ValueIds for this interface.
                 * Used to compute the union of used fields across all collections. */
                IronLIR_ValueId *iface_collection_vids = NULL; /* stb_ds array */
                if (ctx->split_collection_ids) {
                    for (ptrdiff_t si = 0; si < hmlen(ctx->split_collection_ids); si++) {
                        if (strcmp(ctx->split_collection_ids[si].value, iface_mangled) == 0) {
                            arrput(iface_collection_vids, ctx->split_collection_ids[si].key);
                        }
                    }
                }

                /* Phase 48: For each impl type, determine which fields are used
                 * across ALL collections of this interface (union semantics). */
                for (int j = 0; j < entry->impl_count; j++) {
                    Iron_IfaceImpl *impl2 = &entry->impls[j];
                    if (!impl2->is_alive || !impl2->decl) continue;
                    Iron_ObjectDecl *od = impl2->decl;

                    int total_fields = od->field_count;
                    int used_fields = 0;
                    for (int fi = 0; fi < od->field_count; fi++) {
                        Iron_Field *f = (Iron_Field *)od->fields[fi];
                        bool any_used = false;
                        for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                            if (iron_layout_is_field_used(&ctx->layout,
                                    iface_collection_vids[ci2], f->name)) {
                                any_used = true;
                                break;
                            }
                        }
                        if (any_used) used_fields++;
                    }

                    /* Emit reduced storage typedef if some fields are dead */
                    if (used_fields < total_fields && used_fields > 0 &&
                        arrlen(iface_collection_vids) > 0) {
                        const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
                        iron_strbuf_appendf(sb,
                            "/* Phase 48: Reduced storage for %s (%d/%d fields) */\n",
                            impl2->type_name, used_fields, total_fields);
                        iron_strbuf_appendf(sb, "typedef struct {\n");
                        for (int fi = 0; fi < od->field_count; fi++) {
                            Iron_Field *f = (Iron_Field *)od->fields[fi];
                            bool any_used = false;
                            for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                                if (iron_layout_is_field_used(&ctx->layout,
                                        iface_collection_vids[ci2], f->name)) {
                                    any_used = true;
                                    break;
                                }
                            }
                            if (!any_used) continue;
                            /* Emit field with C type */
                            const char *c_type = "int64_t";
                            if (f->type_ann) {
                                Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                                if (!ta->is_func && !ta->is_nullable && !ta->is_array) {
                                    c_type = emit_annotation_to_c(ta->name, ctx);
                                }
                            }
                            /* Phase 50: Value range compression -- use narrower type if proven safe */
                            const char *narrowed = iron_vr_get_narrowed_type(
                                &ctx->value_range, impl2->type_name, f->name);
                            if (narrowed) {
                                c_type = narrowed;
                                if (ctx->report_compression) {
                                    fprintf(stderr, "note: compressed %s.%s to %s\n",
                                        impl2->type_name, f->name, narrowed);
                                }
                            }
                            iron_strbuf_appendf(sb, "    %s %s;\n", c_type, f->name);
                        }
                        iron_strbuf_appendf(sb, "} %s_Stor;\n\n", im);
                        /* Record this type uses reduced storage */
                        shput(ctx->reduced_storage_types, impl2->type_name, true);
                    }
                }

                /* Phase 48-02: Check for common fields.
                 * Common field shared arrays only apply when ALL alive implementors
                 * use AoS layout.  When any type uses SoA, each type stores its own
                 * per-field arrays, so common field factoring doesn't help
                 * (the per-type counts would differ from the shared count). */
                bool any_soa = false;
                for (int j = 0; j < entry->impl_count; j++) {
                    Iron_IfaceImpl *impl_chk = &entry->impls[j];
                    if (!impl_chk->is_alive) continue;
                    IronLayoutKind lk_chk = iron_layout_get_kind(&ctx->layout,
                        iface_mangled, impl_chk->type_name);
                    if (lk_chk == IRON_LAYOUT_SOA) { any_soa = true; break; }
                }
                CommonField *common_fields = NULL;
                if (!any_soa) {
                    common_fields = iron_layout_get_common_fields(
                        &ctx->layout, entry->iface_name);
                }

                iron_strbuf_appendf(sb, "/* Split collection for %s */\n", iface_mangled);
                iron_strbuf_appendf(sb, "typedef struct {\n");

                /* Phase 50: Arena tracking fields for bulk deallocation */
                iron_strbuf_appendf(sb, "    void **_tracked;\n");
                iron_strbuf_appendf(sb, "    int _tracked_count;\n");
                iron_strbuf_appendf(sb, "    int _tracked_cap;\n");

                /* Phase 48-02: Common field shared arrays (before per-type arrays) */
                if (common_fields && arrlen(common_fields) > 0) {
                    iron_strbuf_appendf(sb, "    /* Common fields shared across all implementors */\n");
                    for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                        iron_strbuf_appendf(sb, "    %s *%s_%s;\n",
                            common_fields[cfi].c_type, iface_lower,
                            common_fields[cfi].name);
                    }
                    iron_strbuf_appendf(sb, "    int64_t %s_common_count;\n", iface_lower);
                    iron_strbuf_appendf(sb, "    int64_t %s_common_cap;\n", iface_lower);
                }

                /* Per-type sub-arrays */
                for (int j = 0; j < entry->impl_count; j++) {
                    Iron_IfaceImpl *impl2 = &entry->impls[j];
                    if (!impl2->is_alive) continue;
                    const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
                    char lower_name[256];
                    {
                        size_t nl2 = strlen(impl2->type_name);
                        if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                        for (size_t ci3 = 0; ci3 < nl2; ci3++)
                            lower_name[ci3] = (char)((impl2->type_name[ci3] >= 'A' &&
                                                       impl2->type_name[ci3] <= 'Z')
                                ? impl2->type_name[ci3] + 32
                                : impl2->type_name[ci3]);
                        lower_name[nl2] = '\0';
                    }

                    /* Phase 48-02: Check SoA layout for this type */
                    IronLayoutKind lk = iron_layout_get_kind(&ctx->layout,
                        iface_mangled, impl2->type_name);

                    /* Phase 48-03: Layout annotation override with warning */
                    for (int ci4 = 0; ci4 < (int)arrlen(iface_collection_vids); ci4++) {
                        ptrdiff_t ov_idx = hmgeti(ctx->layout_overrides,
                            iface_collection_vids[ci4]);
                        if (ov_idx >= 0) {
                            int override_hint = ctx->layout_overrides[ov_idx].value;
                            IronLayoutKind override_lk = (override_hint == 1)
                                ? IRON_LAYOUT_SOA : IRON_LAYOUT_AOS;
                            if (override_lk != lk) {
                                fprintf(stderr,
                                    "warning: 'layout: %s' annotation may reduce performance "
                                    "-- compiler analysis suggests %s for %s. "
                                    "Annotation honored.\n",
                                    override_hint == 1 ? "soa" : "aos",
                                    lk == IRON_LAYOUT_SOA ? "SoA" : "AoS",
                                    impl2->type_name);
                            }
                            lk = override_lk;
                            break;
                        }
                    }

                    if (lk == IRON_LAYOUT_SOA && impl2->decl) {
                        /* SoA: emit separate per-field arrays */
                        {
                            char soa_key_tmp[768];
                            snprintf(soa_key_tmp, sizeof(soa_key_tmp), "%s:%s",
                                iface_mangled, impl2->type_name);
                            /* Arena-allocate key so it survives block scope */
                            const char *soa_key_str = iron_arena_strdup(ctx->arena,
                                soa_key_tmp, strlen(soa_key_tmp));
                            shput(ctx->soa_types, soa_key_str, true);
                        }
                        iron_strbuf_appendf(sb, "    /* SoA layout for %s */\n",
                            impl2->type_name);
                        Iron_ObjectDecl *od = impl2->decl;
                        for (int fi = 0; fi < od->field_count; fi++) {
                            Iron_Field *f = (Iron_Field *)od->fields[fi];
                            /* Check if this field is used (dead field elimination) */
                            bool any_used = true;
                            if (arrlen(iface_collection_vids) > 0) {
                                any_used = false;
                                for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                                    if (iron_layout_is_field_used(&ctx->layout,
                                            iface_collection_vids[ci2], f->name)) {
                                        any_used = true;
                                        break;
                                    }
                                }
                            }
                            /* Skip common fields if they exist (stored in shared arrays) */
                            bool is_common = false;
                            if (common_fields) {
                                for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                                    if (strcmp(common_fields[cfi].name, f->name) == 0 &&
                                        common_fields[cfi].position == fi) {
                                        is_common = true;
                                        break;
                                    }
                                }
                            }
                            if (!any_used) continue;
                            if (is_common) continue;
                            /* Emit field-specific array */
                            const char *c_type = "int64_t";
                            if (f->type_ann) {
                                Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                                if (!ta->is_func && !ta->is_nullable && !ta->is_array) {
                                    c_type = emit_annotation_to_c(ta->name, ctx);
                                }
                            }
                            /* Phase 50: Value range compression for SoA field arrays */
                            const char *narrowed_soa = iron_vr_get_narrowed_type(
                                &ctx->value_range, impl2->type_name, f->name);
                            if (narrowed_soa) c_type = narrowed_soa;
                            iron_strbuf_appendf(sb, "    %s *%s_%s;\n",
                                c_type, lower_name, f->name);
                        }
                        iron_strbuf_appendf(sb, "    int64_t %s_count;\n", lower_name);
                        iron_strbuf_appendf(sb, "    int64_t %s_cap;\n", lower_name);
                    } else {
                        /* AoS: Use reduced storage type if available, otherwise full struct */
                        ptrdiff_t red_idx = shgeti(ctx->reduced_storage_types, impl2->type_name);
                        if (red_idx >= 0) {
                            iron_strbuf_appendf(sb, "    %s_Stor *%s_items;\n", im, lower_name);
                        } else {
                            iron_strbuf_appendf(sb, "    %s *%s_items;\n", im, lower_name);
                        }
                        iron_strbuf_appendf(sb, "    int64_t %s_count;\n", lower_name);
                        iron_strbuf_appendf(sb, "    int64_t %s_cap;\n", lower_name);
                    }
                }
                /* Phase 48-03: Check if ALL collections of this interface are unordered */
                bool all_unordered = (arrlen(iface_collection_vids) > 0);
                for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                    if (hmgeti(ctx->unordered_collections, iface_collection_vids[ci2]) < 0) {
                        all_unordered = false;
                        break;
                    }
                }

                /* Order index array (skipped for [T, unordered] collections) */
                if (!all_unordered) {
                    iron_strbuf_appendf(sb, "    struct { uint8_t tag; int64_t idx; } *_order;\n");
                    iron_strbuf_appendf(sb, "    int64_t _order_count;\n");
                    iron_strbuf_appendf(sb, "    int64_t _order_cap;\n");
                }
                iron_strbuf_appendf(sb, "    int64_t _total_count;\n");
                iron_strbuf_appendf(sb, "} Iron_SplitList_%s;\n\n", iface_mangled);

                /* Push functions per type */
                for (int j = 0; j < entry->impl_count; j++) {
                    Iron_IfaceImpl *impl2 = &entry->impls[j];
                    if (!impl2->is_alive) continue;
                    const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
                    char lower_name[256];
                    {
                        size_t nl2 = strlen(impl2->type_name);
                        if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                        for (size_t ci3 = 0; ci3 < nl2; ci3++)
                            lower_name[ci3] = (char)((impl2->type_name[ci3] >= 'A' &&
                                                       impl2->type_name[ci3] <= 'Z')
                                ? impl2->type_name[ci3] + 32
                                : impl2->type_name[ci3]);
                        lower_name[nl2] = '\0';
                    }

                    /* Phase 48-02: Check if this type uses SoA layout */
                    char soa_key[768];
                    snprintf(soa_key, sizeof(soa_key), "%s:%s",
                        iface_mangled, impl2->type_name);
                    bool type_is_soa = (shgeti(ctx->soa_types, soa_key) >= 0);

                    ptrdiff_t red_idx = shgeti(ctx->reduced_storage_types, impl2->type_name);

                    /* Push function always accepts FULL struct (caller pushes concrete object) */
                    iron_strbuf_appendf(sb,
                        "static inline void Iron_SplitList_%s_push_%s("
                        "Iron_SplitList_%s *_sl, %s _val) {\n",
                        iface_mangled, impl2->type_name,
                        iface_mangled, im);

                    if (type_is_soa && impl2->decl) {
                        /* Phase 48-02: SoA push -- grow and copy each field array */
                        Iron_ObjectDecl *od = impl2->decl;

                        /* Capacity growth (shared across all field arrays) */
                        iron_strbuf_appendf(sb,
                            "    if (_sl->%s_count >= _sl->%s_cap) {\n"
                            "        _sl->%s_cap = _sl->%s_cap ? (int64_t)(_sl->%s_cap * 1.5) : 8;\n",
                            lower_name, lower_name,
                            lower_name, lower_name, lower_name);
                        /* Realloc each used non-common field array */
                        for (int fi = 0; fi < od->field_count; fi++) {
                            Iron_Field *f = (Iron_Field *)od->fields[fi];
                            bool any_used = true;
                            if (arrlen(iface_collection_vids) > 0) {
                                any_used = false;
                                for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                                    if (iron_layout_is_field_used(&ctx->layout,
                                            iface_collection_vids[ci2], f->name)) {
                                        any_used = true;
                                        break;
                                    }
                                }
                            }
                            bool is_common = false;
                            if (common_fields) {
                                for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                                    if (strcmp(common_fields[cfi].name, f->name) == 0 &&
                                        common_fields[cfi].position == fi) {
                                        is_common = true;
                                        break;
                                    }
                                }
                            }
                            if (!any_used || is_common) continue;
                            const char *c_type = "int64_t";
                            if (f->type_ann) {
                                Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)f->type_ann;
                                if (!ta->is_func && !ta->is_nullable && !ta->is_array) {
                                    c_type = emit_annotation_to_c(ta->name, ctx);
                                }
                            }
                            /* Phase 50: Use narrowed type for realloc sizeof */
                            const char *narrowed_r = iron_vr_get_narrowed_type(
                                &ctx->value_range, impl2->type_name, f->name);
                            if (narrowed_r) c_type = narrowed_r;
                            iron_strbuf_appendf(sb,
                                "        _sl->%s_%s = (%s *)_iron_sl_realloc_tracked("
                                "&_sl->_tracked, &_sl->_tracked_count, &_sl->_tracked_cap, "
                                "_sl->%s_%s, (size_t)_sl->%s_cap * sizeof(%s));\n",
                                lower_name, f->name, c_type,
                                lower_name, f->name,
                                lower_name, c_type);
                        }
                        iron_strbuf_appendf(sb, "    }\n");

                        /* Copy each field to its own array */
                        for (int fi = 0; fi < od->field_count; fi++) {
                            Iron_Field *f = (Iron_Field *)od->fields[fi];
                            bool any_used = true;
                            if (arrlen(iface_collection_vids) > 0) {
                                any_used = false;
                                for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                                    if (iron_layout_is_field_used(&ctx->layout,
                                            iface_collection_vids[ci2], f->name)) {
                                        any_used = true;
                                        break;
                                    }
                                }
                            }
                            bool is_common = false;
                            if (common_fields) {
                                for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                                    if (strcmp(common_fields[cfi].name, f->name) == 0 &&
                                        common_fields[cfi].position == fi) {
                                        is_common = true;
                                        break;
                                    }
                                }
                            }
                            if (!any_used) continue;
                            if (is_common) {
                                /* Common fields pushed to shared array instead */
                                iron_strbuf_appendf(sb,
                                    "    /* common field %s -> shared array */\n",
                                    f->name);
                                continue;
                            }
                            /* Phase 50: Narrowing cast for SoA field copy */
                            const char *narrowed_sf = iron_vr_get_narrowed_type(
                                &ctx->value_range, impl2->type_name, f->name);
                            if (narrowed_sf) {
                                iron_strbuf_appendf(sb,
                                    "    _sl->%s_%s[_sl->%s_count] = (%s)_val.%s;\n",
                                    lower_name, f->name, lower_name, narrowed_sf, f->name);
                            } else {
                                iron_strbuf_appendf(sb,
                                    "    _sl->%s_%s[_sl->%s_count] = _val.%s;\n",
                                    lower_name, f->name, lower_name, f->name);
                            }
                        }

                        /* Push common fields to shared arrays */
                        if (common_fields && arrlen(common_fields) > 0) {
                            iron_strbuf_appendf(sb,
                                "    if (_sl->%s_common_count >= _sl->%s_common_cap) {\n"
                                "        _sl->%s_common_cap = _sl->%s_common_cap ? (int64_t)(_sl->%s_common_cap * 1.5) : 8;\n",
                                iface_lower, iface_lower,
                                iface_lower, iface_lower, iface_lower);
                            for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                                iron_strbuf_appendf(sb,
                                    "        _sl->%s_%s = (%s *)_iron_sl_realloc_tracked("
                                    "&_sl->_tracked, &_sl->_tracked_count, &_sl->_tracked_cap, "
                                    "_sl->%s_%s, (size_t)_sl->%s_common_cap * sizeof(%s));\n",
                                    iface_lower, common_fields[cfi].name, common_fields[cfi].c_type,
                                    iface_lower, common_fields[cfi].name,
                                    iface_lower, common_fields[cfi].c_type);
                            }
                            iron_strbuf_appendf(sb, "    }\n");
                            for (int cfi = 0; cfi < (int)arrlen(common_fields); cfi++) {
                                iron_strbuf_appendf(sb,
                                    "    _sl->%s_%s[_sl->%s_common_count] = _val.%s;\n",
                                    iface_lower, common_fields[cfi].name,
                                    iface_lower, common_fields[cfi].name);
                            }
                            iron_strbuf_appendf(sb,
                                "    _sl->%s_common_count++;\n", iface_lower);
                        }
                    } else {
                        /* AoS push (original + reduced storage support) */
                        /* Build storage type name */
                        char stor_type_buf[512];
                        const char *stor_type;
                        if (red_idx >= 0) {
                            snprintf(stor_type_buf, sizeof(stor_type_buf), "%s_Stor", im);
                            stor_type = stor_type_buf;
                        } else {
                            stor_type = im;
                        }
                        /* Grow type-specific sub-array */
                        iron_strbuf_appendf(sb,
                            "    if (_sl->%s_count >= _sl->%s_cap) {\n"
                            "        _sl->%s_cap = _sl->%s_cap ? (int64_t)(_sl->%s_cap * 1.5) : 8;\n"
                            "        _sl->%s_items = (%s *)_iron_sl_realloc_tracked("
                            "&_sl->_tracked, &_sl->_tracked_count, &_sl->_tracked_cap, "
                            "_sl->%s_items, (size_t)_sl->%s_cap * sizeof(%s));\n"
                            "    }\n",
                            lower_name, lower_name,
                            lower_name, lower_name, lower_name,
                            lower_name, stor_type,
                            lower_name, lower_name, stor_type);

                        /* Phase 48: For reduced storage, copy only used fields */
                        if (red_idx >= 0 && impl2->decl) {
                            Iron_ObjectDecl *od = impl2->decl;
                            for (int fi = 0; fi < od->field_count; fi++) {
                                Iron_Field *f = (Iron_Field *)od->fields[fi];
                                bool any_used = false;
                                for (int ci2 = 0; ci2 < (int)arrlen(iface_collection_vids); ci2++) {
                                    if (iron_layout_is_field_used(&ctx->layout,
                                            iface_collection_vids[ci2], f->name)) {
                                        any_used = true;
                                        break;
                                    }
                                }
                                if (!any_used) continue;
                                /* Phase 50: Narrowing cast for AoS reduced field copy */
                                const char *narrowed_af = iron_vr_get_narrowed_type(
                                    &ctx->value_range, impl2->type_name, f->name);
                                if (narrowed_af) {
                                    iron_strbuf_appendf(sb,
                                        "    _sl->%s_items[_sl->%s_count].%s = (%s)_val.%s;\n",
                                        lower_name, lower_name, f->name, narrowed_af, f->name);
                                } else {
                                    iron_strbuf_appendf(sb,
                                        "    _sl->%s_items[_sl->%s_count].%s = _val.%s;\n",
                                        lower_name, lower_name, f->name, f->name);
                                }
                            }
                        } else {
                            /* Store full element */
                            iron_strbuf_appendf(sb,
                                "    _sl->%s_items[_sl->%s_count] = _val;\n",
                                lower_name, lower_name);
                        }
                    }

                    /* Grow order index (skipped for unordered collections) */
                    if (!all_unordered) {
                        iron_strbuf_appendf(sb,
                            "    if (_sl->_order_count >= _sl->_order_cap) {\n"
                            "        _sl->_order_cap = _sl->_order_cap ? (int64_t)(_sl->_order_cap * 1.5) : 8;\n"
                            "        _sl->_order = _iron_sl_realloc_tracked("
                            "&_sl->_tracked, &_sl->_tracked_count, &_sl->_tracked_cap, "
                            "_sl->_order, (size_t)_sl->_order_cap * sizeof(*_sl->_order));\n"
                            "    }\n"
                            "    _sl->_order[_sl->_order_count].tag = %d;\n"
                            "    _sl->_order[_sl->_order_count].idx = _sl->%s_count;\n"
                            "    _sl->_order_count++;\n",
                            impl2->tag, lower_name);
                    }
                    /* Increment counts */
                    iron_strbuf_appendf(sb,
                        "    _sl->%s_count++;\n"
                        "    _sl->_total_count++;\n"
                        "}\n\n",
                        lower_name);
                }
                /* Free function -- Phase 50: single bulk free via tracked pointer registry */
                iron_strbuf_appendf(sb,
                    "static inline void Iron_SplitList_%s_free("
                    "Iron_SplitList_%s *_sl) {\n",
                    iface_mangled, iface_mangled);
                iron_strbuf_appendf(sb,
                    "    _iron_sl_free_all(_sl->_tracked, _sl->_tracked_count);\n");
                iron_strbuf_appendf(sb, "}\n\n");
                arrfree(iface_collection_vids);
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
}
