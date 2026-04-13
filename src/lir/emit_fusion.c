/* emit_fusion.c -- Fused loop emission for chained collection operations.
 *
 * Extracted from emit_c.c (Phase 52, Plan 03).
 *
 * Contains:
 *   - emit_fused_chain: emits a fused loop for flat and split collections
 */

#include "lir/emit_fusion.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Phase 49: Emit fused loop ─────────────────────────────────────────────
 * Replaces chained collection method calls (map/filter/reduce/forEach/sum)
 * with a single fused loop per concrete type.  No intermediate allocations.
 * Called at the terminal node of a detected fusion chain.
 */
void emit_fused_chain(EmitCtx *ctx, Iron_StrBuf *sb, IronLIR_Func *fn,
                       FusionChain *chain, IronLIR_Instr *terminal_instr,
                       int indent) {
    int ind = indent;
    bool is_split = chain->is_split;

    /* Determine result characteristics from terminal node */
    FusionChainNode *terminal = &chain->nodes[chain->node_count - 1];
    bool terminal_is_scalar = (strcmp(terminal->method, "reduce") == 0 ||
                               strcmp(terminal->method, "sum") == 0);
    bool terminal_is_void = (strcmp(terminal->method, "forEach") == 0);

    /* Check if terminal result is hoisted */
    bool is_hoisted = (ctx->phi_hoisted &&
                       hmgeti(ctx->phi_hoisted, terminal_instr->id) >= 0);

    /* Resolve source element type.
     * For flat arrays: source is an Iron_List_T, element type = T.
     * For split collections: source is an Iron_SplitList_Iface, element = iface type. */
    Iron_Type *source_type = emit_get_value_type(fn, chain->source);
    const char *source_elem_c = "int64_t";  /* fallback */
    if (source_type && source_type->kind == IRON_TYPE_ARRAY && source_type->array.elem) {
        source_elem_c = emit_type_to_c(source_type->array.elem, ctx);
    }

    /* Resolve terminal result type */
    const char *result_c_type = terminal_instr->type
        ? emit_type_to_c(terminal_instr->type, ctx) : "int64_t";

    /* Determine each chain node's output element type.
     * - map: output elem type comes from the CALL's return type (.array.elem)
     * - filter: output elem = input elem (passes through)
     * - reduce/sum/forEach: scalar result, no intermediate elem */
    /* FIX-02 / AUDIT-06 §15: NULL-check calloc */
    const char **node_out_type = (const char **)calloc((size_t)chain->node_count, sizeof(const char *));
    if (!node_out_type) iron_oom_abort("emit_fusion.c:emit_fused_chain node_out_type");
    for (int ni = 0; ni < chain->node_count; ni++) {
        FusionChainNode *node = &chain->nodes[ni];
        IronLIR_Instr *node_instr = NULL;
        if (node->call_vid < (IronLIR_ValueId)arrlen(fn->value_table))
            node_instr = fn->value_table[node->call_vid];
        if (strcmp(node->method, "map") == 0 && node_instr && node_instr->type &&
            node_instr->type->kind == IRON_TYPE_ARRAY && node_instr->type->array.elem) {
            node_out_type[ni] = emit_type_to_c(node_instr->type->array.elem, ctx);
        } else if (strcmp(node->method, "filter") == 0) {
            /* filter passes through the input element type */
            node_out_type[ni] = (ni > 0) ? node_out_type[ni - 1] : source_elem_c;
        } else if (strcmp(node->method, "reduce") == 0 && node_instr && node_instr->type) {
            node_out_type[ni] = emit_type_to_c(node_instr->type, ctx);
        } else if (strcmp(node->method, "sum") == 0) {
            node_out_type[ni] = (ni > 0) ? node_out_type[ni - 1] : source_elem_c;
        } else if (strcmp(node->method, "forEach") == 0) {
            node_out_type[ni] = "void";
        } else {
            node_out_type[ni] = source_elem_c;
        }
    }

    /* Determine the "current element type" entering each node.
     * Node 0 receives source_elem_c. Node N receives the previous node's output. */
    /* FIX-02 / AUDIT-06 §15: NULL-check calloc */
    const char **node_in_type = (const char **)calloc((size_t)chain->node_count, sizeof(const char *));
    if (!node_in_type) iron_oom_abort("emit_fusion.c:emit_fused_chain node_in_type");
    for (int ni = 0; ni < chain->node_count; ni++) {
        node_in_type[ni] = (ni == 0) ? source_elem_c : node_out_type[ni - 1];
    }

    /* ── A. Flat array fused loop ─────────────────────────────────────────── */
    if (!is_split) {
        /* 1. Declare result variable */
        emit_indent(sb, ind);
        if (terminal_is_scalar) {
            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", result_c_type);
            emit_val(sb, terminal_instr->id);
            iron_strbuf_appendf(sb, " = ");
            if (strcmp(terminal->method, "reduce") == 0 && terminal->init_arg != IRON_LIR_VALUE_INVALID) {
                emit_expr_to_buf(sb, terminal->init_arg, fn, ctx, ctx->current_block_id, 0);
            } else {
                iron_strbuf_appendf(sb, "0");
            }
            iron_strbuf_appendf(sb, ";\n");
        } else if (terminal_is_void) {
            /* forEach: no result variable */
        } else {
            /* Collection terminal (map/filter) */
            const char *res_list_type = emit_type_to_c(terminal_instr->type, ctx);
            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", res_list_type);
            emit_val(sb, terminal_instr->id);
            iron_strbuf_appendf(sb, " = %s_create();\n", res_list_type);
        }

        /* 2. Open scoped block with lambda typedefs + memcpy casts */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "{ /* Phase 49: Emit fused loop (flat) */\n");

        for (int ni = 0; ni < chain->node_count; ni++) {
            FusionChainNode *node = &chain->nodes[ni];
            if (node->lambda_arg_count > 0 && node->lambda_args) {
                const char *in_t = node_in_type[ni];
                /* Determine lambda return type and argument list */
                if (strcmp(node->method, "map") == 0) {
                    emit_indent(sb, ind + 1);
                    iron_strbuf_appendf(sb, "typedef %s (*_FuseFn_%d)(void *, %s);\n",
                        node_out_type[ni], ni, in_t);
                } else if (strcmp(node->method, "filter") == 0) {
                    emit_indent(sb, ind + 1);
                    iron_strbuf_appendf(sb, "typedef bool (*_FuseFn_%d)(void *, %s);\n", ni, in_t);
                } else if (strcmp(node->method, "reduce") == 0) {
                    emit_indent(sb, ind + 1);
                    iron_strbuf_appendf(sb, "typedef %s (*_FuseFn_%d)(void *, %s, %s);\n",
                        node_out_type[ni], ni, node_out_type[ni], in_t);
                } else if (strcmp(node->method, "forEach") == 0) {
                    emit_indent(sb, ind + 1);
                    iron_strbuf_appendf(sb, "typedef void (*_FuseFn_%d)(void *, %s);\n", ni, in_t);
                }
                emit_indent(sb, ind + 1);
                iron_strbuf_appendf(sb, "_FuseFn_%d _fuse_fn_%d; memcpy(&_fuse_fn_%d, &",
                    ni, ni, ni);
                emit_expr_to_buf(sb, node->lambda_args[0], fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ".fn, sizeof(_fuse_fn_%d));\n", ni);
                emit_indent(sb, ind + 1);
                iron_strbuf_appendf(sb, "void *_fuse_env_%d = ", ni);
                emit_expr_to_buf(sb, node->lambda_args[0], fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ".env;\n");
            }
        }

        /* 3. Emit the loop */
        emit_indent(sb, ind + 1);
        iron_strbuf_appendf(sb, "for (int64_t _fi = 0; _fi < ");
        emit_expr_to_buf(sb, chain->source, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ".count; _fi++) {\n");

        /* Element extraction */
        emit_indent(sb, ind + 2);
        iron_strbuf_appendf(sb, "%s _fuse_elem = ", source_elem_c);
        emit_expr_to_buf(sb, chain->source, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ".items[_fi];\n");

        /* Apply each chain operation */
        const char *cur_var = "_fuse_elem";
        int filter_depth = 0;  /* track nested if blocks from filter */
        for (int ni = 0; ni < chain->node_count; ni++) {
            FusionChainNode *node = &chain->nodes[ni];
            int inner_ind = ind + 2 + filter_depth;

            if (strcmp(node->method, "map") == 0) {
                emit_indent(sb, inner_ind);
                iron_strbuf_appendf(sb, "%s _fuse_v%d = _fuse_fn_%d(_fuse_env_%d, %s);\n",
                    node_out_type[ni], ni, ni, ni, cur_var);
                /* Update cur_var for next operation */
                /* FIX-02 / AUDIT-06 §15: NULL-check calloc */
                char *new_var = (char *)calloc(32, 1);
                if (!new_var) iron_oom_abort("emit_fusion.c:emit_fused_chain flat_cur_var");
                snprintf(new_var, 32, "_fuse_v%d", ni);
                cur_var = new_var;
            } else if (strcmp(node->method, "filter") == 0) {
                emit_indent(sb, inner_ind);
                iron_strbuf_appendf(sb, "if (!_fuse_fn_%d(_fuse_env_%d, %s)) continue;\n",
                    ni, ni, cur_var);
                /* cur_var unchanged -- element passes through filter */
            } else if (strcmp(node->method, "reduce") == 0) {
                emit_indent(sb, inner_ind);
                emit_val(sb, terminal_instr->id);
                iron_strbuf_appendf(sb, " = _fuse_fn_%d(_fuse_env_%d, ",
                    ni, ni);
                emit_val(sb, terminal_instr->id);
                iron_strbuf_appendf(sb, ", %s);\n", cur_var);
            } else if (strcmp(node->method, "sum") == 0) {
                emit_indent(sb, inner_ind);
                emit_val(sb, terminal_instr->id);
                iron_strbuf_appendf(sb, " += %s;\n", cur_var);
            } else if (strcmp(node->method, "forEach") == 0) {
                emit_indent(sb, inner_ind);
                iron_strbuf_appendf(sb, "_fuse_fn_%d(_fuse_env_%d, %s);\n",
                    ni, ni, cur_var);
            }

            /* For collection terminal that's the last node: push to result */
            if (ni == chain->node_count - 1 && !terminal_is_scalar && !terminal_is_void) {
                int push_ind = ind + 2 + filter_depth;
                const char *res_list_type = emit_type_to_c(terminal_instr->type, ctx);
                emit_indent(sb, push_ind);
                iron_strbuf_appendf(sb, "%s_push(&", res_list_type);
                emit_val(sb, terminal_instr->id);
                iron_strbuf_appendf(sb, ", %s);\n", cur_var);
            }
        }

        /* Close loop */
        emit_indent(sb, ind + 1);
        iron_strbuf_appendf(sb, "}\n");

        /* 4. Close scoped block */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "}\n");
    }

    /* ── B. Split collection fused loop ───────────────────────────────────── */
    else {
        const char *sp_iface = chain->sp_iface;
        Iron_IfaceEntry *sp_entry = NULL;
        if (ctx->iface_reg) {
            for (int ri = 0; ri < (int)shlen(ctx->iface_reg->map); ri++) {
                const char *mc2 = emit_mangle_name(
                    ctx->iface_reg->map[ri].value.iface_name, ctx->arena);
                if (strcmp(mc2, sp_iface) == 0) {
                    sp_entry = &ctx->iface_reg->map[ri].value;
                    break;
                }
            }
        }

        if (!sp_entry) {
            /* Fallback: can't resolve interface -- emit as normal call */
            free(node_out_type);
            free(node_in_type);
            return;
        }

        /* 1. Declare result variable for scalar terminals (shared across type loops) */
        if (terminal_is_scalar) {
            emit_indent(sb, ind);
            if (!is_hoisted) iron_strbuf_appendf(sb, "%s ", result_c_type);
            emit_val(sb, terminal_instr->id);
            iron_strbuf_appendf(sb, " = ");
            if (strcmp(terminal->method, "reduce") == 0 && terminal->init_arg != IRON_LIR_VALUE_INVALID) {
                emit_expr_to_buf(sb, terminal->init_arg, fn, ctx, ctx->current_block_id, 0);
            } else {
                iron_strbuf_appendf(sb, "0");
            }
            iron_strbuf_appendf(sb, ";\n");
        } else if (terminal_is_void) {
            /* forEach: no result variable */
        } else {
            /* Collection terminal -- create split list result */
            emit_indent(sb, ind);
            if (!is_hoisted) iron_strbuf_appendf(sb, "Iron_SplitList_%s ", sp_iface);
            emit_val(sb, terminal_instr->id);
            iron_strbuf_appendf(sb, " = {0};\n");
            hmput(ctx->split_collection_ids, terminal_instr->id, sp_iface);
        }

        /* 2. Open scoped block */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "{ /* Phase 49: Emit fused loop (split) */\n");

        /* Emit lambda typedefs + memcpy casts (shared across type loops) */
        for (int ni = 0; ni < chain->node_count; ni++) {
            FusionChainNode *node = &chain->nodes[ni];
            if (node->lambda_arg_count > 0 && node->lambda_args) {
                const char *in_t = node_in_type[ni];
                if (strcmp(node->method, "map") == 0) {
                    emit_indent(sb, ind + 1);
                    iron_strbuf_appendf(sb, "typedef %s (*_FuseFn_%d)(void *, %s);\n",
                        node_out_type[ni], ni, in_t);
                } else if (strcmp(node->method, "filter") == 0) {
                    emit_indent(sb, ind + 1);
                    iron_strbuf_appendf(sb, "typedef bool (*_FuseFn_%d)(void *, %s);\n", ni, in_t);
                } else if (strcmp(node->method, "reduce") == 0) {
                    emit_indent(sb, ind + 1);
                    iron_strbuf_appendf(sb, "typedef %s (*_FuseFn_%d)(void *, %s, %s);\n",
                        node_out_type[ni], ni, node_out_type[ni], in_t);
                } else if (strcmp(node->method, "forEach") == 0) {
                    emit_indent(sb, ind + 1);
                    iron_strbuf_appendf(sb, "typedef void (*_FuseFn_%d)(void *, %s);\n", ni, in_t);
                }
                emit_indent(sb, ind + 1);
                iron_strbuf_appendf(sb, "_FuseFn_%d _fuse_fn_%d; memcpy(&_fuse_fn_%d, &",
                    ni, ni, ni);
                emit_expr_to_buf(sb, node->lambda_args[0], fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ".fn, sizeof(_fuse_fn_%d));\n", ni);
                emit_indent(sb, ind + 1);
                iron_strbuf_appendf(sb, "void *_fuse_env_%d = ", ni);
                emit_expr_to_buf(sb, node->lambda_args[0], fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ".env;\n");
            }
        }

        /* 3. Per-type fused loops */
        for (int ji = 0; ji < sp_entry->impl_count; ji++) {
            Iron_IfaceImpl *impl = &sp_entry->impls[ji];
            if (!impl->is_alive) continue;

            /* Compute lowercase type name */
            char lower_name[256];
            {
                size_t nl2 = strlen(impl->type_name);
                if (nl2 >= sizeof(lower_name)) nl2 = sizeof(lower_name) - 1;
                for (size_t ci3 = 0; ci3 < nl2; ci3++)
                    lower_name[ci3] = (char)((impl->type_name[ci3] >= 'A' &&
                                               impl->type_name[ci3] <= 'Z')
                        ? impl->type_name[ci3] + 32
                        : impl->type_name[ci3]);
                lower_name[nl2] = '\0';
            }

            /* Phase 57: when the per-type sub-array stores Iron_<Type>_Stor
             * (reduced variant), we must call the _from_<Type>_Stor sibling
             * emitted by emit_structs.c Phase 57. The storage shape is
             * controlled by ctx->reduced_storage_types -- set by
             * emit_split.c whenever dead-field elimination removed any
             * field, regardless of AoS vs SoA selection.
             *
             * SoA implementors (ctx->soa_types) are a subset of reduced
             * storage; AoS implementors with at least one dead field are
             * also reduced. The previous (void)is_soa; defer marker only
             * caught the SoA subcase, missing AoS+dead-fields fused chains
             * such as the soa_fusion_map_sum reproduction (which falls
             * through to AoS because no for_pre loop is visible to
             * iron_layout_select but reduced storage still triggers). */
            bool is_reduced = false;
            if (ctx->reduced_storage_types &&
                shgeti(ctx->reduced_storage_types, impl->type_name) >= 0) {
                is_reduced = true;
            }

            /* Emit per-type loop */
            emit_indent(sb, ind + 1);
            iron_strbuf_appendf(sb, "for (int64_t _fi = 0; _fi < ");
            emit_expr_to_buf(sb, chain->source, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, ".%s_count; _fi++) {\n", lower_name);

            /* Construct element: wrap sub-array item in interface tagged
             * union. Phase 57: when reduced storage is in use, call the
             * sibling _from_<Type>_Stor constructor (emitted by emit_structs.c
             * Phase 57 path) instead of the AoS _from_<Type>. */
            emit_indent(sb, ind + 2);
            const char *ctor_suffix = is_reduced ? "_Stor" : "";
            iron_strbuf_appendf(sb, "%s _fuse_elem = %s_from_%s%s(",
                sp_iface, sp_iface, impl->type_name, ctor_suffix);
            emit_expr_to_buf(sb, chain->source, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, ".%s_items[_fi]);\n", lower_name);

            /* Apply chain operations inline */
            const char *cur_var = "_fuse_elem";
            for (int ni = 0; ni < chain->node_count; ni++) {
                FusionChainNode *node = &chain->nodes[ni];
                int inner_ind = ind + 2;

                if (strcmp(node->method, "map") == 0) {
                    emit_indent(sb, inner_ind);
                    iron_strbuf_appendf(sb, "%s _fuse_v%d = _fuse_fn_%d(_fuse_env_%d, %s);\n",
                        node_out_type[ni], ni, ni, ni, cur_var);
                    /* FIX-02 / AUDIT-06 §15: NULL-check calloc */
                    char *new_var = (char *)calloc(32, 1);
                    if (!new_var) iron_oom_abort("emit_fusion.c:emit_fused_chain split_cur_var");
                    snprintf(new_var, 32, "_fuse_v%d", ni);
                    cur_var = new_var;
                } else if (strcmp(node->method, "filter") == 0) {
                    emit_indent(sb, inner_ind);
                    iron_strbuf_appendf(sb, "if (!_fuse_fn_%d(_fuse_env_%d, %s)) continue;\n",
                        ni, ni, cur_var);
                } else if (strcmp(node->method, "reduce") == 0) {
                    emit_indent(sb, inner_ind);
                    emit_val(sb, terminal_instr->id);
                    iron_strbuf_appendf(sb, " = _fuse_fn_%d(_fuse_env_%d, ",
                        ni, ni);
                    emit_val(sb, terminal_instr->id);
                    iron_strbuf_appendf(sb, ", %s);\n", cur_var);
                } else if (strcmp(node->method, "sum") == 0) {
                    emit_indent(sb, inner_ind);
                    emit_val(sb, terminal_instr->id);
                    iron_strbuf_appendf(sb, " += %s;\n", cur_var);
                } else if (strcmp(node->method, "forEach") == 0) {
                    emit_indent(sb, inner_ind);
                    iron_strbuf_appendf(sb, "_fuse_fn_%d(_fuse_env_%d, %s);\n",
                        ni, ni, cur_var);
                }

                /* Collection terminal push for split results */
                if (ni == chain->node_count - 1 && !terminal_is_scalar && !terminal_is_void) {
                    emit_indent(sb, inner_ind);
                    iron_strbuf_appendf(sb, "Iron_SplitList_%s_push_%s(&",
                        sp_iface, impl->type_name);
                    emit_val(sb, terminal_instr->id);
                    iron_strbuf_appendf(sb, ", ");
                    /* For filter terminal, push the unwrapped item back */
                    if (strcmp(node->method, "filter") == 0) {
                        emit_expr_to_buf(sb, chain->source, fn, ctx, ctx->current_block_id, 0);
                        iron_strbuf_appendf(sb, ".%s_items[_fi]", lower_name);
                    } else {
                        iron_strbuf_appendf(sb, "%s", cur_var);
                    }
                    iron_strbuf_appendf(sb, ");\n");
                }
            }

            /* Close per-type loop */
            emit_indent(sb, ind + 1);
            iron_strbuf_appendf(sb, "}\n");
        }

        /* 4. Close scoped block */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "}\n");
    }

    free(node_out_type);
    free(node_in_type);
}
