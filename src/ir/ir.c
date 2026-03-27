#include "ir/ir.h"
#include <string.h>

/* ── Internal helper ──────────────────────────────────────────────────────── */

/* Allocate and initialize a new instruction, append to block, register in
 * value_table if it produces a value. */
static IronIR_Instr *alloc_instr(IronIR_Func *fn, IronIR_Block *block,
                                  IronIR_InstrKind kind, Iron_Type *type,
                                  Iron_Span span, bool produces_value) {
    IronIR_Instr *instr = ARENA_ALLOC(fn->arena, IronIR_Instr);
    memset(instr, 0, sizeof(*instr));
    instr->kind = kind;
    instr->type = type;
    instr->span = span;

    if (produces_value) {
        instr->id = fn->next_value_id++;
        /* Grow value_table to accommodate this id */
        while (arrlen(fn->value_table) <= (ptrdiff_t)instr->id) {
            arrput(fn->value_table, NULL);
        }
        fn->value_table[instr->id] = instr;
    } else {
        instr->id = IRON_IR_VALUE_INVALID;
    }

    arrput(block->instrs, instr);
    block->instr_count = (int)arrlen(block->instrs);

    return instr;
}

/* ── Module ───────────────────────────────────────────────────────────────── */

IronIR_Module *iron_ir_module_create(Iron_Arena *ir_arena, const char *name) {
    IronIR_Module *mod = ARENA_ALLOC(ir_arena, IronIR_Module);
    memset(mod, 0, sizeof(*mod));
    mod->name  = name;
    mod->arena = ir_arena;
    /* stb_ds convention: NULL = empty array/map */
    mod->type_decls    = NULL;
    mod->extern_decls  = NULL;
    mod->funcs         = NULL;
    mod->mono_registry = NULL;
    return mod;
}

void iron_ir_module_destroy(IronIR_Module *mod) {
    if (!mod) return;

    /* Walk functions */
    for (int fi = 0; fi < mod->func_count; fi++) {
        IronIR_Func *fn = mod->funcs[fi];
        if (!fn) continue;

        /* Walk blocks */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *blk = fn->blocks[bi];
            if (!blk) continue;

            /* Free per-instruction stb_ds arrays */
            for (int ii = 0; ii < blk->instr_count; ii++) {
                IronIR_Instr *instr = blk->instrs[ii];
                if (!instr) continue;
                switch (instr->kind) {
                    case IRON_IR_CALL:
                        arrfree(instr->call.args);
                        break;
                    case IRON_IR_SWITCH:
                        arrfree(instr->sw.case_values);
                        arrfree(instr->sw.case_blocks);
                        break;
                    case IRON_IR_CONSTRUCT:
                        arrfree(instr->construct.field_vals);
                        break;
                    case IRON_IR_ARRAY_LIT:
                        arrfree(instr->array_lit.elements);
                        break;
                    case IRON_IR_INTERP_STRING:
                        arrfree(instr->interp_string.parts);
                        break;
                    case IRON_IR_MAKE_CLOSURE:
                        arrfree(instr->make_closure.captures);
                        break;
                    case IRON_IR_PARALLEL_FOR:
                        arrfree(instr->parallel_for.captures);
                        break;
                    case IRON_IR_PHI:
                        arrfree(instr->phi.values);
                        arrfree(instr->phi.pred_blocks);
                        break;
                    default:
                        break;
                }
            }

            arrfree(blk->instrs);
            arrfree(blk->preds);
            arrfree(blk->succs);
            shfree(blk->var_defs);
            shfree(blk->incomplete_phis);
        }

        arrfree(fn->blocks);
        arrfree(fn->value_table);
    }

    arrfree(mod->funcs);
    arrfree(mod->type_decls);
    arrfree(mod->extern_decls);
    shfree(mod->mono_registry);
    /* Caller is responsible for iron_arena_free() on the arena */
}

/* ── Function ─────────────────────────────────────────────────────────────── */

IronIR_Func *iron_ir_func_create(IronIR_Module *mod, const char *name,
                                  IronIR_Param *params, int param_count,
                                  Iron_Type *return_type) {
    IronIR_Func *fn = ARENA_ALLOC(mod->arena, IronIR_Func);
    memset(fn, 0, sizeof(*fn));
    fn->name         = name;
    fn->return_type  = return_type;
    fn->params       = params;
    fn->param_count  = param_count;
    fn->next_value_id = 1;
    fn->next_block_id = 1;
    fn->arena        = mod->arena;
    fn->blocks       = NULL;
    fn->value_table  = NULL;
    /* value_table[0] = NULL (IRON_IR_VALUE_INVALID slot) */
    arrput(fn->value_table, NULL);

    arrput(mod->funcs, fn);
    mod->func_count = (int)arrlen(mod->funcs);
    return fn;
}

/* ── Block ────────────────────────────────────────────────────────────────── */

IronIR_Block *iron_ir_block_create(IronIR_Func *func, const char *label) {
    IronIR_Block *blk = ARENA_ALLOC(func->arena, IronIR_Block);
    memset(blk, 0, sizeof(*blk));
    blk->id    = func->next_block_id++;
    blk->label = label;
    blk->instrs          = NULL;
    blk->preds           = NULL;
    blk->succs           = NULL;
    blk->var_defs        = NULL;
    blk->incomplete_phis = NULL;

    arrput(func->blocks, blk);
    func->block_count = (int)arrlen(func->blocks);
    return blk;
}

/* ── Instruction constructors ─────────────────────────────────────────────── */

IronIR_Instr *iron_ir_const_int(IronIR_Func *fn, IronIR_Block *block,
                                 int64_t value, Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_CONST_INT, type, span, true);
    i->const_int.value = value;
    return i;
}

IronIR_Instr *iron_ir_const_float(IronIR_Func *fn, IronIR_Block *block,
                                   double value, Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_CONST_FLOAT, type, span, true);
    i->const_float.value = value;
    return i;
}

IronIR_Instr *iron_ir_const_bool(IronIR_Func *fn, IronIR_Block *block,
                                  bool value, Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_CONST_BOOL, type, span, true);
    i->const_bool.value = value;
    return i;
}

IronIR_Instr *iron_ir_const_string(IronIR_Func *fn, IronIR_Block *block,
                                    const char *value, Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_CONST_STRING, type, span, true);
    i->const_str.value = value;
    return i;
}

IronIR_Instr *iron_ir_const_null(IronIR_Func *fn, IronIR_Block *block,
                                  Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_CONST_NULL, type, span, true);
    i->const_null._pad = 0;
    return i;
}

IronIR_Instr *iron_ir_binop(IronIR_Func *fn, IronIR_Block *block,
                             IronIR_InstrKind kind,
                             IronIR_ValueId left, IronIR_ValueId right,
                             Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, kind, type, span, true);
    i->binop.left  = left;
    i->binop.right = right;
    return i;
}

IronIR_Instr *iron_ir_unop(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_InstrKind kind, IronIR_ValueId operand,
                            Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, kind, type, span, true);
    i->unop.operand = operand;
    return i;
}

IronIR_Instr *iron_ir_alloca(IronIR_Func *fn, IronIR_Block *block,
                              Iron_Type *alloc_type, const char *name_hint,
                              Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_ALLOCA, alloc_type, span, true);
    i->alloca.alloc_type = alloc_type;
    i->alloca.name_hint  = name_hint;
    return i;
}

IronIR_Instr *iron_ir_load(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_ValueId ptr, Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_LOAD, type, span, true);
    i->load.ptr = ptr;
    return i;
}

IronIR_Instr *iron_ir_store(IronIR_Func *fn, IronIR_Block *block,
                             IronIR_ValueId ptr, IronIR_ValueId value,
                             Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_STORE, NULL, span, false);
    i->store.ptr   = ptr;
    i->store.value = value;
    return i;
}

IronIR_Instr *iron_ir_get_field(IronIR_Func *fn, IronIR_Block *block,
                                 IronIR_ValueId object, const char *field,
                                 Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_GET_FIELD, type, span, true);
    i->field.object = object;
    i->field.field  = field;
    i->field.value  = IRON_IR_VALUE_INVALID;
    return i;
}

IronIR_Instr *iron_ir_set_field(IronIR_Func *fn, IronIR_Block *block,
                                 IronIR_ValueId object, const char *field,
                                 IronIR_ValueId value, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_SET_FIELD, NULL, span, false);
    i->field.object = object;
    i->field.field  = field;
    i->field.value  = value;
    return i;
}

IronIR_Instr *iron_ir_get_index(IronIR_Func *fn, IronIR_Block *block,
                                 IronIR_ValueId array, IronIR_ValueId idx,
                                 Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_GET_INDEX, type, span, true);
    i->index.array = array;
    i->index.index = idx;
    i->index.value = IRON_IR_VALUE_INVALID;
    return i;
}

IronIR_Instr *iron_ir_set_index(IronIR_Func *fn, IronIR_Block *block,
                                 IronIR_ValueId array, IronIR_ValueId idx,
                                 IronIR_ValueId value, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_SET_INDEX, NULL, span, false);
    i->index.array = array;
    i->index.index = idx;
    i->index.value = value;
    return i;
}

IronIR_Instr *iron_ir_call(IronIR_Func *fn, IronIR_Block *block,
                            Iron_FuncDecl *func_decl, IronIR_ValueId func_ptr,
                            IronIR_ValueId *args, int arg_count,
                            Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_CALL, type, span, true);
    i->call.func_decl = func_decl;
    i->call.func_ptr  = func_ptr;
    i->call.arg_count = arg_count;
    i->call.args      = NULL;
    for (int k = 0; k < arg_count; k++) {
        arrput(i->call.args, args[k]);
    }
    return i;
}

IronIR_Instr *iron_ir_jump(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_BlockId target, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_JUMP, NULL, span, false);
    i->jump.target = target;
    return i;
}

IronIR_Instr *iron_ir_branch(IronIR_Func *fn, IronIR_Block *block,
                              IronIR_ValueId cond,
                              IronIR_BlockId then_block, IronIR_BlockId else_block,
                              Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_BRANCH, NULL, span, false);
    i->branch.cond       = cond;
    i->branch.then_block = then_block;
    i->branch.else_block = else_block;
    return i;
}

IronIR_Instr *iron_ir_switch(IronIR_Func *fn, IronIR_Block *block,
                              IronIR_ValueId subject, IronIR_BlockId default_block,
                              int *case_values, IronIR_BlockId *case_blocks,
                              int case_count, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_SWITCH, NULL, span, false);
    i->sw.subject       = subject;
    i->sw.default_block = default_block;
    i->sw.case_count    = case_count;
    i->sw.case_values   = NULL;
    i->sw.case_blocks   = NULL;
    for (int k = 0; k < case_count; k++) {
        arrput(i->sw.case_values, case_values[k]);
        arrput(i->sw.case_blocks, case_blocks[k]);
    }
    return i;
}

IronIR_Instr *iron_ir_return(IronIR_Func *fn, IronIR_Block *block,
                              IronIR_ValueId value, bool is_void,
                              Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_RETURN, type, span, false);
    i->ret.value   = value;
    i->ret.is_void = is_void;
    return i;
}

IronIR_Instr *iron_ir_cast(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_ValueId value, Iron_Type *target_type,
                            Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_CAST, target_type, span, true);
    i->cast.value       = value;
    i->cast.target_type = target_type;
    return i;
}

IronIR_Instr *iron_ir_heap_alloc(IronIR_Func *fn, IronIR_Block *block,
                                  IronIR_ValueId inner_val,
                                  bool auto_free, bool escapes,
                                  Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_HEAP_ALLOC, type, span, true);
    i->heap_alloc.inner_val = inner_val;
    i->heap_alloc.auto_free = auto_free;
    i->heap_alloc.escapes   = escapes;
    return i;
}

IronIR_Instr *iron_ir_rc_alloc(IronIR_Func *fn, IronIR_Block *block,
                                IronIR_ValueId inner_val,
                                Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_RC_ALLOC, type, span, true);
    i->rc_alloc.inner_val = inner_val;
    return i;
}

IronIR_Instr *iron_ir_free(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_ValueId value, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_FREE, NULL, span, false);
    i->free_instr.value = value;
    return i;
}

IronIR_Instr *iron_ir_construct(IronIR_Func *fn, IronIR_Block *block,
                                 Iron_Type *type,
                                 IronIR_ValueId *field_vals, int field_count,
                                 Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_CONSTRUCT, type, span, true);
    i->construct.type        = type;
    i->construct.field_count = field_count;
    i->construct.field_vals  = NULL;
    for (int k = 0; k < field_count; k++) {
        arrput(i->construct.field_vals, field_vals[k]);
    }
    return i;
}

IronIR_Instr *iron_ir_array_lit(IronIR_Func *fn, IronIR_Block *block,
                                 Iron_Type *elem_type,
                                 IronIR_ValueId *elements, int element_count,
                                 Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_ARRAY_LIT, type, span, true);
    i->array_lit.elem_type     = elem_type;
    i->array_lit.element_count = element_count;
    i->array_lit.elements      = NULL;
    for (int k = 0; k < element_count; k++) {
        arrput(i->array_lit.elements, elements[k]);
    }
    return i;
}

IronIR_Instr *iron_ir_slice(IronIR_Func *fn, IronIR_Block *block,
                             IronIR_ValueId array,
                             IronIR_ValueId start, IronIR_ValueId end,
                             Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_SLICE, type, span, true);
    i->slice.array = array;
    i->slice.start = start;
    i->slice.end   = end;
    return i;
}

IronIR_Instr *iron_ir_is_null(IronIR_Func *fn, IronIR_Block *block,
                               IronIR_ValueId value, Iron_Span span) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_IS_NULL, bool_type, span, true);
    i->null_check.value = value;
    return i;
}

IronIR_Instr *iron_ir_is_not_null(IronIR_Func *fn, IronIR_Block *block,
                                   IronIR_ValueId value, Iron_Span span) {
    Iron_Type *bool_type = iron_type_make_primitive(IRON_TYPE_BOOL);
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_IS_NOT_NULL, bool_type, span, true);
    i->null_check.value = value;
    return i;
}

IronIR_Instr *iron_ir_interp_string(IronIR_Func *fn, IronIR_Block *block,
                                     IronIR_ValueId *parts, int part_count,
                                     Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_INTERP_STRING, type, span, true);
    i->interp_string.part_count = part_count;
    i->interp_string.parts      = NULL;
    for (int k = 0; k < part_count; k++) {
        arrput(i->interp_string.parts, parts[k]);
    }
    return i;
}

IronIR_Instr *iron_ir_make_closure(IronIR_Func *fn, IronIR_Block *block,
                                    const char *lifted_func_name,
                                    IronIR_ValueId *captures, int capture_count,
                                    Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_MAKE_CLOSURE, type, span, true);
    i->make_closure.lifted_func_name = lifted_func_name;
    i->make_closure.capture_count   = capture_count;
    i->make_closure.captures        = NULL;
    for (int k = 0; k < capture_count; k++) {
        arrput(i->make_closure.captures, captures[k]);
    }
    return i;
}

IronIR_Instr *iron_ir_func_ref(IronIR_Func *fn, IronIR_Block *block,
                                const char *func_name,
                                Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_FUNC_REF, type, span, true);
    i->func_ref.func_name = func_name;
    return i;
}

IronIR_Instr *iron_ir_spawn(IronIR_Func *fn, IronIR_Block *block,
                             const char *lifted_func_name,
                             IronIR_ValueId pool_val, const char *handle_name,
                             Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_SPAWN, type, span, true);
    i->spawn.lifted_func_name = lifted_func_name;
    i->spawn.pool_val         = pool_val;
    i->spawn.handle_name      = handle_name;
    return i;
}

IronIR_Instr *iron_ir_parallel_for(IronIR_Func *fn, IronIR_Block *block,
                                    const char *loop_var_name,
                                    IronIR_ValueId range_val,
                                    const char *chunk_func_name,
                                    IronIR_ValueId pool_val,
                                    IronIR_ValueId *captures, int capture_count,
                                    Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_PARALLEL_FOR, NULL, span, false);
    i->parallel_for.loop_var_name   = loop_var_name;
    i->parallel_for.range_val       = range_val;
    i->parallel_for.chunk_func_name = chunk_func_name;
    i->parallel_for.pool_val        = pool_val;
    i->parallel_for.capture_count   = capture_count;
    i->parallel_for.captures        = NULL;
    for (int k = 0; k < capture_count; k++) {
        arrput(i->parallel_for.captures, captures[k]);
    }
    return i;
}

IronIR_Instr *iron_ir_await(IronIR_Func *fn, IronIR_Block *block,
                             IronIR_ValueId handle,
                             Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_AWAIT, type, span, true);
    i->await.handle = handle;
    return i;
}

IronIR_Instr *iron_ir_phi(IronIR_Func *fn, IronIR_Block *block,
                           Iron_Type *type, Iron_Span span) {
    IronIR_Instr *i = alloc_instr(fn, block, IRON_IR_PHI, type, span, true);
    i->phi.values      = NULL;
    i->phi.pred_blocks = NULL;
    i->phi.count       = 0;
    return i;
}

/* ── Phi manipulation ─────────────────────────────────────────────────────── */

void iron_ir_phi_add_incoming(IronIR_Instr *phi, IronIR_ValueId value,
                               IronIR_BlockId pred_block) {
    arrput(phi->phi.values, value);
    arrput(phi->phi.pred_blocks, pred_block);
    phi->phi.count++;
}

/* ── Helpers ──────────────────────────────────────────────────────────────── */

bool iron_ir_is_terminator(IronIR_InstrKind kind) {
    return kind == IRON_IR_JUMP   ||
           kind == IRON_IR_BRANCH ||
           kind == IRON_IR_SWITCH ||
           kind == IRON_IR_RETURN;
}

/* ── Module helpers ───────────────────────────────────────────────────────── */

void iron_ir_module_add_type_decl(IronIR_Module *mod, IronIR_TypeDeclKind kind,
                                   const char *name, Iron_Type *type) {
    IronIR_TypeDecl *decl = ARENA_ALLOC(mod->arena, IronIR_TypeDecl);
    decl->kind = kind;
    decl->name = name;
    decl->type = type;
    arrput(mod->type_decls, decl);
    mod->type_decl_count = (int)arrlen(mod->type_decls);
}

void iron_ir_module_add_extern(IronIR_Module *mod,
                                const char *iron_name, const char *c_name,
                                Iron_Type **param_types, int param_count,
                                Iron_Type *return_type) {
    IronIR_ExternDecl *decl = ARENA_ALLOC(mod->arena, IronIR_ExternDecl);
    decl->iron_name   = iron_name;
    decl->c_name      = c_name;
    decl->param_types = param_types;
    decl->param_count = param_count;
    decl->return_type = return_type;
    arrput(mod->extern_decls, decl);
    mod->extern_decl_count = (int)arrlen(mod->extern_decls);
}
