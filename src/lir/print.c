#include "ir/print.h"
#include "util/strbuf.h"
#include "util/arena.h"
#include "analyzer/types.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Resolve a block ID to its label string within a function.
 * Returns "<invalid>" if the block ID is not found. */
static const char *resolve_block_label(const IronIR_Func *fn, IronIR_BlockId id) {
    if (id == IRON_IR_BLOCK_INVALID) return "<invalid>";
    for (int i = 0; i < fn->block_count; i++) {
        if (fn->blocks[i]->id == id) {
            return fn->blocks[i]->label;
        }
    }
    return "<invalid>";
}

/* Print a type annotation using a temporary arena. */
static void append_type(Iron_StrBuf *sb, const Iron_Type *type, Iron_Arena *tmp) {
    if (!type) {
        iron_strbuf_appendf(sb, "Void");
        return;
    }
    const char *ts = iron_type_to_string(type, tmp);
    iron_strbuf_appendf(sb, "%s", ts);
}

/* ── Instruction printer ──────────────────────────────────────────────────── */

static void print_instr(Iron_StrBuf *sb, const IronIR_Instr *instr,
                         const IronIR_Func *fn, bool show_annotations,
                         Iron_Arena *tmp) {
    switch (instr->kind) {

    /* ── Constants ──────────────────────────────────────────────────────── */

    case IRON_IR_CONST_INT:
        iron_strbuf_appendf(sb, "  %%%u = const_int %lld : ",
                            instr->id, (long long)instr->const_int.value);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_CONST_FLOAT:
        iron_strbuf_appendf(sb, "  %%%u = const_float %g : ",
                            instr->id, instr->const_float.value);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_CONST_BOOL:
        iron_strbuf_appendf(sb, "  %%%u = const_bool %s : ",
                            instr->id, instr->const_bool.value ? "true" : "false");
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_CONST_STRING:
        iron_strbuf_appendf(sb, "  %%%u = const_string \"%s\" : ",
                            instr->id,
                            instr->const_str.value ? instr->const_str.value : "");
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_CONST_NULL:
        iron_strbuf_appendf(sb, "  %%%u = const_null : ", instr->id);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    /* ── Binary ops ─────────────────────────────────────────────────────── */

    case IRON_IR_ADD:
        iron_strbuf_appendf(sb, "  %%%u = add %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_SUB:
        iron_strbuf_appendf(sb, "  %%%u = sub %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_MUL:
        iron_strbuf_appendf(sb, "  %%%u = mul %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_DIV:
        iron_strbuf_appendf(sb, "  %%%u = div %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_MOD:
        iron_strbuf_appendf(sb, "  %%%u = mod %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_EQ:
        iron_strbuf_appendf(sb, "  %%%u = eq %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_NEQ:
        iron_strbuf_appendf(sb, "  %%%u = neq %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_LT:
        iron_strbuf_appendf(sb, "  %%%u = lt %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_LTE:
        iron_strbuf_appendf(sb, "  %%%u = lte %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_GT:
        iron_strbuf_appendf(sb, "  %%%u = gt %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_GTE:
        iron_strbuf_appendf(sb, "  %%%u = gte %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_AND:
        iron_strbuf_appendf(sb, "  %%%u = and %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_OR:
        iron_strbuf_appendf(sb, "  %%%u = or %%%u, %%%u : ",
                            instr->id, instr->binop.left, instr->binop.right);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    /* ── Unary ops ──────────────────────────────────────────────────────── */

    case IRON_IR_NEG:
        iron_strbuf_appendf(sb, "  %%%u = neg %%%u : ",
                            instr->id, instr->unop.operand);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_NOT:
        iron_strbuf_appendf(sb, "  %%%u = not %%%u : ",
                            instr->id, instr->unop.operand);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    /* ── Memory ─────────────────────────────────────────────────────────── */

    case IRON_IR_ALLOCA:
        iron_strbuf_appendf(sb, "  %%%u = alloca ", instr->id);
        append_type(sb, instr->alloca.alloc_type, tmp);
        if (show_annotations && instr->alloca.name_hint) {
            iron_strbuf_appendf(sb, " ; name_hint: \"%s\"", instr->alloca.name_hint);
        }
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_LOAD:
        iron_strbuf_appendf(sb, "  %%%u = load %%%u : ",
                            instr->id, instr->load.ptr);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_STORE:
        iron_strbuf_appendf(sb, "  store %%%u, %%%u\n",
                            instr->store.ptr, instr->store.value);
        break;

    /* ── Field / Index ──────────────────────────────────────────────────── */

    case IRON_IR_GET_FIELD:
        iron_strbuf_appendf(sb, "  %%%u = get_field %%%u.%s : ",
                            instr->id, instr->field.object, instr->field.field);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_SET_FIELD:
        iron_strbuf_appendf(sb, "  set_field %%%u.%s, %%%u\n",
                            instr->field.object, instr->field.field, instr->field.value);
        break;

    case IRON_IR_GET_INDEX:
        iron_strbuf_appendf(sb, "  %%%u = get_index %%%u[%%%u] : ",
                            instr->id, instr->index.array, instr->index.index);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_SET_INDEX:
        iron_strbuf_appendf(sb, "  set_index %%%u[%%%u], %%%u\n",
                            instr->index.array, instr->index.index, instr->index.value);
        break;

    /* ── Call ───────────────────────────────────────────────────────────── */

    case IRON_IR_CALL: {
        bool is_void = (instr->type == NULL);
        if (!is_void) {
            iron_strbuf_appendf(sb, "  %%%u = ", instr->id);
        } else {
            iron_strbuf_appendf(sb, "  ");
        }

        if (instr->call.func_decl) {
            iron_strbuf_appendf(sb, "call @%s(", instr->call.func_decl->name);
        } else {
            iron_strbuf_appendf(sb, "call %%%u(", instr->call.func_ptr);
        }

        for (int i = 0; i < instr->call.arg_count; i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            iron_strbuf_appendf(sb, "%%%u", instr->call.args[i]);
        }

        if (!is_void) {
            iron_strbuf_appendf(sb, ") : ");
            append_type(sb, instr->type, tmp);
            iron_strbuf_appendf(sb, "\n");
        } else {
            iron_strbuf_appendf(sb, ")\n");
        }
        break;
    }

    /* ── Control flow (terminators) ─────────────────────────────────────── */

    case IRON_IR_JUMP:
        iron_strbuf_appendf(sb, "  jump %s\n",
                            resolve_block_label(fn, instr->jump.target));
        break;

    case IRON_IR_BRANCH:
        iron_strbuf_appendf(sb, "  branch %%%u, %s, %s\n",
                            instr->branch.cond,
                            resolve_block_label(fn, instr->branch.then_block),
                            resolve_block_label(fn, instr->branch.else_block));
        break;

    case IRON_IR_SWITCH: {
        iron_strbuf_appendf(sb, "  switch %%%u, default: %s",
                            instr->sw.subject,
                            resolve_block_label(fn, instr->sw.default_block));
        if (instr->sw.case_count > 0) {
            iron_strbuf_appendf(sb, ", [");
            for (int i = 0; i < instr->sw.case_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ", ");
                iron_strbuf_appendf(sb, "%d: %s",
                                    instr->sw.case_values[i],
                                    resolve_block_label(fn, instr->sw.case_blocks[i]));
            }
            iron_strbuf_appendf(sb, "]");
        }
        iron_strbuf_appendf(sb, "\n");
        break;
    }

    case IRON_IR_RETURN:
        if (instr->ret.is_void) {
            iron_strbuf_appendf(sb, "  ret void\n");
        } else {
            iron_strbuf_appendf(sb, "  ret %%%u\n", instr->ret.value);
        }
        break;

    /* ── Cast ───────────────────────────────────────────────────────────── */

    case IRON_IR_CAST:
        iron_strbuf_appendf(sb, "  %%%u = cast %%%u : ",
                            instr->id, instr->cast.value);
        append_type(sb, instr->cast.target_type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    /* ── Heap / RC / Free ───────────────────────────────────────────────── */

    case IRON_IR_HEAP_ALLOC:
        iron_strbuf_appendf(sb, "  %%%u = heap_alloc %%%u : ",
                            instr->id, instr->heap_alloc.inner_val);
        append_type(sb, instr->type, tmp);
        if (show_annotations) {
            if (instr->heap_alloc.auto_free) iron_strbuf_appendf(sb, " ; auto_free");
            if (instr->heap_alloc.escapes)   iron_strbuf_appendf(sb, " ; escapes");
        }
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_RC_ALLOC:
        iron_strbuf_appendf(sb, "  %%%u = rc_alloc %%%u : ",
                            instr->id, instr->rc_alloc.inner_val);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    case IRON_IR_FREE:
        iron_strbuf_appendf(sb, "  free %%%u\n", instr->free_instr.value);
        break;

    /* ── Construct / Array / Slice ──────────────────────────────────────── */

    case IRON_IR_CONSTRUCT: {
        iron_strbuf_appendf(sb, "  %%%u = construct ", instr->id);
        append_type(sb, instr->construct.type, tmp);
        iron_strbuf_appendf(sb, "(");
        for (int i = 0; i < instr->construct.field_count; i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            iron_strbuf_appendf(sb, "%%%u", instr->construct.field_vals[i]);
        }
        iron_strbuf_appendf(sb, ") : ");
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;
    }

    case IRON_IR_ARRAY_LIT: {
        iron_strbuf_appendf(sb, "  %%%u = array_lit [", instr->id);
        for (int i = 0; i < instr->array_lit.element_count; i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            iron_strbuf_appendf(sb, "%%%u", instr->array_lit.elements[i]);
        }
        iron_strbuf_appendf(sb, "] : ");
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;
    }

    case IRON_IR_SLICE: {
        iron_strbuf_appendf(sb, "  %%%u = slice %%%u[", instr->id, instr->slice.array);
        if (instr->slice.start == IRON_IR_VALUE_INVALID) {
            iron_strbuf_appendf(sb, "begin");
        } else {
            iron_strbuf_appendf(sb, "%%%u", instr->slice.start);
        }
        iron_strbuf_appendf(sb, ":");
        if (instr->slice.end == IRON_IR_VALUE_INVALID) {
            iron_strbuf_appendf(sb, "end");
        } else {
            iron_strbuf_appendf(sb, "%%%u", instr->slice.end);
        }
        iron_strbuf_appendf(sb, "] : ");
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;
    }

    /* ── Null checks ────────────────────────────────────────────────────── */

    case IRON_IR_IS_NULL:
        iron_strbuf_appendf(sb, "  %%%u = is_null %%%u : Bool\n",
                            instr->id, instr->null_check.value);
        break;

    case IRON_IR_IS_NOT_NULL:
        iron_strbuf_appendf(sb, "  %%%u = is_not_null %%%u : Bool\n",
                            instr->id, instr->null_check.value);
        break;

    /* ── String interpolation ───────────────────────────────────────────── */

    case IRON_IR_INTERP_STRING: {
        iron_strbuf_appendf(sb, "  %%%u = interp_string [", instr->id);
        for (int i = 0; i < instr->interp_string.part_count; i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            iron_strbuf_appendf(sb, "%%%u", instr->interp_string.parts[i]);
        }
        iron_strbuf_appendf(sb, "] : ");
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;
    }

    /* ── Closures / Refs ────────────────────────────────────────────────── */

    case IRON_IR_MAKE_CLOSURE: {
        iron_strbuf_appendf(sb, "  %%%u = make_closure @%s(",
                            instr->id, instr->make_closure.lifted_func_name);
        for (int i = 0; i < instr->make_closure.capture_count; i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            iron_strbuf_appendf(sb, "%%%u", instr->make_closure.captures[i]);
        }
        iron_strbuf_appendf(sb, ") : ");
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;
    }

    case IRON_IR_FUNC_REF:
        iron_strbuf_appendf(sb, "  %%%u = func_ref @%s : ",
                            instr->id, instr->func_ref.func_name);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    /* ── Concurrency ────────────────────────────────────────────────────── */

    case IRON_IR_SPAWN:
        if (instr->spawn.pool_val == IRON_IR_VALUE_INVALID) {
            iron_strbuf_appendf(sb, "  %%%u = spawn @%s, pool: default\n",
                                instr->id, instr->spawn.lifted_func_name);
        } else {
            iron_strbuf_appendf(sb, "  %%%u = spawn @%s, pool: %%%u\n",
                                instr->id, instr->spawn.lifted_func_name,
                                instr->spawn.pool_val);
        }
        break;

    case IRON_IR_PARALLEL_FOR: {
        if (instr->parallel_for.pool_val == IRON_IR_VALUE_INVALID) {
            iron_strbuf_appendf(sb, "  parallel_for %s, %%%u, chunk: @%s, pool: default",
                                instr->parallel_for.loop_var_name,
                                instr->parallel_for.range_val,
                                instr->parallel_for.chunk_func_name);
        } else {
            iron_strbuf_appendf(sb, "  parallel_for %s, %%%u, chunk: @%s, pool: %%%u",
                                instr->parallel_for.loop_var_name,
                                instr->parallel_for.range_val,
                                instr->parallel_for.chunk_func_name,
                                instr->parallel_for.pool_val);
        }
        if (instr->parallel_for.capture_count > 0) {
            iron_strbuf_appendf(sb, ", captures: [");
            for (int i = 0; i < instr->parallel_for.capture_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ", ");
                iron_strbuf_appendf(sb, "%%%u", instr->parallel_for.captures[i]);
            }
            iron_strbuf_appendf(sb, "]");
        }
        iron_strbuf_appendf(sb, "\n");
        break;
    }

    case IRON_IR_AWAIT:
        iron_strbuf_appendf(sb, "  %%%u = await %%%u : ",
                            instr->id, instr->await.handle);
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;

    /* ── SSA Phi ────────────────────────────────────────────────────────── */

    case IRON_IR_PHI: {
        iron_strbuf_appendf(sb, "  %%%u = phi [", instr->id);
        for (int i = 0; i < instr->phi.count; i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            iron_strbuf_appendf(sb, "%%%u: %s",
                                instr->phi.values[i],
                                resolve_block_label(fn, instr->phi.pred_blocks[i]));
        }
        iron_strbuf_appendf(sb, "] : ");
        append_type(sb, instr->type, tmp);
        iron_strbuf_appendf(sb, "\n");
        break;
    }

    /* ── Poison ─────────────────────────────────────────────────────────── */

    case IRON_IR_POISON:
        iron_strbuf_appendf(sb, "  %%%u = poison", instr->id);
        if (instr->type) {
            iron_strbuf_appendf(sb, " : ");
            append_type(sb, instr->type, tmp);
        }
        iron_strbuf_appendf(sb, "\n");
        break;

    default:
        assert(false && "unhandled IronIR_InstrKind in printer");
        break;
    }
}

/* ── Block printer ────────────────────────────────────────────────────────── */

static void print_block(Iron_StrBuf *sb, const IronIR_Block *block,
                         const IronIR_Func *fn, bool show_annotations,
                         Iron_Arena *tmp) {
    iron_strbuf_appendf(sb, "%s:", block->label);

    if (show_annotations && block->preds && arrlen(block->preds) > 0) {
        iron_strbuf_appendf(sb, " ; preds: ");
        for (int i = 0; i < (int)arrlen(block->preds); i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            iron_strbuf_appendf(sb, "%s", resolve_block_label(fn, block->preds[i]));
        }
    }
    iron_strbuf_appendf(sb, "\n");

    for (int i = 0; i < block->instr_count; i++) {
        print_instr(sb, block->instrs[i], fn, show_annotations, tmp);
    }
}

/* ── Function printer ─────────────────────────────────────────────────────── */

static void print_func(Iron_StrBuf *sb, const IronIR_Func *fn,
                        bool show_annotations, Iron_Arena *tmp) {
    iron_strbuf_appendf(sb, "func @%s(", fn->name);
    for (int i = 0; i < fn->param_count; i++) {
        if (i > 0) iron_strbuf_appendf(sb, ", ");
        iron_strbuf_appendf(sb, "%s: ", fn->params[i].name);
        append_type(sb, fn->params[i].type, tmp);
    }
    iron_strbuf_appendf(sb, ") -> ");
    append_type(sb, fn->return_type, tmp);
    iron_strbuf_appendf(sb, " {\n");

    for (int i = 0; i < fn->block_count; i++) {
        print_block(sb, fn->blocks[i], fn, show_annotations, tmp);
    }

    iron_strbuf_appendf(sb, "}\n");
}

/* ── Public API ───────────────────────────────────────────────────────────── */

char *iron_ir_print(const IronIR_Module *module, bool show_annotations) {
    if (!module) return NULL;

    Iron_StrBuf sb = iron_strbuf_create(4096);

    /* Temporary arena for iron_type_to_string calls */
    Iron_Arena tmp = iron_arena_create(4096);
    iron_types_init(&tmp);

    /* Module header */
    iron_strbuf_appendf(&sb, "; Module: %s\n", module->name);

    /* Type declarations */
    for (int i = 0; i < module->type_decl_count; i++) {
        const IronIR_TypeDecl *td = module->type_decls[i];
        const char *kind_str;
        switch (td->kind) {
        case IRON_IR_TYPE_OBJECT:    kind_str = "type";      break;
        case IRON_IR_TYPE_ENUM:      kind_str = "type";      break;
        case IRON_IR_TYPE_INTERFACE: kind_str = "interface"; break;
        default:                     kind_str = "type";      break;
        }
        iron_strbuf_appendf(&sb, "%s @%s = ", kind_str, td->name);
        append_type(&sb, td->type, &tmp);
        iron_strbuf_appendf(&sb, "\n");
    }

    /* Extern declarations */
    for (int i = 0; i < module->extern_decl_count; i++) {
        const IronIR_ExternDecl *ed = module->extern_decls[i];
        iron_strbuf_appendf(&sb, "extern @%s(", ed->c_name);
        for (int j = 0; j < ed->param_count; j++) {
            if (j > 0) iron_strbuf_appendf(&sb, ", ");
            append_type(&sb, ed->param_types[j], &tmp);
        }
        iron_strbuf_appendf(&sb, ") -> ");
        append_type(&sb, ed->return_type, &tmp);
        iron_strbuf_appendf(&sb, "\n");
    }

    /* Functions */
    for (int i = 0; i < module->func_count; i++) {
        iron_strbuf_appendf(&sb, "\n");
        print_func(&sb, module->funcs[i], show_annotations, &tmp);
    }

    /* Copy to malloc'd string for caller */
    const char *content = iron_strbuf_get(&sb);
    size_t len = strlen(content);
    char *result = malloc(len + 1);
    if (result) {
        memcpy(result, content, len + 1);
    }

    iron_strbuf_free(&sb);
    iron_arena_free(&tmp);

    return result;
}
