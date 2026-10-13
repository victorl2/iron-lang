/* emit_fusion.c -- Fused loop emission for chained collection operations.
 *
 * Extracted from emit_c.c (Phase 52, Plan 03).
 *
 * Contains:
 *   - emit_fused_chain: emits the fused loop of a detected chain
 */

#include "lir/emit_fusion.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Emit a fused loop ─────────────────────────────────────────────────────
 * Replaces chained list method calls (map/filter/reduce/forEach/sum) with a
 * single loop over the source list. No intermediate allocations. Called at
 * the last call of a detected chain.
 */
void emit_fused_chain(EmitCtx *ctx, Iron_StrBuf *sb, IronLIR_Func *fn,
                       FusionChain *chain, IronLIR_Instr *terminal_instr,
                       int indent) {
    int ind = indent;

    /* Determine result characteristics from terminal node */
    FusionChainNode *terminal = &chain->nodes[chain->node_count - 1];
    bool terminal_is_scalar = (strcmp(terminal->method, "reduce") == 0 ||
                               strcmp(terminal->method, "sum") == 0);
    bool terminal_is_void = (strcmp(terminal->method, "forEach") == 0);

    /* Check if terminal result is hoisted */
    bool is_hoisted = (ctx->phi_hoisted &&
                       hmgeti(ctx->phi_hoisted, terminal_instr->id) >= 0);

    /* Resolve source element type: the source is an Iron_List_T. */
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

    /* FIX-03 / AUDIT-04 §8: matched-free tracking for per-map cur_var calloc
     * buffers. Each `map` node in the fused chain calloc's a fresh 32-byte
     * scratch buffer to hold the identifier name for the map's output
     * variable (`_fuse_vN`). Pre-67-07 those buffers leaked every emission
     * — one per map per fused chain, uncapped. This tracker collects every
     * calloc'd cur_var buffer and frees them at function exit below.
     */
    char **flat_cur_var_bufs  = NULL;

    /* ── The fused loop. Interface lists never reach here (emit_c.c). ─────── */
    {
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
            iron_strbuf_appendf(sb, " = %s_create%s();\n", res_list_type,
                emit_list_has_columns(ctx, terminal_instr->type) &&
                terminal_instr->type->array.layout_hint == 1 ? "_soa" : "");
        }

        /* 2. Open scoped block with lambda typedefs + memcpy casts */
        emit_indent(sb, ind);
        /* The marker names the Iron line of the last stage and the stages
         * that became this loop; tests/integration/fusible_parity.py reads it. */
        iron_strbuf_appendf(sb, "{ /* fused loop, line %u:", (unsigned)terminal_instr->span.line);
        for (int ni = 0; ni < chain->node_count; ni++)
            iron_strbuf_appendf(sb, " %s", chain->nodes[ni].method);
        iron_strbuf_appendf(sb, " */\n");

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
                iron_strbuf_appendf(sb, "_FuseFn_%d _fuse_fn_%d; iron_mem_copy(&_fuse_fn_%d, &",
                    ni, ni, ni);
                emit_expr_to_buf(sb, node->lambda_args[0], fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ".fn, sizeof(_fuse_fn_%d));\n", ni);
                emit_indent(sb, ind + 1);
                iron_strbuf_appendf(sb, "void *_fuse_env_%d = ", ni);
                emit_expr_to_buf(sb, node->lambda_args[0], fn, ctx, ctx->current_block_id, 0);
                iron_strbuf_appendf(sb, ".env;\n");
            }
        }

        /* 3. Emit the loop. A bounded vector / fixed-size array source keeps
         * its elements inline (data[0 .. len)); a list's live in its buffer. */
        Iron_Type *src_t = emit_get_value_type(fn, chain->source);
        bool src_bvec = src_t && src_t->kind == IRON_TYPE_ARRAY && src_t->array.is_bounded;
        emit_indent(sb, ind + 1);
        iron_strbuf_appendf(sb, "for (int64_t _fi = 0; _fi < ");
        emit_expr_to_buf(sb, chain->source, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ".%s; _fi++) {\n", src_bvec ? "len" : "count");

        /* Element extraction */
        emit_indent(sb, ind + 2);
        iron_strbuf_appendf(sb, "%s _fuse_elem = ", source_elem_c);
        if (emit_list_has_columns(ctx, src_t)) {
            iron_strbuf_appendf(sb, "%s_read(", emit_type_to_c(src_t, ctx));
            emit_expr_to_buf(sb, chain->source, fn, ctx, ctx->current_block_id, 0);
            iron_strbuf_appendf(sb, ", _fi);\n");
        } else {
        emit_expr_to_buf(sb, chain->source, fn, ctx, ctx->current_block_id, 0);
        iron_strbuf_appendf(sb, ".%s[_fi];\n", src_bvec ? "data" : "items");
        }

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
                /* FIX-03 / AUDIT-04 §8: track calloc'd buffer for matched
                 * free at function exit (replaces pre-67-07 leak). */
                char *new_var = (char *)calloc(32, 1);
                if (!new_var) iron_oom_abort("emit_fusion.c:emit_fused_chain flat_cur_var");
                snprintf(new_var, 32, "_fuse_v%d", ni);
                arrput(flat_cur_var_bufs, new_var);
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

    free(node_out_type);
    free(node_in_type);

    /* Free the per-map cur_var name buffers. */
    for (int fi = 0; fi < (int)arrlen(flat_cur_var_bufs); fi++) {
        free(flat_cur_var_bufs[fi]);
    }
    arrfree(flat_cur_var_bufs);
}
