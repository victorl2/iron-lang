/* emit_split.c -- Split collection struct generation, push/free functions, and prescan.
 *
 * Extracted from emit_c.c and emit_structs.c (Phase 52, Plan 03).
 *
 * Contains:
 *   - emit_prescan_split_collections: module-level scan for interface ARRAY_LITs
 *   - emit_split_arena_helpers: one-shot arena tracking helper emission
 *   - emit_split_collection_for_iface: per-interface struct/push/free emission
 */

#include "lir/emit_split.h"
#include "parser/ast.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>

/* ── Module-level prescan for split collections & layout analysis ─────────── */

void emit_prescan_split_collections(EmitCtx *ctx) {
    /* P7 local auto-narrowing applies to every module, not only modules that
     * happen to have an interface registry.  Field compression shares the
     * same analysis context but is likewise independent of split detection. */
    ctx->value_range.arena = ctx->arena;
    iron_vr_analyze(&ctx->value_range, ctx->module, ctx->iface_reg);

    if (!ctx->iface_reg) return;

    /* Iterate ALL functions to find interface-typed ARRAY_LITs */
    for (int fi = 0; fi < ctx->module->func_count; fi++) {
        IronLIR_Func *fn = ctx->module->funcs[fi];
        if (!fn || fn->is_extern || fn->block_count == 0) continue;
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronLIR_Block *blk = fn->blocks[bi];
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronLIR_Instr *in2 = blk->instrs[ii];
                if (in2->kind == IRON_LIR_CALL && in2->call.arg_count >= 2 &&
                    in2->call.func_ptr != IRON_LIR_VALUE_INVALID &&
                    (ptrdiff_t)in2->call.func_ptr < arrlen(fn->value_table) &&
                    fn->value_table[in2->call.func_ptr] &&
                    fn->value_table[in2->call.func_ptr]->kind == IRON_LIR_FUNC_REF) {
                    const char *cn = fn->value_table[in2->call.func_ptr]->func_ref.func_name;
                    size_t cl = cn ? strlen(cn) : 0;
                    Iron_Type *at = emit_get_value_type(fn, in2->call.args[0]);
                    if (cl > 7 && strncmp(cn, "Iron_List_", 10) == 0 &&
                        strcmp(cn + cl - 7, "_remove") == 0 &&
                        at && at->kind == IRON_TYPE_ARRAY && at->array.elem &&
                        at->array.elem->kind == IRON_TYPE_INTERFACE &&
                        at->array.elem->interface.decl) {
                        shput(ctx->iface_elem_assigned,
                              emit_mangle_name(at->array.elem->interface.decl->name, ctx->arena),
                              true);
                    }
                }
                if (in2->kind == IRON_LIR_SET_INDEX) {
                    Iron_Type *at = emit_get_value_type(fn, in2->index.array);
                    if (at && at->kind == IRON_TYPE_ARRAY && !at->array.is_bounded &&
                        at->array.elem && at->array.elem->kind == IRON_TYPE_INTERFACE &&
                        at->array.elem->interface.decl) {
                        const char *im = emit_mangle_name(
                            at->array.elem->interface.decl->name, ctx->arena);
                        shput(ctx->iface_elem_assigned, im, true);
                    }
                }
                if (in2->kind == IRON_LIR_ARRAY_LIT &&
                    in2->array_lit.elem_type &&
                    in2->array_lit.elem_type->kind == IRON_TYPE_INTERFACE &&
                    in2->array_lit.elem_type->interface.decl) {
                    const char *im = emit_mangle_name(
                        in2->array_lit.elem_type->interface.decl->name, ctx->arena);
                    const char *im_copy = iron_arena_strdup(ctx->arena, im, strlen(im));
                    if (!im_copy) iron_oom_abort("emit_split.c:emit_prescan_split_collections iface_mangled");
                    hmput(ctx->split_collection_ids, in2->id, im_copy);
                    /* Phase 48-03: Check for layout annotation override */
                    if (in2->type && in2->type->kind == IRON_TYPE_ARRAY) {
                        if (in2->type->array.layout_hint != 0) {
                            hmput(ctx->layout_overrides, in2->id,
                                  in2->type->array.layout_hint);
                        }
                        if (in2->type->array.is_unordered) {
                            hmput(ctx->unordered_collections, in2->id, true);
                        }
                    }
                }
            }
        }
    }

    /* Run field access analysis on the identified split collections */
    if (ctx->split_collection_ids && hmlen(ctx->split_collection_ids) > 0) {
        /* Convert anonymous struct map to Iron_SplitCollectionId for layout analysis */
        Iron_SplitCollectionId *la_ids = NULL;
        for (ptrdiff_t i = 0; i < hmlen(ctx->split_collection_ids); i++) {
            Iron_SplitCollectionId entry;
            entry.key = ctx->split_collection_ids[i].key;
            entry.value = ctx->split_collection_ids[i].value;
            hmputs(la_ids, entry);
        }
        ctx->layout.arena = ctx->arena;
        iron_layout_analyze(&ctx->layout, ctx->module, la_ids, ctx->iface_reg);
        /* Phase 48-02: SoA/AoS layout selection and common field detection */
        iron_layout_select(&ctx->layout, ctx->module, la_ids, ctx->iface_reg);
        hmfree(la_ids);
    }

}

/* ── Arena-tracked allocation helpers ─────────────────────────────────────── */

void emit_split_arena_helpers(EmitCtx *ctx) {
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

/* ── Per-interface split collection emission ──────────────────────────────── */

/* An implementor with lifecycle glue (drop, copy, rc or list fields) is
 * stored as whole objects (AoS): its drop / copy glue works on an object,
 * which a per-field (SoA) layout does not hold. */
/* The layout of one implementor: lifecycle forces AoS, then a `layout:`
 * annotation on any collection of the interface, then the analysis. Used
 * both for the common-field decision and for the per-type storage, which
 * must agree (common fields only exist when no implementor is SoA). */
static bool iface_variant_is_boxed(EmitCtx *ctx, const char *iface_mangled,
                                   const char *type_name) {
    char key[512];
    snprintf(key, sizeof(key), "%s:%s", iface_mangled, type_name);
    return ctx->indirect_variants && shgeti(ctx->indirect_variants, key) >= 0;
}

/* A boxed (indirect) implementor: element reads borrow a pointer to the
 * element in place, which needs the whole object stored (AoS, not
 * reduced). */
static bool split_impl_is_boxed(EmitCtx *ctx, const char *iface_mangled,
                                Iron_IfaceImpl *impl) {
    char key[512];
    snprintf(key, sizeof(key), "%s:%s", iface_mangled, impl->type_name);
    return ctx->indirect_variants && shgeti(ctx->indirect_variants, key) >= 0;
}

static IronLayoutKind split_layout_kind(EmitCtx *ctx, const char *iface_mangled,
                                        Iron_IfaceImpl *impl,
                                        IronLIR_ValueId *collection_vids) {
    if (impl->decl && (od_needs_drop(ctx, impl->decl) ||
                       od_needs_copy_fixup(ctx, impl->decl)))
        return IRON_LAYOUT_AOS;
    if (ctx->iface_elem_assigned && shgeti(ctx->iface_elem_assigned, iface_mangled) >= 0)
        return IRON_LAYOUT_AOS;
    if (split_impl_is_boxed(ctx, iface_mangled, impl))
        return IRON_LAYOUT_AOS;
    for (int ci = 0; ci < (int)arrlen(collection_vids); ci++) {
        ptrdiff_t ov = hmgeti(ctx->layout_overrides, collection_vids[ci]);
        if (ov >= 0)
            return ctx->layout_overrides[ov].value == 1 ? IRON_LAYOUT_SOA
                                                        : IRON_LAYOUT_AOS;
    }
    return iron_layout_get_kind(&ctx->layout, iface_mangled, impl->type_name);
}

/* The element-editing helpers of an interface list (#180):
 *   set(i, v)    `xs[i] = v`: drops the element it replaces; the same
 *                implementor overwrites its slot, another one moves it;
 *   insert(i, v) appends v to its implementor's array, then gives it
 *                position i in the order index;
 *   remove(i)    returns the element (owned) and closes the gap;
 *   clear()      drops every element;
 *   reverse()    reverses the order index.
 * A value passed in is owned: a boxed payload moves out of its box, which
 * is freed. Taking an element out of its per-type array (_detach) refills
 * the slot with that array's last element, so set and remove need whole
 * objects stored (split_layout_kind). */
static void lower_impl_name(char *out, size_t cap, const char *name) {
    size_t n = strlen(name);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
    out[n] = '\0';
}

static void emit_split_edit_helpers(EmitCtx *ctx, Iron_StrBuf *sb, const char *iface_mangled,
                                    Iron_IfaceEntry *entry, bool all_unordered) {
    bool edits = ctx->iface_elem_assigned &&
                 shgeti(ctx->iface_elem_assigned, iface_mangled) >= 0;
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive || !impl2->decl || !od_needs_drop(ctx, impl2->decl)) continue;
        const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
        emit_ensure_drop(ctx, im, impl2->decl);
        iron_strbuf_appendf(sb, "static void %s_drop(%s *self);\n", im, im);
    }
    /* clear() works on every layout: each count goes back to zero. */
    iron_strbuf_appendf(sb,
        "static inline void Iron_SplitList_%s_clear(Iron_SplitList_%s *_sl) {\n",
        iface_mangled, iface_mangled);
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive) continue;
        char ln[256];
        lower_impl_name(ln, sizeof(ln), impl2->type_name);
        if (impl2->decl && od_needs_drop(ctx, impl2->decl))
            iron_strbuf_appendf(sb,
                "    for (int64_t _k = 0; _k < _sl->%s_count; _k++) %s_drop(&_sl->%s_items[_k]);\n",
                ln, emit_mangle_name(impl2->type_name, ctx->arena), ln);
        iron_strbuf_appendf(sb, "    _sl->%s_count = 0;\n", ln);
    }
    if (!all_unordered) iron_strbuf_appendf(sb, "    _sl->_order_count = 0;\n");
    iron_strbuf_appendf(sb, "    _sl->_total_count = 0;\n}\n\n");
    if (all_unordered) return;

    iron_strbuf_appendf(sb,
        "static inline void Iron_SplitList_%s_reverse(Iron_SplitList_%s *_sl) {\n"
        "    for (int64_t _a = 0, _b = _sl->_order_count - 1; _a < _b; _a++, _b--) {\n"
        "        unsigned char _t[sizeof(*_sl->_order)];\n"
        "        memcpy(_t, &_sl->_order[_a], sizeof(_t));\n"
        "        memcpy(&_sl->_order[_a], &_sl->_order[_b], sizeof(_t));\n"
        "        memcpy(&_sl->_order[_b], _t, sizeof(_t));\n"
        "    }\n"
        "}\n\n",
        iface_mangled, iface_mangled);

    /* Appends an owned interface value to its implementor's array (and the
     * order index). */
    iron_strbuf_appendf(sb,
        "static inline bool Iron_SplitList_%s__append(Iron_SplitList_%s *_sl, %s _val) {\n"
        "    switch (_val.tag) {\n",
        iface_mangled, iface_mangled, iface_mangled);
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive) continue;
        if (iface_variant_is_boxed(ctx, iface_mangled, impl2->type_name))
            iron_strbuf_appendf(sb,
                "    case %d: Iron_SplitList_%s_push_%s(_sl, *_val.data.%s); free(_val.data.%s); return true;\n",
                impl2->tag, iface_mangled, impl2->type_name, impl2->type_name, impl2->type_name);
        else
            iron_strbuf_appendf(sb,
                "    case %d: Iron_SplitList_%s_push_%s(_sl, _val.data.%s); return true;\n",
                impl2->tag, iface_mangled, impl2->type_name, impl2->type_name);
    }
    iron_strbuf_appendf(sb, "    default: return false;\n    }\n}\n\n");

    iron_strbuf_appendf(sb,
        "static inline void Iron_SplitList_%s_insert(Iron_SplitList_%s *_sl, int64_t _i, %s _val) {\n"
        "    if ((uint64_t)_i > (uint64_t)_sl->_order_count) iron_panic_index_oob(__FILE__, __LINE__, _i, _sl->_order_count);\n"
        "    if (!Iron_SplitList_%s__append(_sl, _val)) return;\n"
        "    int64_t _n = _sl->_order_count;\n"
        "    unsigned char _t[sizeof(*_sl->_order)];\n"
        "    memcpy(_t, &_sl->_order[_n - 1], sizeof(_t));\n"
        "    memmove(&_sl->_order[_i + 1], &_sl->_order[_i], (size_t)(_n - 1 - _i) * sizeof(*_sl->_order));\n"
        "    memcpy(&_sl->_order[_i], _t, sizeof(_t));\n"
        "}\n\n",
        iface_mangled, iface_mangled, iface_mangled, iface_mangled);

    if (!edits) return;

    /* Takes the element at per-type slot _oi of implementor _ot out of its
     * array: the array's last element fills the slot. */
    iron_strbuf_appendf(sb,
        "static inline void Iron_SplitList_%s__detach(Iron_SplitList_%s *_sl, uint8_t _ot, int64_t _oi) {\n"
        "    switch (_ot) {\n",
        iface_mangled, iface_mangled);
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive) continue;
        char ln[256];
        lower_impl_name(ln, sizeof(ln), impl2->type_name);
        iron_strbuf_appendf(sb,
            "    case %d: {\n"
            "        int64_t _last = _sl->%s_count - 1;\n"
            "        if (_oi != _last) {\n"
            "            _sl->%s_items[_oi] = _sl->%s_items[_last];\n"
            "            for (int64_t _k = 0; _k < _sl->_order_count; _k++)\n"
            "                if (_sl->_order[_k].tag == %d && _sl->_order[_k].idx == _last) { _sl->_order[_k].idx = _oi; break; }\n"
            "        }\n"
            "        _sl->%s_count--;\n"
            "        break;\n"
            "    }\n",
            impl2->tag, ln, ln, ln, impl2->tag, ln);
    }
    iron_strbuf_appendf(sb, "    default: break;\n    }\n}\n\n");

    /* set */
    iron_strbuf_appendf(sb,
        "static inline void Iron_SplitList_%s_set(Iron_SplitList_%s *_sl, int64_t _i, %s _val) {\n"
        "    if ((uint64_t)_i >= (uint64_t)_sl->_order_count) iron_panic_index_oob(__FILE__, __LINE__, _i, _sl->_order_count);\n"
        "    uint8_t _ot = _sl->_order[_i].tag;\n"
        "    int64_t _oi = _sl->_order[_i].idx;\n"
        "    switch (_ot) {\n",
        iface_mangled, iface_mangled, iface_mangled);
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive || !impl2->decl || !od_needs_drop(ctx, impl2->decl)) continue;
        char ln[256];
        lower_impl_name(ln, sizeof(ln), impl2->type_name);
        iron_strbuf_appendf(sb, "    case %d: %s_drop(&_sl->%s_items[_oi]); break;\n",
                            impl2->tag, emit_mangle_name(impl2->type_name, ctx->arena), ln);
    }
    iron_strbuf_appendf(sb,
        "    default: break;\n"
        "    }\n"
        "    if (_ot == _val.tag) {\n"
        "        switch (_val.tag) {\n");
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive) continue;
        char ln[256];
        lower_impl_name(ln, sizeof(ln), impl2->type_name);
        if (iface_variant_is_boxed(ctx, iface_mangled, impl2->type_name))
            iron_strbuf_appendf(sb,
                "        case %d: _sl->%s_items[_oi] = *_val.data.%s; free(_val.data.%s); break;\n",
                impl2->tag, ln, impl2->type_name, impl2->type_name);
        else
            iron_strbuf_appendf(sb,
                "        case %d: _sl->%s_items[_oi] = _val.data.%s; break;\n",
                impl2->tag, ln, impl2->type_name);
    }
    iron_strbuf_appendf(sb,
        "        default: break;\n"
        "        }\n"
        "        return;\n"
        "    }\n"
        "    Iron_SplitList_%s__detach(_sl, _ot, _oi);\n"
        "    if (!Iron_SplitList_%s__append(_sl, _val)) return;\n"
        "    /* the appended order entry takes position _i */\n"
        "    _sl->_order[_i] = _sl->_order[_sl->_order_count - 1];\n"
        "    _sl->_order_count--;\n"
        "    _sl->_total_count--;\n"
        "}\n\n",
        iface_mangled, iface_mangled);

    /* remove */
    iron_strbuf_appendf(sb,
        "static inline %s Iron_SplitList_%s_remove(Iron_SplitList_%s *_sl, int64_t _i) {\n"
        "    if ((uint64_t)_i >= (uint64_t)_sl->_order_count) iron_panic_index_oob(__FILE__, __LINE__, _i, _sl->_order_count);\n"
        "    uint8_t _ot = _sl->_order[_i].tag;\n"
        "    int64_t _oi = _sl->_order[_i].idx;\n"
        "    %s _out;\n"
        "    memset(&_out, 0, sizeof(_out));\n"
        "    switch (_ot) {\n",
        iface_mangled, iface_mangled, iface_mangled, iface_mangled);
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive) continue;
        char ln[256];
        lower_impl_name(ln, sizeof(ln), impl2->type_name);
        iron_strbuf_appendf(sb, "    case %d: _out = %s_from_%s(_sl->%s_items[_oi]); break;\n",
                            impl2->tag, iface_mangled, impl2->type_name, ln);
    }
    iron_strbuf_appendf(sb,
        "    default: break;\n"
        "    }\n"
        "    Iron_SplitList_%s__detach(_sl, _ot, _oi);\n"
        "    memmove(&_sl->_order[_i], &_sl->_order[_i + 1], (size_t)(_sl->_order_count - 1 - _i) * sizeof(*_sl->_order));\n"
        "    _sl->_order_count--;\n"
        "    _sl->_total_count--;\n"
        "    return _out;\n"
        "}\n\n",
        iface_mangled);
}

void emit_split_collection_for_iface(EmitCtx *ctx, const char *iface_mangled,
                                      Iron_IfaceEntry *entry) {
    Iron_StrBuf *sb = &ctx->struct_bodies;

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
        bool elem_assigned = ctx->iface_elem_assigned &&
            shgeti(ctx->iface_elem_assigned, iface_mangled) >= 0;
        if (used_fields < total_fields && used_fields > 0 &&
            arrlen(iface_collection_vids) > 0 && !elem_assigned &&
            !split_impl_is_boxed(ctx, iface_mangled, impl2)) {
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
        IronLayoutKind lk_chk = split_layout_kind(ctx, iface_mangled, impl_chk,
                                                  iface_collection_vids);
        if (lk_chk == IRON_LAYOUT_SOA) { any_soa = true; break; }
    }
    CommonField *common_fields = NULL;
    if (!any_soa) {
        common_fields = iron_layout_get_common_fields(
            &ctx->layout, entry->iface_name);
    }

    /* Array members and their capacity fields, recorded as the struct is
     * emitted, for the clone below. */
    typedef struct { const char *arr; const char *cap; } SlMember;
    SlMember *sl_members = NULL;
#define SL_MEMBER(...) do { \
        char _mb[512]; snprintf(_mb, sizeof(_mb), __VA_ARGS__); \
        char *_cp = strchr(_mb, '|'); if (_cp) *_cp = '\0'; \
        SlMember _m = { iron_arena_strdup(ctx->arena, _mb, strlen(_mb)), \
                        _cp ? iron_arena_strdup(ctx->arena, _cp + 1, strlen(_cp + 1)) : NULL }; \
        arrput(sl_members, _m); \
    } while (0)
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
            SL_MEMBER("%s_%s|%s_common_cap", iface_lower, common_fields[cfi].name, iface_lower);
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
        IronLayoutKind lk = iron_layout_get_kind(&ctx->layout, iface_mangled,
                                                 impl2->type_name);

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
        /* The decision itself (lifecycle, annotation, analysis) is the
         * same one the common-field check used. */
        lk = split_layout_kind(ctx, iface_mangled, impl2, iface_collection_vids);

        if (lk == IRON_LAYOUT_SOA && impl2->decl) {
            /* SoA: emit separate per-field arrays */
            {
                char soa_key_tmp[768];
                snprintf(soa_key_tmp, sizeof(soa_key_tmp), "%s:%s",
                    iface_mangled, impl2->type_name);
                /* Arena-allocate key so it survives block scope */
                const char *soa_key_str = iron_arena_strdup(ctx->arena,
                    soa_key_tmp, strlen(soa_key_tmp));
                if (!soa_key_str) iron_oom_abort("emit_split.c:emit_split_collection_for_iface soa_key");
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
                SL_MEMBER("%s_%s|%s_cap", lower_name, f->name, lower_name);
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
            SL_MEMBER("%s_items|%s_cap", lower_name, lower_name);
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
        SL_MEMBER("_order|_order_cap");
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
    emit_split_edit_helpers(ctx, sb, iface_mangled, entry, all_unordered);
    /* take(): the list moves out and the source is left empty (#174). */
    iron_strbuf_appendf(sb,
        "static inline Iron_SplitList_%s Iron_SplitList_%s_take(Iron_SplitList_%s *_sl) {\n"
        "    Iron_SplitList_%s out = *_sl;\n"
        "    memset(_sl, 0, sizeof(*_sl));\n"
        "    return out;\n"
        "}\n\n",
        iface_mangled, iface_mangled, iface_mangled, iface_mangled);
    /* copy(): every array is duplicated into the copy's own tracked
     * allocations, then elements with copy glue are fixed up (such
     * implementors are always stored as whole objects, see
     * split_layout_kind). */
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive || !impl2->decl || !od_needs_copy_fixup(ctx, impl2->decl)) continue;
        const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
        emit_ensure_copy_fixup(ctx, im, impl2->decl);
        iron_strbuf_appendf(sb, "static void %s_copied(%s *self);\n", im, im);
    }
    iron_strbuf_appendf(sb,
        "static inline Iron_SplitList_%s Iron_SplitList_%s_clone(const Iron_SplitList_%s *_src) {\n"
        "    Iron_SplitList_%s dst = *_src;\n"
        "    dst._tracked = NULL; dst._tracked_count = 0; dst._tracked_cap = 0;\n",
        iface_mangled, iface_mangled, iface_mangled, iface_mangled);
    for (int mi = 0; mi < (int)arrlen(sl_members); mi++) {
        /* Allocated at the source's capacity; only `count` elements are
         * initialised and copied (every cap field has a matching count). */
        char cnt[512];
        snprintf(cnt, sizeof(cnt), "%s", sl_members[mi].cap);
        size_t cl = strlen(cnt);
        if (cl >= 4 && strcmp(cnt + cl - 4, "_cap") == 0)
            snprintf(cnt + cl - 4, sizeof(cnt) - (cl - 4), "_count");
        iron_strbuf_appendf(sb,
            "    if (_src->%s) {\n"
            "        size_t _cap = (size_t)_src->%s * sizeof(*_src->%s);\n"
            "        size_t _n = (size_t)_src->%s * sizeof(*_src->%s);\n"
            "        dst.%s = _iron_sl_realloc_tracked(&dst._tracked, &dst._tracked_count, "
            "&dst._tracked_cap, NULL, _cap ? _cap : 1);\n"
            "        if (!dst.%s) iron_oom_abort(\"split list copy\");\n"
            "        if (_n) memcpy(dst.%s, _src->%s, _n);\n"
            "    }\n",
            sl_members[mi].arr, sl_members[mi].cap, sl_members[mi].arr,
            cnt, sl_members[mi].arr,
            sl_members[mi].arr, sl_members[mi].arr, sl_members[mi].arr, sl_members[mi].arr);
    }
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive || !impl2->decl || !od_needs_copy_fixup(ctx, impl2->decl)) continue;
        const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
        char lower_name[256];
        size_t nl2 = strlen(impl2->type_name);
        if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
        for (size_t ci3 = 0; ci3 < nl2; ci3++)
            lower_name[ci3] = (char)((impl2->type_name[ci3] >= 'A' &&
                                       impl2->type_name[ci3] <= 'Z')
                ? impl2->type_name[ci3] + 32 : impl2->type_name[ci3]);
        lower_name[nl2] = '\0';
        iron_strbuf_appendf(sb,
            "    for (int64_t _i = 0; _i < dst.%s_count; _i++) %s_copied(&dst.%s_items[_i]);\n",
            lower_name, im, lower_name);
    }
    iron_strbuf_appendf(sb, "    return dst;\n}\n\n");
    arrfree(sl_members);
#undef SL_MEMBER

    /* Drop glue of implementors the free function calls: synthesised into
     * lifted_funcs (rendered later), so declare it first. */
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive || !impl2->decl || !od_needs_drop(ctx, impl2->decl)) continue;
        const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
        emit_ensure_drop(ctx, im, impl2->decl);
        iron_strbuf_appendf(sb, "static void %s_drop(%s *self);\n", im, im);
    }
    /* Free function -- Phase 50: single bulk free via tracked pointer registry */
    iron_strbuf_appendf(sb,
        "static inline void Iron_SplitList_%s_free("
        "Iron_SplitList_%s *_sl) {\n",
        iface_mangled, iface_mangled);
    /* Each element is dropped before the arrays go (#180). */
    for (int j = 0; j < entry->impl_count; j++) {
        Iron_IfaceImpl *impl2 = &entry->impls[j];
        if (!impl2->is_alive || !impl2->decl || !od_needs_drop(ctx, impl2->decl)) continue;
        const char *im = emit_mangle_name(impl2->type_name, ctx->arena);
        char lower_name[256];
        size_t nl2 = strlen(impl2->type_name);
        if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
        for (size_t ci3 = 0; ci3 < nl2; ci3++)
            lower_name[ci3] = (char)((impl2->type_name[ci3] >= 'A' &&
                                       impl2->type_name[ci3] <= 'Z')
                ? impl2->type_name[ci3] + 32 : impl2->type_name[ci3]);
        lower_name[nl2] = '\0';
        iron_strbuf_appendf(sb,
            "    for (int64_t _i = 0; _i < _sl->%s_count; _i++) %s_drop(&_sl->%s_items[_i]);\n",
            lower_name, im, lower_name);
    }
    iron_strbuf_appendf(sb,
        "    _iron_sl_free_all(_sl->_tracked, _sl->_tracked_count);\n");
    iron_strbuf_appendf(sb, "}\n\n");
    arrfree(iface_collection_vids);
}
