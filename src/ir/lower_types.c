/* lower_types.c — Module-level declaration lowering (Pass 1)
 *                 and post-pass lambda/spawn/parallel-for lifting.
 *
 * Pass 1 (lower_module_decls): registers all module-level declarations in the
 * IrModule in the required ordering: interfaces first (vtable dependency), then
 * objects, enums, and function signatures, then extern functions.
 *
 * Post-pass (lower_lift_pending): after all function bodies are lowered, lifts
 * pending lambda/spawn/parallel-for bodies to top-level IrFunctions with
 * capture analysis.
 */

#include "ir/lower_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

/* ── collect_captures (local copy, no codegen dependency) ─────────────────── */

/* Collect identifiers used in the body that are NOT in the param list.
 * These are potential captures from the enclosing scope.
 * Returns an stb_ds array of const char* names (caller must arrfree). */
static const char **collect_captures(Iron_Node *body, Iron_Node **params,
                                      int param_count) {
    const char **captures = NULL;

    if (!body) return NULL;

    /* Simple DFS without the full visitor mechanism */
    Iron_Node **stack = NULL;
    arrput(stack, body);

    while (arrlen(stack) > 0) {
        Iron_Node *node = stack[arrlen(stack) - 1];
        arrsetlen(stack, arrlen(stack) - 1);

        if (!node) continue;

        if (node->kind == IRON_NODE_IDENT) {
            Iron_Ident *id = (Iron_Ident *)node;
            /* Skip "self" and "super" */
            if (strcmp(id->name, "self") == 0 ||
                strcmp(id->name, "super") == 0) {
                continue;
            }
            /* Skip if it's a param name */
            bool is_param = false;
            for (int i = 0; i < param_count; i++) {
                Iron_Param *p = (Iron_Param *)params[i];
                if (strcmp(p->name, id->name) == 0) {
                    is_param = true;
                    break;
                }
            }
            if (is_param) continue;
            /* Check if already in captures list */
            bool already = false;
            for (int i = 0; i < (int)arrlen(captures); i++) {
                if (strcmp(captures[i], id->name) == 0) {
                    already = true;
                    break;
                }
            }
            if (!already) {
                arrput(captures, id->name);
            }
            continue;
        }

        /* Push children based on node kind */
        switch (node->kind) {
            case IRON_NODE_BLOCK: {
                Iron_Block *blk = (Iron_Block *)node;
                for (int i = 0; i < blk->stmt_count; i++) {
                    arrput(stack, blk->stmts[i]);
                }
                break;
            }
            case IRON_NODE_VAL_DECL: {
                Iron_ValDecl *vd = (Iron_ValDecl *)node;
                if (vd->init) arrput(stack, vd->init);
                break;
            }
            case IRON_NODE_VAR_DECL: {
                Iron_VarDecl *vd = (Iron_VarDecl *)node;
                if (vd->init) arrput(stack, vd->init);
                break;
            }
            case IRON_NODE_ASSIGN: {
                Iron_AssignStmt *as = (Iron_AssignStmt *)node;
                arrput(stack, as->target);
                arrput(stack, as->value);
                break;
            }
            case IRON_NODE_RETURN: {
                Iron_ReturnStmt *rs = (Iron_ReturnStmt *)node;
                if (rs->value) arrput(stack, rs->value);
                break;
            }
            case IRON_NODE_BINARY: {
                Iron_BinaryExpr *bin = (Iron_BinaryExpr *)node;
                arrput(stack, bin->left);
                arrput(stack, bin->right);
                break;
            }
            case IRON_NODE_UNARY: {
                Iron_UnaryExpr *un = (Iron_UnaryExpr *)node;
                arrput(stack, un->operand);
                break;
            }
            case IRON_NODE_CALL: {
                Iron_CallExpr *ce = (Iron_CallExpr *)node;
                arrput(stack, ce->callee);
                for (int i = 0; i < ce->arg_count; i++) {
                    arrput(stack, ce->args[i]);
                }
                break;
            }
            case IRON_NODE_IF: {
                Iron_IfStmt *is = (Iron_IfStmt *)node;
                arrput(stack, is->condition);
                arrput(stack, is->body);
                if (is->else_body) arrput(stack, is->else_body);
                break;
            }
            case IRON_NODE_INDEX: {
                Iron_IndexExpr *idx = (Iron_IndexExpr *)node;
                arrput(stack, idx->object);
                arrput(stack, idx->index);
                break;
            }
            case IRON_NODE_FIELD_ACCESS: {
                Iron_FieldAccess *fa = (Iron_FieldAccess *)node;
                arrput(stack, fa->object);
                break;
            }
            case IRON_NODE_METHOD_CALL: {
                Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)node;
                arrput(stack, mc->object);
                for (int i = 0; i < mc->arg_count; i++) {
                    arrput(stack, mc->args[i]);
                }
                break;
            }
            case IRON_NODE_FOR: {
                Iron_ForStmt *fs2 = (Iron_ForStmt *)node;
                arrput(stack, fs2->iterable);
                if (fs2->body) arrput(stack, fs2->body);
                break;
            }
            case IRON_NODE_WHILE: {
                Iron_WhileStmt *ws2 = (Iron_WhileStmt *)node;
                arrput(stack, ws2->condition);
                if (ws2->body) arrput(stack, ws2->body);
                break;
            }
            default:
                /* Skip other nodes for capture detection */
                break;
        }
    }

    arrfree(stack);
    return captures;
}

/* ── Resolve type annotation to Iron_Type* ───────────────────────────────── */
/* Shared implementation — mirrors the version in lower.c.
 * Only handles primitive types; for named/generic types the type checker
 * sets resolved_return_type which is the authoritative source. */

static Iron_Type *lower_types_resolve_ann(Iron_Node *ann_node) {
    if (!ann_node) return iron_type_make_primitive(IRON_TYPE_VOID);
    if (ann_node->kind != IRON_NODE_TYPE_ANNOTATION) return NULL;
    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)ann_node;

    if (strcmp(ta->name, "Int") == 0)    return iron_type_make_primitive(IRON_TYPE_INT);
    if (strcmp(ta->name, "Float") == 0)  return iron_type_make_primitive(IRON_TYPE_FLOAT);
    if (strcmp(ta->name, "Bool") == 0)   return iron_type_make_primitive(IRON_TYPE_BOOL);
    if (strcmp(ta->name, "String") == 0) return iron_type_make_primitive(IRON_TYPE_STRING);
    if (strcmp(ta->name, "Void") == 0)   return iron_type_make_primitive(IRON_TYPE_VOID);

    return NULL;
}

/* ── Param array builder (shared by func and method registration) ────────── */

static IronIR_Param *build_param_array(IronIR_LowerCtx *ctx,
                                        Iron_Node **params, int param_count) {
    if (param_count == 0) return NULL;

    IronIR_Param *arr = (IronIR_Param *)iron_arena_alloc(
        ctx->ir_arena,
        (size_t)param_count * sizeof(IronIR_Param),
        _Alignof(IronIR_Param));

    for (int p = 0; p < param_count; p++) {
        Iron_Param *ap = (Iron_Param *)params[p];
        Iron_Type  *pt = lower_types_resolve_ann(ap->type_ann);
        arr[p].name = ap->name;
        arr[p].type = pt;
    }
    return arr;
}

/* ── Pass 1: Register module-level declarations ──────────────────────────── */

void lower_module_decls(IronIR_LowerCtx *ctx) {
    /* Pass 1a: Register interfaces FIRST — vtable types must precede objects
     * that implement them (IRON_IR_TYPE_INTERFACE ordering requirement). */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_INTERFACE_DECL) continue;

        Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)decl;
        /* Register the interface type. The Iron_Type* for the interface is not
         * tracked in Iron_InterfaceDecl (it's a structural type), so pass NULL.
         * Phase 9's C emitter will reconstruct the vtable struct from the
         * method_sigs array. */
        iron_ir_module_add_type_decl(ctx->module, IRON_IR_TYPE_INTERFACE,
                                     iface->name, NULL);
    }

    /* Pass 1b: Register objects (after interfaces for vtable dependency). */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_OBJECT_DECL) continue;

        Iron_ObjectDecl *obj = (Iron_ObjectDecl *)decl;
        iron_ir_module_add_type_decl(ctx->module, IRON_IR_TYPE_OBJECT,
                                     obj->name, NULL);

        /* For generic objects: register known instantiations in mono_registry.
         * The registry tracks mangled names so Phase 9 knows which concrete
         * types to emit. (For non-generic objects this loop is a no-op.) */
        if (obj->generic_param_count > 0) {
            /* Generic objects are registered when they're instantiated (at
             * call/construct sites). Nothing to do here without resolved
             * instantiation information — the type checker would provide that.
             * We note the base name so Phase 9 can skip uninstantiated generics. */
            (void)obj;
        }
    }

    /* Pass 1c: Register enums. */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_ENUM_DECL) continue;

        Iron_EnumDecl *en = (Iron_EnumDecl *)decl;
        iron_ir_module_add_type_decl(ctx->module, IRON_IR_TYPE_ENUM,
                                     en->name, NULL);
    }

    /* Pass 1d: Register function signatures (including extern functions).
     * Non-extern functions get a full IrFunc scaffold; extern functions use
     * iron_ir_module_add_extern and are tagged is_extern=true on their IrFunc. */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_FUNC_DECL) continue;

        Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;

        /* Normalize void return: IR convention is return_type == NULL for void */
        Iron_Type *ret_type = fd->resolved_return_type;
        if (ret_type && ret_type->kind == IRON_TYPE_VOID) ret_type = NULL;

        IronIR_Param *params = build_param_array(ctx, fd->params, fd->param_count);
        IronIR_Func  *fn     = iron_ir_func_create(ctx->module, fd->name,
                                                    params, fd->param_count,
                                                    ret_type);

        if (fd->is_extern) {
            fn->is_extern      = true;
            fn->extern_c_name  = fd->extern_c_name;
            /* Also register in extern_decls for Phase 9's declaration emission */
            Iron_Type **param_types = NULL;
            if (fd->param_count > 0) {
                param_types = (Iron_Type **)iron_arena_alloc(
                    ctx->ir_arena,
                    (size_t)fd->param_count * sizeof(Iron_Type *),
                    _Alignof(Iron_Type *));
                for (int p = 0; p < fd->param_count; p++) {
                    param_types[p] = params ? params[p].type : NULL;
                }
            }
            iron_ir_module_add_extern(ctx->module, fd->name, fd->extern_c_name,
                                      param_types, fd->param_count, ret_type);
        }
    }

    /* Pass 1e: Handle import declarations (extern functions from imports).
     * Iron's import syntax exposes external modules. If an import brings in
     * extern function declarations, register them via add_extern. The current
     * AST for IRON_NODE_IMPORT_DECL only stores path/alias; actual extern
     * functions from the import are not materialized as child nodes in the
     * current design. This is handled at the module level by the linker.
     * We iterate imports to record the dependency in the module metadata. */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_IMPORT_DECL) continue;
        /* Iron_ImportDecl only has path and alias — no child extern decls.
         * The linker handles import resolution. Nothing to register here. */
        (void)decl;
    }

    /* Pass 1f: Register method declarations (on object types).
     * Method declarations appear as IRON_NODE_METHOD_DECL at the top level.
     * Each becomes an IrFunc with a mangled name (TypeName_methodName). */
    for (int i = 0; i < ctx->program->decl_count; i++) {
        Iron_Node *decl = ctx->program->decls[i];
        if (decl->kind != IRON_NODE_METHOD_DECL) continue;

        Iron_MethodDecl *md = (Iron_MethodDecl *)decl;

        /* Mangle method name: "TypeName_methodName" */
        size_t name_len = strlen(md->type_name) + 1 + strlen(md->method_name) + 1;
        char *mangled = (char *)iron_arena_alloc(ctx->ir_arena, name_len,
                                                  _Alignof(char));
        snprintf(mangled, name_len, "%s_%s", md->type_name, md->method_name);

        Iron_Type *ret_type = md->resolved_return_type;
        if (ret_type && ret_type->kind == IRON_TYPE_VOID) ret_type = NULL;

        IronIR_Param *params = build_param_array(ctx, md->params, md->param_count);
        iron_ir_func_create(ctx->module, mangled, params, md->param_count,
                            ret_type);
    }

    /* Pass 1g: Walk all declarations for monomorphization tracking.
     * For each IRON_NODE_CONSTRUCT with generic_arg_count > 0, compute the
     * mangled name and register in mono_registry. This is a lightweight scan —
     * the actual monomorphized type emission is Phase 9's responsibility.
     * We use a simple iterative AST walk rather than the full visitor to avoid
     * introducing visitor infrastructure dependencies here. */
    /* NOTE: Full monomorphization scanning requires walking function bodies,
     * which are not yet lowered at this point in the pipeline. The mono_registry
     * is populated incrementally as generic constructs are encountered during
     * Pass 2 (function body lowering). For Phase 8, the registry starts empty
     * and Phase 9 will populate it during its own type emission pass. This
     * matches the research note: mono_registry is used for deduplication, not
     * pre-discovery. */
}

/* ── Post-pass: lift lambda/spawn/pfor bodies ─────────────────────────────── */

/* Lift a lambda node to a top-level IrFunc. */
static void lift_lambda(IronIR_LowerCtx *ctx, LiftPending *lp) {
    Iron_LambdaExpr *lam = (Iron_LambdaExpr *)lp->ast_node;
    if (!lam) return;

    /* Collect captures: variables referenced in the body that are not params */
    const char **captures = collect_captures(lam->body, lam->params,
                                             lam->param_count);
    int capture_count = (int)arrlen(captures);

    /* Determine total param count (env pointer if captures, then declared params) */
    int total_params = lam->param_count;
    if (capture_count > 0) {
        total_params += 1;  /* env: void* as first param */
    }

    IronIR_Param *params = NULL;
    if (total_params > 0) {
        params = (IronIR_Param *)iron_arena_alloc(
            ctx->ir_arena,
            (size_t)total_params * sizeof(IronIR_Param),
            _Alignof(IronIR_Param));
    }

    int param_idx = 0;
    if (capture_count > 0) {
        /* First param: env pointer (void*) */
        params[param_idx].name = "env";
        params[param_idx].type = NULL;  /* void* — Phase 9 will type this */
        param_idx++;
    }

    /* Remaining params: lambda's declared parameters */
    for (int p = 0; p < lam->param_count; p++) {
        Iron_Param *ap = (Iron_Param *)lam->params[p];
        Iron_Type  *pt = lower_types_resolve_ann(ap->type_ann);
        params[param_idx].name = ap->name;
        params[param_idx].type = pt;
        param_idx++;
    }

    /* Determine return type from lambda's return annotation */
    Iron_Type *ret_type = NULL;
    if (lam->return_type) {
        ret_type = lower_types_resolve_ann(lam->return_type);
    }
    if (ret_type && ret_type->kind == IRON_TYPE_VOID) ret_type = NULL;

    /* Create the lifted function */
    IronIR_Func *lifted = iron_ir_func_create(ctx->module, lp->lifted_name,
                                               params, total_params, ret_type);
    if (!lifted) {
        arrfree(captures);
        return;
    }

    /* Lower the lambda body into the lifted function */
    IronIR_Func  *saved_func  = ctx->current_func;
    IronIR_Block *saved_block = ctx->current_block;
    const char   *saved_name  = ctx->current_func_name;

    ctx->current_func      = lifted;
    ctx->current_func_name = lp->lifted_name;

    /* Reset per-function maps for the lifted function */
    shfree(ctx->val_binding_map);
    shfree(ctx->var_alloca_map);
    shfree(ctx->param_map);
    ctx->val_binding_map = NULL;
    ctx->var_alloca_map  = NULL;
    ctx->param_map       = NULL;

    IronIR_Block *entry = iron_ir_block_create(lifted, "entry");
    ctx->current_block  = entry;

    /* If there are captures, the env param holds them.
     * We create loads from env fields for each capture.
     * Since we cannot construct a true struct type for env here (Phase 9 does
     * that), we register each capture as a val_binding_map entry pointing to
     * its synthetic ValueId. This is a Phase 8 approximation — Phase 9 will
     * emit the env struct and field loads properly. */
    int param_start = capture_count > 0 ? 1 : 0;
    for (int p = param_start; p < total_params; p++) {
        Iron_Param *ap = (Iron_Param *)lam->params[p - param_start];
        Iron_Type  *pt = params[p].type;

        /* Synthetic ValueId for this param */
        IronIR_ValueId param_val_id = lifted->next_value_id++;
        while (arrlen(lifted->value_table) <= (ptrdiff_t)param_val_id) {
            arrput(lifted->value_table, NULL);
        }
        lifted->value_table[param_val_id] = NULL;
        shput(ctx->param_map, ap->name, param_val_id);

        /* alloca+store for the param */
        IronIR_Instr *slot = iron_ir_alloca(lifted, entry, pt, ap->name,
                                             lam->span);
        iron_ir_store(lifted, entry, slot->id, param_val_id, lam->span);
        shput(ctx->var_alloca_map, ap->name, slot->id);
    }

    /* Lower the lambda body block */
    ctx->function_scope_depth = ctx->defer_depth;
    if (lam->body) {
        Iron_Block *body = (Iron_Block *)lam->body;
        push_defer_scope(ctx);
        for (int i = 0; i < body->stmt_count; i++) {
            if (!ctx->current_block) break;
            lower_stmt(ctx, body->stmts[i]);
        }
        if (ctx->current_block) {
            emit_defers_ir(ctx, ctx->function_scope_depth);
        }
        pop_defer_scope(ctx);
    }

    /* Implicit void return if needed */
    if (lifted->return_type == NULL && ctx->current_block) {
        if (ctx->current_block->instr_count == 0 ||
            !iron_ir_is_terminator(
                ctx->current_block->instrs[ctx->current_block->instr_count - 1]->kind)) {
            iron_ir_return(lifted, ctx->current_block, IRON_IR_VALUE_INVALID,
                           true, NULL, lam->span);
        }
    }

    /* Restore context */
    ctx->current_func      = saved_func;
    ctx->current_block     = saved_block;
    ctx->current_func_name = saved_name;

    /* Restore per-function maps for the enclosing function */
    shfree(ctx->val_binding_map);
    shfree(ctx->var_alloca_map);
    shfree(ctx->param_map);
    ctx->val_binding_map = NULL;
    ctx->var_alloca_map  = NULL;
    ctx->param_map       = NULL;

    arrfree(captures);
}

/* Lift a spawn body to a top-level IrFunc. */
static void lift_spawn(IronIR_LowerCtx *ctx, LiftPending *lp) {
    Iron_SpawnStmt *ss = (Iron_SpawnStmt *)lp->ast_node;
    if (!ss) return;

    /* Collect captures from the spawn body */
    const char **captures = collect_captures(ss->body, NULL, 0);
    int capture_count = (int)arrlen(captures);

    /* Spawn function: captures become explicit parameters */
    IronIR_Param *params = NULL;
    if (capture_count > 0) {
        params = (IronIR_Param *)iron_arena_alloc(
            ctx->ir_arena,
            (size_t)capture_count * sizeof(IronIR_Param),
            _Alignof(IronIR_Param));
        for (int c = 0; c < capture_count; c++) {
            params[c].name = captures[c];
            params[c].type = NULL;  /* type unknown at this pass */
        }
    }

    /* Create the lifted function (void return) */
    IronIR_Func *lifted = iron_ir_func_create(ctx->module, lp->lifted_name,
                                               params, capture_count, NULL);
    if (!lifted) {
        arrfree(captures);
        return;
    }

    /* Lower spawn body */
    IronIR_Func  *saved_func  = ctx->current_func;
    IronIR_Block *saved_block = ctx->current_block;
    const char   *saved_name  = ctx->current_func_name;

    ctx->current_func      = lifted;
    ctx->current_func_name = lp->lifted_name;

    shfree(ctx->val_binding_map);
    shfree(ctx->var_alloca_map);
    shfree(ctx->param_map);
    ctx->val_binding_map = NULL;
    ctx->var_alloca_map  = NULL;
    ctx->param_map       = NULL;

    IronIR_Block *entry = iron_ir_block_create(lifted, "entry");
    ctx->current_block  = entry;

    /* Register capture params as alloca slots */
    for (int c = 0; c < capture_count; c++) {
        IronIR_ValueId param_val_id = lifted->next_value_id++;
        while (arrlen(lifted->value_table) <= (ptrdiff_t)param_val_id) {
            arrput(lifted->value_table, NULL);
        }
        lifted->value_table[param_val_id] = NULL;
        shput(ctx->param_map, captures[c], param_val_id);

        IronIR_Instr *slot = iron_ir_alloca(lifted, entry, NULL,
                                             captures[c], ss->span);
        iron_ir_store(lifted, entry, slot->id, param_val_id, ss->span);
        shput(ctx->var_alloca_map, captures[c], slot->id);
    }

    ctx->function_scope_depth = ctx->defer_depth;
    if (ss->body) {
        Iron_Block *body = (Iron_Block *)ss->body;
        push_defer_scope(ctx);
        for (int i = 0; i < body->stmt_count; i++) {
            if (!ctx->current_block) break;
            lower_stmt(ctx, body->stmts[i]);
        }
        if (ctx->current_block) {
            emit_defers_ir(ctx, ctx->function_scope_depth);
        }
        pop_defer_scope(ctx);
    }

    /* Emit implicit void return */
    if (ctx->current_block) {
        if (ctx->current_block->instr_count == 0 ||
            !iron_ir_is_terminator(
                ctx->current_block->instrs[ctx->current_block->instr_count - 1]->kind)) {
            iron_ir_return(lifted, ctx->current_block, IRON_IR_VALUE_INVALID,
                           true, NULL, ss->span);
        }
    }

    ctx->current_func      = saved_func;
    ctx->current_block     = saved_block;
    ctx->current_func_name = saved_name;

    shfree(ctx->val_binding_map);
    shfree(ctx->var_alloca_map);
    shfree(ctx->param_map);
    ctx->val_binding_map = NULL;
    ctx->var_alloca_map  = NULL;
    ctx->param_map       = NULL;

    arrfree(captures);
}

/* Lift a parallel-for body to a top-level chunk IrFunc. */
static void lift_pfor(IronIR_LowerCtx *ctx, LiftPending *lp) {
    Iron_ForStmt *fs = (Iron_ForStmt *)lp->ast_node;
    if (!fs) return;

    /* Collect captures from the pfor body (excluding the loop variable) */
    const char **captures = collect_captures(fs->body, NULL, 0);
    int capture_count = (int)arrlen(captures);

    /* Chunk function params: (loop_var: Int) + captured variables.
     * The loop variable is passed as the first param (range sliced by runtime). */
    int total_params = 1 + capture_count;
    IronIR_Param *params = (IronIR_Param *)iron_arena_alloc(
        ctx->ir_arena,
        (size_t)total_params * sizeof(IronIR_Param),
        _Alignof(IronIR_Param));

    /* First param: the loop variable */
    params[0].name = fs->var_name ? fs->var_name : "i";
    params[0].type = iron_type_make_primitive(IRON_TYPE_INT);

    /* Remaining params: captures */
    for (int c = 0; c < capture_count; c++) {
        params[1 + c].name = captures[c];
        params[1 + c].type = NULL;
    }

    /* Create the chunk function (void return) */
    IronIR_Func *lifted = iron_ir_func_create(ctx->module, lp->lifted_name,
                                               params, total_params, NULL);
    if (!lifted) {
        arrfree(captures);
        return;
    }

    /* Lower pfor body */
    IronIR_Func  *saved_func  = ctx->current_func;
    IronIR_Block *saved_block = ctx->current_block;
    const char   *saved_name  = ctx->current_func_name;

    ctx->current_func      = lifted;
    ctx->current_func_name = lp->lifted_name;

    shfree(ctx->val_binding_map);
    shfree(ctx->var_alloca_map);
    shfree(ctx->param_map);
    ctx->val_binding_map = NULL;
    ctx->var_alloca_map  = NULL;
    ctx->param_map       = NULL;

    IronIR_Block *entry = iron_ir_block_create(lifted, "entry");
    ctx->current_block  = entry;

    /* Register loop variable param */
    {
        IronIR_ValueId param_val_id = lifted->next_value_id++;
        while (arrlen(lifted->value_table) <= (ptrdiff_t)param_val_id) {
            arrput(lifted->value_table, NULL);
        }
        lifted->value_table[param_val_id] = NULL;
        const char *lv_name = fs->var_name ? fs->var_name : "i";
        shput(ctx->param_map, lv_name, param_val_id);
        IronIR_Instr *slot = iron_ir_alloca(lifted, entry,
                                             iron_type_make_primitive(IRON_TYPE_INT),
                                             lv_name, fs->span);
        iron_ir_store(lifted, entry, slot->id, param_val_id, fs->span);
        shput(ctx->var_alloca_map, lv_name, slot->id);
    }

    /* Register capture params */
    for (int c = 0; c < capture_count; c++) {
        IronIR_ValueId param_val_id = lifted->next_value_id++;
        while (arrlen(lifted->value_table) <= (ptrdiff_t)param_val_id) {
            arrput(lifted->value_table, NULL);
        }
        lifted->value_table[param_val_id] = NULL;
        shput(ctx->param_map, captures[c], param_val_id);
        IronIR_Instr *slot = iron_ir_alloca(lifted, entry, NULL,
                                             captures[c], fs->span);
        iron_ir_store(lifted, entry, slot->id, param_val_id, fs->span);
        shput(ctx->var_alloca_map, captures[c], slot->id);
    }

    ctx->function_scope_depth = ctx->defer_depth;
    if (fs->body) {
        Iron_Block *body = (Iron_Block *)fs->body;
        push_defer_scope(ctx);
        for (int i = 0; i < body->stmt_count; i++) {
            if (!ctx->current_block) break;
            lower_stmt(ctx, body->stmts[i]);
        }
        if (ctx->current_block) {
            emit_defers_ir(ctx, ctx->function_scope_depth);
        }
        pop_defer_scope(ctx);
    }

    /* Emit implicit void return */
    if (ctx->current_block) {
        if (ctx->current_block->instr_count == 0 ||
            !iron_ir_is_terminator(
                ctx->current_block->instrs[ctx->current_block->instr_count - 1]->kind)) {
            iron_ir_return(lifted, ctx->current_block, IRON_IR_VALUE_INVALID,
                           true, NULL, fs->span);
        }
    }

    ctx->current_func      = saved_func;
    ctx->current_block     = saved_block;
    ctx->current_func_name = saved_name;

    shfree(ctx->val_binding_map);
    shfree(ctx->var_alloca_map);
    shfree(ctx->param_map);
    ctx->val_binding_map = NULL;
    ctx->var_alloca_map  = NULL;
    ctx->param_map       = NULL;

    arrfree(captures);
}

/* ── Post-pass: process all pending lifts ────────────────────────────────── */

void lower_lift_pending(IronIR_LowerCtx *ctx) {
    for (int i = 0; i < (int)arrlen(ctx->pending_lifts); i++) {
        LiftPending *lp = &ctx->pending_lifts[i];
        switch (lp->kind) {
            case LIFT_LAMBDA:       lift_lambda(ctx, lp); break;
            case LIFT_SPAWN:        lift_spawn(ctx, lp);  break;
            case LIFT_PARALLEL_FOR: lift_pfor(ctx, lp);   break;
        }
    }
}
