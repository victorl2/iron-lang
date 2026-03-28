/* emit_c.c — IR-to-C emission backend for the Iron compiler.
 *
 * Implements iron_ir_emit_c():
 *   1. Phi elimination pre-pass (phi -> alloca+store+load)
 *   2. Include directives
 *   3. Forward declarations (typedef struct Iron_Foo Iron_Foo;)
 *   4. Struct bodies (topologically sorted) + interface vtables
 *   5. Enum definitions
 *   6. Global constants (none in IR path — handled during lowering)
 *   7. Function prototypes
 *   8. Lifted function bodies (lambda_, spawn_, parallel_)
 *   9. Function implementations
 *  10. main() wrapper
 */

#include "ir/emit_c.h"
#include "ir/ir.h"
#include "util/strbuf.h"
#include "util/arena.h"
#include "parser/ast.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>

/* ── EmitCtx ──────────────────────────────────────────────────────────────── */

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;
    IronIR_Module *module;

    /* Output sections — emitted in order */
    Iron_StrBuf    includes;
    Iron_StrBuf    forward_decls;
    Iron_StrBuf    struct_bodies;
    Iron_StrBuf    enum_defs;
    Iron_StrBuf    global_consts;
    Iron_StrBuf    prototypes;
    Iron_StrBuf    lifted_funcs;
    Iron_StrBuf    implementations;
    Iron_StrBuf    main_wrapper;

    /* State */
    char        **emitted_optionals;             /* stb_ds string array */
    struct { char *key; bool value; } *mono_registry; /* stb_ds string map */
    int           next_type_tag;                 /* starts at 1 */
    int           indent;

    /* Stack-array tracking: maps ValueId -> original ARRAY_LIT ValueId.
     * Propagated through alloca/store/load chains so GET_INDEX/SET_INDEX/GET_FIELD
     * can emit direct C array access instead of Iron_List_T function calls.
     * A value of 0 (IRON_IR_VALUE_INVALID) means not a stack array. */
    struct { IronIR_ValueId key; IronIR_ValueId value; } *stack_array_ids;  /* stb_ds hashmap */

    /* Heap-array lifecycle tracking (COLL-04): maps ValueId -> original
     * ARRAY_LIT ValueId for heap-allocated lists (use_stack_repr == false).
     * Propagated through alloca/store/load chains.  Used to emit _free()
     * calls before RETURN for non-escaping heap arrays. */
    struct { IronIR_ValueId key; IronIR_ValueId value; } *heap_array_ids;   /* stb_ds hashmap */

    /* Set of heap-array ARRAY_LIT ValueIds that escape the function (via
     * RETURN, SET_FIELD, CONSTRUCT field, CALL arg, or MAKE_CLOSURE capture).
     * These must NOT be freed before return. */
    struct { IronIR_ValueId key; bool value; } *escaped_heap_ids;           /* stb_ds hashmap */

    /* Array parameter passing modes (PARAM-01/PARAM-02):
     * Maps "func_name\tparam_index" -> ArrayParamMode (as int).
     * Determined by analyze_array_param_modes() before emission begins. */
    struct { char *key; int value; } *array_param_modes;                    /* stb_ds string map */

    /* Revoked fill() stack-array candidates: ValueIds of __builtin_fill calls
     * that were initially eligible for stack allocation but were revoked
     * because they escape the function. */
    struct { IronIR_ValueId key; bool value; } *revoked_fill_ids;           /* stb_ds hashmap */

    /* Read-only parameter alias tracking: maps alloca ValueId -> param ValueId.
     * For parameters that are never modified (only read), we skip the
     * alloca+store+load chain and reference the parameter value directly. */
    struct { IronIR_ValueId key; IronIR_ValueId value; } *param_alias_ids;  /* stb_ds hashmap */
} EmitCtx;

/* ── Array parameter passing mode (PARAM-01/PARAM-02) ────────────────────── */

typedef enum {
    ARRAY_PARAM_LIST,      /* keep as Iron_List_T (default, safe fallback) */
    ARRAY_PARAM_CONST_PTR, /* const T* + len (read-only parameter) */
    ARRAY_PARAM_MUT_PTR    /* T* + len (mutable, no resize) */
} ArrayParamMode;

/* ── Name mangling helpers ────────────────────────────────────────────────── */

static const char *emit_mangle_name(const char *name, Iron_Arena *arena) {
    size_t len   = strlen(name);
    size_t total = 5 + len + 1;
    char  *buf   = (char *)iron_arena_alloc(arena, total, 1);
    memcpy(buf, "Iron_", 5);
    memcpy(buf + 5, name, len + 1);
    return buf;
}

/* Map an Iron function name to the C symbol name.
 * - Lifted functions (lambda_, spawn_, parallel_) are kept as-is.
 * - Built-in function names (println, print, len, etc.) map to Iron_XXX.
 * - All other user-defined functions map to Iron_XXX.
 * The returned string is arena-allocated or a static literal. */
static const char *mangle_func_name(const char *name, Iron_Arena *arena) {
    if (!name) return "NULL";

    /* Lifted functions are already internal C identifiers — keep as-is.
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

    /* All other names: apply Iron_ prefix */
    return emit_mangle_name(name, arena);
}

/* Resolve a function IR name to its C symbol, honoring extern_c_name.
 * Looks up the function in the module; if found and is_extern, uses extern_c_name.
 * Otherwise falls back to mangle_func_name(). */
static const char *resolve_func_c_name(EmitCtx *ctx, const char *ir_name) {
    if (!ir_name) return "NULL";
    for (int fi = 0; fi < ctx->module->func_count; fi++) {
        IronIR_Func *f = ctx->module->funcs[fi];
        if (strcmp(f->name, ir_name) == 0 && f->is_extern) {
            return f->extern_c_name ? f->extern_c_name : ir_name;
        }
    }
    return mangle_func_name(ir_name, ctx->arena);
}

/* Sanitize a block label for use as a C identifier: replace dots with underscores. */
static const char *sanitize_label(const char *label, Iron_Arena *arena) {
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
    for (size_t i = 0; i <= len; i++) {
        buf[i] = (label[i] == '.') ? '_' : label[i];
    }
    return buf;
}

/* ── Type-to-C mapping (no Iron_Codegen dependency) ───────────────────────── */

/* Forward declaration for mutual recursion */
static const char *emit_type_to_c(const Iron_Type *t, EmitCtx *ctx);

static const char *emit_optional_struct_name(const Iron_Type *inner,
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
    iron_strbuf_free(&sb);
    return result;
}

static void emit_ensure_optional(EmitCtx *ctx, const Iron_Type *inner);

static const char *emit_type_to_c(const Iron_Type *t, EmitCtx *ctx) {
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
            return emit_mangle_name(t->object.decl->name, ctx->arena);

        case IRON_TYPE_ENUM:
            return emit_mangle_name(t->enu.decl->name, ctx->arena);

        case IRON_TYPE_INTERFACE:
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
            iron_strbuf_free(&sb);
            return result;
        }

        case IRON_TYPE_FUNC:
            return "void*";

        case IRON_TYPE_ARRAY: {
            /* Arrays are represented as Iron_List_<elem_c_type> in C.
             * e.g. [Int] -> Iron_List_int64_t, [Float] -> Iron_List_double */
            const char *elem_c = emit_type_to_c(t->array.elem, ctx);
            Iron_StrBuf sb = iron_strbuf_create(64);
            iron_strbuf_appendf(&sb, "Iron_List_");
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
            iron_strbuf_free(&sb);
            return result;
        }

        case IRON_TYPE_GENERIC_PARAM:
            return "void*";
    }
    return "int"; /* unreachable fallback */
}

static void emit_ensure_optional(EmitCtx *ctx, const Iron_Type *inner) {
    const char *struct_name = emit_optional_struct_name(inner, ctx);

    for (int i = 0; i < (int)arrlen(ctx->emitted_optionals); i++) {
        if (strcmp(ctx->emitted_optionals[i], struct_name) == 0) return;
    }

    arrput(ctx->emitted_optionals,
           iron_arena_strdup(ctx->arena, struct_name, strlen(struct_name)));

    const char *c_inner = emit_type_to_c(inner, ctx);
    iron_strbuf_appendf(&ctx->struct_bodies,
                         "typedef struct { %s value; bool has_value; } %s;\n",
                         c_inner, struct_name);
}

/* ── Phi elimination pre-pass ────────────────────────────────────────────── */

/* Find a block within a function by ID */
static IronIR_Block *find_block(IronIR_Func *fn, IronIR_BlockId id) {
    for (int i = 0; i < fn->block_count; i++) {
        if (fn->blocks[i]->id == id) return fn->blocks[i];
    }
    return NULL;
}

/* Find index of the terminator instruction in a block */
static int find_terminator_idx(IronIR_Block *block) {
    for (int i = 0; i < block->instr_count; i++) {
        if (iron_ir_is_terminator(block->instrs[i]->kind)) return i;
    }
    return block->instr_count;  /* no terminator found — append at end */
}

/* Insert a pre-built instruction into a block before the terminator */
static void insert_store_before_terminator_instr(IronIR_Block *block,
                                                   IronIR_Instr *store) {
    int term_idx = find_terminator_idx(block);

    /* Shift all instructions from term_idx onward by one */
    arrput(block->instrs, NULL);  /* grow by 1 */
    block->instr_count++;
    for (int i = block->instr_count - 1; i > term_idx; i--) {
        block->instrs[i] = block->instrs[i - 1];
    }
    block->instrs[term_idx] = store;
}

/* Create an alloca instruction directly (without appending to any block).
 * This is used by phi_eliminate to create alloca instructions that will be
 * manually inserted into the entry block. */
static IronIR_Instr *make_alloca_instr(IronIR_Func *fn, Iron_Type *alloc_type,
                                        Iron_Span span) {
    IronIR_Instr *instr = ARENA_ALLOC(fn->arena, IronIR_Instr);
    memset(instr, 0, sizeof(*instr));
    instr->kind             = IRON_IR_ALLOCA;
    instr->type             = alloc_type;  /* alloca result "type" for load */
    instr->span             = span;
    instr->alloca.alloc_type = alloc_type;
    instr->alloca.name_hint  = NULL;

    /* Assign a value ID */
    instr->id = fn->next_value_id++;
    while (arrlen(fn->value_table) <= (ptrdiff_t)instr->id) {
        arrput(fn->value_table, NULL);
    }
    fn->value_table[instr->id] = instr;
    return instr;
}

/* Create a store instruction directly (without appending to any block). */
static IronIR_Instr *make_store_instr(IronIR_Func *fn, IronIR_ValueId ptr,
                                       IronIR_ValueId value, Iron_Span span) {
    IronIR_Instr *instr = ARENA_ALLOC(fn->arena, IronIR_Instr);
    memset(instr, 0, sizeof(*instr));
    instr->kind        = IRON_IR_STORE;
    instr->span        = span;
    instr->id          = IRON_IR_VALUE_INVALID;
    instr->store.ptr   = ptr;
    instr->store.value = value;
    return instr;
}

/* Phi elimination: walk all functions, replace phi nodes with alloca+store+load */
static void phi_eliminate(IronIR_Module *module) {
    for (int fi = 0; fi < module->func_count; fi++) {
        IronIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Collect all phi nodes first to avoid modifying while iterating */
        IronIR_Instr **phis = NULL;  /* stb_ds array of phi instrs */

        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_PHI) {
                    arrput(phis, instr);
                }
            }
        }

        /* Process each phi */
        for (int pi = 0; pi < (int)arrlen(phis); pi++) {
            IronIR_Instr *phi = phis[pi];
            Iron_Span span = phi->span;

            /* 1. Create an alloca instruction (without appending to a block) */
            IronIR_Instr *alloca_instr = make_alloca_instr(fn, phi->type, span);
            IronIR_ValueId alloca_id = alloca_instr->id;

            /* Insert alloca at the top of the entry block */
            IronIR_Block *entry = fn->blocks[0];
            arrput(entry->instrs, NULL);  /* grow by 1 */
            entry->instr_count++;
            for (int i = entry->instr_count - 1; i > 0; i--) {
                entry->instrs[i] = entry->instrs[i - 1];
            }
            entry->instrs[0] = alloca_instr;

            /* 2. In each predecessor block, insert store before terminator */
            for (int i = 0; i < phi->phi.count; i++) {
                IronIR_BlockId pred_id = phi->phi.pred_blocks[i];
                IronIR_ValueId val     = phi->phi.values[i];
                IronIR_Block  *pred    = find_block(fn, pred_id);
                if (!pred) continue;

                IronIR_Instr *store = make_store_instr(fn, alloca_id, val, span);
                insert_store_before_terminator_instr(pred, store);
            }

            /* 2b. Find the block containing this phi and ensure ALL its
             * predecessors have a store for the alloca.  Predecessors not
             * covered by the phi operands (e.g., elif branches with array
             * indexing that introduce intermediate ValueIds) get a default
             * store using the first phi value as a safe fallback. */
            {
                IronIR_Block *phi_block = NULL;
                for (int bi = 0; bi < fn->block_count; bi++) {
                    IronIR_Block *blk = fn->blocks[bi];
                    for (int ii = 0; ii < blk->instr_count; ii++) {
                        if (blk->instrs[ii] == phi) {
                            phi_block = blk;
                            break;
                        }
                    }
                    if (phi_block) break;
                }
                if (phi_block && phi->phi.count > 0) {
                    IronIR_ValueId default_val = phi->phi.values[0];
                    for (int pi2 = 0; pi2 < (int)arrlen(phi_block->preds); pi2++) {
                        IronIR_BlockId pred_id = phi_block->preds[pi2];
                        /* Check if this predecessor is already covered */
                        bool covered = false;
                        for (int k = 0; k < phi->phi.count; k++) {
                            if (phi->phi.pred_blocks[k] == pred_id) {
                                covered = true;
                                break;
                            }
                        }
                        if (!covered) {
                            IronIR_Block *pred = find_block(fn, pred_id);
                            if (pred) {
                                IronIR_Instr *store = make_store_instr(
                                    fn, alloca_id, default_val, span);
                                insert_store_before_terminator_instr(pred, store);
                            }
                        }
                    }
                }
            }

            /* 3. Replace phi with a LOAD from the alloca */
            phi->kind     = IRON_IR_LOAD;
            phi->load.ptr = alloca_id;
            /* phi->id and phi->type remain the same — the load produces the
             * same ValueId so all existing uses are automatically valid */
        }

        arrfree(phis);
    }
}

/* ── Array parameter mode helpers (PARAM-01/PARAM-02) ────────────────────── */

/* Build a key for the array_param_modes map: "func_name\tparam_index" */
static const char *make_param_mode_key(const char *func_name, int param_index,
                                        Iron_Arena *arena) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", param_index);
    size_t fn_len = strlen(func_name);
    size_t idx_len = strlen(buf);
    size_t total = fn_len + 1 + idx_len + 1;
    char *key = (char *)iron_arena_alloc(arena, total, 1);
    memcpy(key, func_name, fn_len);
    key[fn_len] = '\t';
    memcpy(key + fn_len + 1, buf, idx_len + 1);
    return key;
}

/* Look up the ArrayParamMode for a given function + param index. */
static ArrayParamMode get_array_param_mode(EmitCtx *ctx, const char *func_name,
                                            int param_index) {
    const char *key = make_param_mode_key(func_name, param_index, ctx->arena);
    ptrdiff_t idx = shgeti(ctx->array_param_modes, key);
    if (idx >= 0) return (ArrayParamMode)ctx->array_param_modes[idx].value;
    return ARRAY_PARAM_LIST;
}

/* Find an IronIR_Func in the module by IR name. */
static IronIR_Func *find_ir_func(EmitCtx *ctx, const char *ir_name) {
    if (!ir_name) return NULL;
    for (int i = 0; i < ctx->module->func_count; i++) {
        if (strcmp(ctx->module->funcs[i]->name, ir_name) == 0)
            return ctx->module->funcs[i];
    }
    return NULL;
}

/* Analyze all functions and determine which array parameters can be passed
 * as pointer+length instead of Iron_List_T.
 *
 * A parameter qualifies when it is ONLY used for:
 *   - GET_INDEX (read access)
 *   - SET_INDEX (write access, mutable ptr)
 *   - GET_FIELD .count (len() builtin)
 *   - STORE of param value into its own alloca (entry-block pattern)
 *
 * Disqualified when:
 *   - Loaded alias is stored into another alloca (var a = arr pattern)
 *   - Alias passed as CALL argument
 *   - Alias used in RETURN, SET_FIELD, CONSTRUCT, MAKE_CLOSURE, SLICE
 *   - Alloca is reassigned with a non-alias value */
static void analyze_array_param_modes(EmitCtx *ctx) {
    IronIR_Module *module = ctx->module;

    for (int fi = 0; fi < module->func_count; fi++) {
        IronIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        for (int pi = 0; pi < fn->param_count; pi++) {
            Iron_Type *pt = fn->params[pi].type;
            if (!pt || pt->kind != IRON_TYPE_ARRAY) continue;

            IronIR_ValueId param_val_id = (IronIR_ValueId)(pi * 2 + 1);
            IronIR_ValueId alloca_id    = (IronIR_ValueId)(pi * 2 + 2);

            /* Build alias set: param_val and alloca, plus LOADs from alloca */
            struct { IronIR_ValueId key; bool value; } *aliases = NULL;
            hmput(aliases, param_val_id, true);
            hmput(aliases, alloca_id, true);

            for (int bi = 0; bi < fn->block_count; bi++) {
                IronIR_Block *block = fn->blocks[bi];
                for (int ii = 0; ii < block->instr_count; ii++) {
                    IronIR_Instr *instr = block->instrs[ii];
                    if (instr->kind == IRON_IR_LOAD) {
                        if (hmgeti(aliases, instr->load.ptr) >= 0) {
                            hmput(aliases, instr->id, true);
                        }
                    }
                }
            }

            /* Scan for disqualifying uses */
            bool has_write = false;
            bool disqualified = false;

            for (int bi = 0; bi < fn->block_count && !disqualified; bi++) {
                IronIR_Block *block = fn->blocks[bi];
                for (int ii = 0; ii < block->instr_count && !disqualified; ii++) {
                    IronIR_Instr *instr = block->instrs[ii];

                    switch (instr->kind) {
                    case IRON_IR_GET_INDEX:
                        break;
                    case IRON_IR_SET_INDEX:
                        if (hmgeti(aliases, instr->index.array) >= 0)
                            has_write = true;
                        break;
                    case IRON_IR_GET_FIELD:
                        break;
                    case IRON_IR_STORE:
                        /* store(param_alloca, param_val) is the entry-block pattern - ok.
                         * store(alias, non-alias) = reassignment - disqualify.
                         * store(non-alias, alias) = copying param to another var - disqualify. */
                        if (hmgeti(aliases, instr->store.ptr) >= 0 &&
                            hmgeti(aliases, instr->store.value) < 0) {
                            disqualified = true;
                        }
                        if (hmgeti(aliases, instr->store.ptr) < 0 &&
                            hmgeti(aliases, instr->store.value) >= 0) {
                            disqualified = true;
                        }
                        break;
                    case IRON_IR_CALL:
                        for (int ai = 0; ai < instr->call.arg_count; ai++) {
                            if (hmgeti(aliases, instr->call.args[ai]) >= 0)
                                disqualified = true;
                        }
                        break;
                    case IRON_IR_RETURN:
                        if (!instr->ret.is_void &&
                            hmgeti(aliases, instr->ret.value) >= 0)
                            disqualified = true;
                        break;
                    case IRON_IR_SET_FIELD:
                        if (hmgeti(aliases, instr->field.value) >= 0)
                            disqualified = true;
                        break;
                    case IRON_IR_CONSTRUCT:
                        for (int fj = 0; fj < instr->construct.field_count; fj++) {
                            if (hmgeti(aliases, instr->construct.field_vals[fj]) >= 0)
                                disqualified = true;
                        }
                        break;
                    case IRON_IR_MAKE_CLOSURE:
                        for (int ci = 0; ci < instr->make_closure.capture_count; ci++) {
                            if (hmgeti(aliases, instr->make_closure.captures[ci]) >= 0)
                                disqualified = true;
                        }
                        break;
                    case IRON_IR_SLICE:
                        if (hmgeti(aliases, instr->slice.array) >= 0)
                            disqualified = true;
                        break;
                    default:
                        break;
                    }
                }
            }

            hmfree(aliases);

            if (!disqualified) {
                ArrayParamMode mode = has_write
                    ? ARRAY_PARAM_MUT_PTR : ARRAY_PARAM_CONST_PTR;
                const char *key = make_param_mode_key(fn->name, pi, ctx->arena);
                shput(ctx->array_param_modes, key, (int)mode);
            }
        }
    }
}

/* ── Stack-array optimization pre-pass (ARR-01) ─────────────────────────── */

/* Mark ARRAY_LIT instructions with known element counts <= 256 as
 * stack-array eligible.  This runs after phi elimination so the IR is stable. */
static void optimize_array_repr(IronIR_Module *module, EmitCtx *ctx) {
    for (int fi = 0; fi < module->func_count; fi++) {
        IronIR_Func *fn = module->funcs[fi];
        if (fn->is_extern || fn->block_count == 0) continue;

        /* Pass 1: Mark array literals as stack-eligible */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_ARRAY_LIT) {
                    if (instr->array_lit.element_count > 0 &&
                        instr->array_lit.element_count <= 256) {
                        instr->array_lit.use_stack_repr = true;
                    }
                }
            }
        }

        /* Pass 2: Check if any stack array escapes the function via RETURN.
         * If an array value (directly or through alloca/load chain) reaches a
         * RETURN instruction, revoke its stack eligibility since the stack
         * memory would be invalid after the function returns.
         *
         * We build a set of stack-array ValueIds and alloca ValueIds that
         * hold stack arrays, then check if any RETURN references them. */
        /* For simplicity, we track: STORE(alloca, stack_array) -> alloca is "tainted".
         * LOAD(alloca) -> loaded value is "tainted".
         * RETURN(tainted_value) -> revoke the original array_lit. */
        struct { IronIR_ValueId key; IronIR_ValueId value; } *sa_map = NULL;
        /* Mark all stack array lits and ALL fill() calls (constant or dynamic count) */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_ARRAY_LIT && instr->array_lit.use_stack_repr) {
                    hmput(sa_map, instr->id, instr->id);
                }
                /* __builtin_fill — all calls (constant or dynamic count) */
                if (instr->kind == IRON_IR_CALL && instr->call.arg_count == 2 &&
                    instr->type && instr->type->kind == IRON_TYPE_ARRAY) {
                    IronIR_ValueId fptr = instr->call.func_ptr;
                    if (fptr != IRON_IR_VALUE_INVALID &&
                        fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                        fn->value_table[fptr] != NULL &&
                        fn->value_table[fptr]->kind == IRON_IR_FUNC_REF &&
                        strcmp(fn->value_table[fptr]->func_ref.func_name,
                               "__builtin_fill") == 0) {
                        hmput(sa_map, instr->id, instr->id);
                    }
                }
            }
        }
        /* Propagate through store/load */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_STORE) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->store.value);
                    if (vi >= 0) hmput(sa_map, instr->store.ptr, sa_map[vi].value);
                } else if (instr->kind == IRON_IR_LOAD) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->load.ptr);
                    if (vi >= 0) hmput(sa_map, instr->id, sa_map[vi].value);
                }
            }
        }
        /* Check returns and function call arguments for escapes */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_RETURN && !instr->ret.is_void) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->ret.value);
                    if (vi >= 0) {
                        IronIR_ValueId orig = sa_map[vi].value;
                        if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[orig]) {
                            if (fn->value_table[orig]->kind == IRON_IR_ARRAY_LIT)
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            else
                                hmput(ctx->revoked_fill_ids, orig, true);
                        }
                    }
                }
                /* Check if stack array is passed as argument to a function call.
                 * If the callee param uses pointer mode (PARAM-01/02), the stack
                 * array can be passed directly. Otherwise revoke stack repr. */
                if (instr->kind == IRON_IR_CALL) {
                    /* Resolve callee IR name for pointer-mode check */
                    const char *call_ir_name = NULL;
                    if (instr->call.func_decl && !instr->call.func_decl->is_extern) {
                        call_ir_name = instr->call.func_decl->name;
                    } else if (!instr->call.func_decl) {
                        IronIR_ValueId fptr = instr->call.func_ptr;
                        if (fptr != IRON_IR_VALUE_INVALID &&
                            fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[fptr] != NULL &&
                            fn->value_table[fptr]->kind == IRON_IR_FUNC_REF) {
                            const char *rn = fn->value_table[fptr]->func_ref.func_name;
                            IronIR_Func *cf = find_ir_func(ctx, rn);
                            if (cf && !cf->is_extern) call_ir_name = rn;
                        }
                    }
                    for (int ai = 0; ai < instr->call.arg_count; ai++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->call.args[ai]);
                        if (vi >= 0) {
                            /* Check if callee accepts pointer mode for this param */
                            ArrayParamMode cpmode = ARRAY_PARAM_LIST;
                            if (call_ir_name)
                                cpmode = get_array_param_mode(ctx, call_ir_name, ai);
                            if (cpmode == ARRAY_PARAM_CONST_PTR ||
                                cpmode == ARRAY_PARAM_MUT_PTR)
                                continue; /* callee accepts pointer+len */
                            IronIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                if (fn->value_table[orig]->kind == IRON_IR_ARRAY_LIT)
                                    fn->value_table[orig]->array_lit.use_stack_repr = false;
                                else
                                    hmput(ctx->revoked_fill_ids, orig, true);
                            }
                        }
                    }
                }
                /* Check if stack array is used in SET_FIELD (stored into object) */
                if (instr->kind == IRON_IR_SET_FIELD) {
                    ptrdiff_t vi = hmgeti(sa_map, instr->field.value);
                    if (vi >= 0) {
                        IronIR_ValueId orig = sa_map[vi].value;
                        if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                            fn->value_table[orig]) {
                            if (fn->value_table[orig]->kind == IRON_IR_ARRAY_LIT)
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            else
                                hmput(ctx->revoked_fill_ids, orig, true);
                        }
                    }
                }
                /* Check if stack array is used as field in CONSTRUCT (stored into struct) */
                if (instr->kind == IRON_IR_CONSTRUCT) {
                    for (int fi2 = 0; fi2 < instr->construct.field_count; fi2++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->construct.field_vals[fi2]);
                        if (vi >= 0) {
                            IronIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                if (fn->value_table[orig]->kind == IRON_IR_ARRAY_LIT)
                                    fn->value_table[orig]->array_lit.use_stack_repr = false;
                                else
                                    hmput(ctx->revoked_fill_ids, orig, true);
                            }
                        }
                    }
                }
                /* Check if stack array is used in MAKE_CLOSURE captures */
                if (instr->kind == IRON_IR_MAKE_CLOSURE) {
                    for (int ci = 0; ci < instr->make_closure.capture_count; ci++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->make_closure.captures[ci]);
                        if (vi >= 0) {
                            IronIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            }
                        }
                    }
                }
                /* Check if stack array is used in INTERP_STRING */
                if (instr->kind == IRON_IR_INTERP_STRING) {
                    for (int pi = 0; pi < instr->interp_string.part_count; pi++) {
                        ptrdiff_t vi = hmgeti(sa_map, instr->interp_string.parts[pi]);
                        if (vi >= 0) {
                            IronIR_ValueId orig = sa_map[vi].value;
                            if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                                fn->value_table[orig]) {
                                fn->value_table[orig]->array_lit.use_stack_repr = false;
                            }
                        }
                    }
                }
            }
        }
        hmfree(sa_map);
    }
}

/* ── Block label resolution ───────────────────────────────────────────────── */

/* Build a unique C label for a block: "<sanitized_label>_b<id>".
 * This avoids duplicate-label errors when nested control flow reuses
 * the same label string (e.g., multiple "if_merge" blocks in one function). */
static const char *make_block_label(IronIR_BlockId id, const char *raw_label,
                                     Iron_Arena *arena) {
    /* Sanitize dots first */
    const char *san = sanitize_label(raw_label, arena);
    /* Allocate "label_b<id>\0" */
    size_t san_len = strlen(san);
    /* Max digits for a 32-bit int = 10 + "b" prefix + "_" + NUL = 14 extra */
    char *buf = (char *)iron_arena_alloc(arena, san_len + 16, 1);
    snprintf(buf, san_len + 16, "%s_b%d", san, (int)id);
    return buf;
}

static const char *resolve_label(IronIR_Func *fn, IronIR_BlockId id,
                                  Iron_Arena *arena) {
    for (int i = 0; i < fn->block_count; i++) {
        if (fn->blocks[i]->id == id) {
            return make_block_label(id, fn->blocks[i]->label, arena);
        }
    }
    return "unknown_block";
}

/* ── Instruction emission ─────────────────────────────────────────────────── */

static void emit_indent(Iron_StrBuf *sb, int level) {
    for (int i = 0; i < level * 4; i++) {
        iron_strbuf_append(sb, " ", 1);
    }
}

/* Emit the C name for a value: _v{id} */
static void emit_val(Iron_StrBuf *sb, IronIR_ValueId id) {
    iron_strbuf_appendf(sb, "_v%u", id);
}

/* Determine whether the target object type should use -> (pointer) or . */
static bool type_is_pointer(const Iron_Type *t) {
    if (!t) return false;
    if (t->kind == IRON_TYPE_RC) return true;
    if (t->kind == IRON_TYPE_NULLABLE) {
        /* nullable pointers */
        return false;
    }
    return false;
}

/* ── Stack-array tracking helpers ────────────────────────────────────────── */

/* Check if a ValueId is known to be a stack-represented array.
 * Returns the original ARRAY_LIT ValueId, or 0 if not a stack array. */
static IronIR_ValueId get_stack_array_origin(EmitCtx *ctx, IronIR_ValueId id) {
    if (!ctx->stack_array_ids) return IRON_IR_VALUE_INVALID;
    ptrdiff_t idx = hmgeti(ctx->stack_array_ids, id);
    if (idx >= 0) return ctx->stack_array_ids[idx].value;
    return IRON_IR_VALUE_INVALID;
}

/* Register a ValueId as a stack array, with the given origin ARRAY_LIT id. */
static void mark_stack_array(EmitCtx *ctx, IronIR_ValueId id,
                              IronIR_ValueId origin) {
    hmput(ctx->stack_array_ids, id, origin);
}

/* (resolve_stack_array_origin removed — pre-scan in emit_func_body handles propagation) */

static void emit_instr(Iron_StrBuf *sb, IronIR_Instr *instr,
                        IronIR_Func *fn, EmitCtx *ctx) {
    int ind = ctx->indent;

    switch (instr->kind) {

    /* ── Constants ──────────────────────────────────────────────────────── */

    case IRON_IR_CONST_INT:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (%s)%lldLL;\n",
                            emit_type_to_c(instr->type, ctx),
                            (long long)instr->const_int.value);
        break;

    case IRON_IR_CONST_FLOAT:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = %g;\n", instr->const_float.value);
        break;

    case IRON_IR_CONST_BOOL:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = %s;\n",
                            instr->const_bool.value ? "true" : "false");
        break;

    case IRON_IR_CONST_STRING: {
        const char *sv = instr->const_str.value ? instr->const_str.value : "";
        size_t slen = strlen(sv);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "Iron_String ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = iron_string_from_literal(\"%s\", %zu);\n",
                            sv, slen);
        break;
    }

    case IRON_IR_CONST_NULL:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "void* ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = NULL;\n");
        break;

    /* ── Arithmetic ─────────────────────────────────────────────────────── */

    case IRON_IR_ADD:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " + ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_SUB:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " - ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_MUL:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " * ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_DIV:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " / ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_MOD:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " %% ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Comparison ─────────────────────────────────────────────────────── */

    case IRON_IR_EQ:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " == ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_NEQ:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " != ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_LT:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " < ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_LTE:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " <= ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_GT:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " > ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_GTE:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " >= ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Logical ────────────────────────────────────────────────────────── */

    case IRON_IR_AND:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " && ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_OR:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->binop.left);
        iron_strbuf_appendf(sb, " || ");
        emit_val(sb, instr->binop.right);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Unary ──────────────────────────────────────────────────────────── */

    case IRON_IR_NEG:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = -");
        emit_val(sb, instr->unop.operand);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_NOT:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = !");
        emit_val(sb, instr->unop.operand);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Memory ─────────────────────────────────────────────────────────── */

    case IRON_IR_ALLOCA: {
        /* Skip alloca for read-only parameter aliases — no variable needed */
        if (hmgeti(ctx->param_alias_ids, instr->id) >= 0) break;

        /* Check if this alloca holds a stack array (determined by pre-scan) */
        IronIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->id);
        if (sa_origin != IRON_IR_VALUE_INVALID) {
            /* Emit as pointer to element type so C array decays correctly */
            const char *elem_c = "int64_t"; /* fallback */
            if (instr->alloca.alloc_type &&
                instr->alloca.alloc_type->kind == IRON_TYPE_ARRAY &&
                instr->alloca.alloc_type->array.elem) {
                elem_c = emit_type_to_c(instr->alloca.alloc_type->array.elem, ctx);
            }
            /* PARAM-01: Check if origin is a const pointer param.
             * Origin pvid = pi*2+1, so pi = (pvid-1)/2. If the param
             * has CONST_PTR mode, emit const qualifier on the pointer. */
            bool is_const_origin = false;
            if (sa_origin != IRON_IR_VALUE_INVALID && (sa_origin & 1) &&
                sa_origin <= (IronIR_ValueId)(fn->param_count * 2)) {
                int pi_idx = (int)(sa_origin - 1) / 2;
                if (pi_idx < fn->param_count) {
                    ArrayParamMode pm = get_array_param_mode(ctx, fn->name, pi_idx);
                    if (pm == ARRAY_PARAM_CONST_PTR) is_const_origin = true;
                }
            }
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s%s *", is_const_origin ? "const " : "", elem_c);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, ";\n");
            /* Also emit companion length variable for this alloca */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "int64_t ");
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, "_len;\n");
        } else {
            /* Declare a C variable of the alloc_type */
            const char *c_type = emit_type_to_c(instr->alloca.alloc_type, ctx);
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, ";\n");
        }
        break;
    }

    case IRON_IR_LOAD: {
        /* Load from a read-only parameter alias: reference the param directly */
        {
            ptrdiff_t pa_idx = hmgeti(ctx->param_alias_ids, instr->load.ptr);
            if (pa_idx >= 0) {
                IronIR_ValueId param_val = ctx->param_alias_ids[pa_idx].value;
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, " = ");
                emit_val(sb, param_val);
                iron_strbuf_appendf(sb, ";\n");
                break;
            }
        }

        IronIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->load.ptr);
        if (sa_origin != IRON_IR_VALUE_INVALID) {
            /* Loading from a stack-array alloca: emit as pointer copy */
            const char *elem_c = "int64_t"; /* fallback */
            if (instr->type && instr->type->kind == IRON_TYPE_ARRAY &&
                instr->type->array.elem) {
                elem_c = emit_type_to_c(instr->type->array.elem, ctx);
            }
            /* PARAM-01: preserve const qualifier from origin */
            bool is_const_origin = false;
            if (sa_origin != IRON_IR_VALUE_INVALID && (sa_origin & 1) &&
                sa_origin <= (IronIR_ValueId)(fn->param_count * 2)) {
                int pi_idx = (int)(sa_origin - 1) / 2;
                if (pi_idx < fn->param_count) {
                    ArrayParamMode pm = get_array_param_mode(ctx, fn->name, pi_idx);
                    if (pm == ARRAY_PARAM_CONST_PTR) is_const_origin = true;
                }
            }
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s%s *", is_const_origin ? "const " : "", elem_c);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
            emit_val(sb, instr->load.ptr);
            iron_strbuf_appendf(sb, ";\n");
            /* Copy companion length */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "int64_t ");
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, "_len = ");
            emit_val(sb, instr->load.ptr);
            iron_strbuf_appendf(sb, "_len;\n");
            /* Propagate stack-array status to loaded value */
            mark_stack_array(ctx, instr->id, sa_origin);
        } else {
            /* Load from the alloca variable — just copy it */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
            emit_val(sb, instr->load.ptr);
            iron_strbuf_appendf(sb, ";\n");
        }
        break;
    }

    case IRON_IR_STORE: {
        /* Skip store into read-only parameter alias alloca */
        if (hmgeti(ctx->param_alias_ids, instr->store.ptr) >= 0) break;

        /* Check if we're storing a stack array into an alloca */
        IronIR_ValueId sa_ptr = get_stack_array_origin(ctx, instr->store.ptr);
        IronIR_ValueId sa_val = get_stack_array_origin(ctx, instr->store.value);
        if (sa_ptr != IRON_IR_VALUE_INVALID && sa_val != IRON_IR_VALUE_INVALID) {
            /* Store stack array pointer into alloca: ptr = value; ptr_len = value_len; */
            emit_indent(sb, ind);
            emit_val(sb, instr->store.ptr);
            iron_strbuf_appendf(sb, " = ");
            emit_val(sb, instr->store.value);
            iron_strbuf_appendf(sb, ";\n");
            /* Propagate companion length */
            emit_indent(sb, ind);
            emit_val(sb, instr->store.ptr);
            iron_strbuf_appendf(sb, "_len = ");
            emit_val(sb, sa_val);
            iron_strbuf_appendf(sb, "_len;\n");
        } else {
            /* Write into alloca variable: ptr = value */
            emit_indent(sb, ind);
            emit_val(sb, instr->store.ptr);
            iron_strbuf_appendf(sb, " = ");
            emit_val(sb, instr->store.value);
            iron_strbuf_appendf(sb, ";\n");
        }
        break;
    }

    /* ── Field / Index ──────────────────────────────────────────────────── */

    case IRON_IR_GET_FIELD: {
        /* Check if this is a .count access on a stack array (from len() builtin) */
        IronIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->field.object);
        if (sa_origin != IRON_IR_VALUE_INVALID &&
            instr->field.field && strcmp(instr->field.field, "count") == 0) {
            /* Emit: int64_t _vN = _vOBJ_len; (companion length variable) */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
            emit_val(sb, instr->field.object);
            iron_strbuf_appendf(sb, "_len;\n");
            break;
        }
        /* object.field or object->field for heap/rc pointers */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->field.object);
        /* Use -> when the object value comes from a heap or rc allocation
         * (those produce pointer types in the emitted C). */
        bool obj_is_ptr = false;
        if (instr->field.object < (IronIR_ValueId)arrlen(fn->value_table) &&
            fn->value_table[instr->field.object]) {
            IronIR_InstrKind k = fn->value_table[instr->field.object]->kind;
            obj_is_ptr = (k == IRON_IR_HEAP_ALLOC || k == IRON_IR_RC_ALLOC);
        }
        iron_strbuf_appendf(sb, "%s%s;\n", obj_is_ptr ? "->" : ".", instr->field.field);
        break;
    }

    case IRON_IR_SET_FIELD: {
        /* object.field = value or object->field = value for heap/rc */
        emit_indent(sb, ind);
        emit_val(sb, instr->field.object);
        bool obj_is_ptr = false;
        if (instr->field.object < (IronIR_ValueId)arrlen(fn->value_table) &&
            fn->value_table[instr->field.object]) {
            IronIR_InstrKind k = fn->value_table[instr->field.object]->kind;
            obj_is_ptr = (k == IRON_IR_HEAP_ALLOC || k == IRON_IR_RC_ALLOC);
        }
        iron_strbuf_appendf(sb, "%s%s = ", obj_is_ptr ? "->" : ".", instr->field.field);
        emit_val(sb, instr->field.value);
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    case IRON_IR_GET_INDEX: {
        /* ARR-02: Check if the source array is stack-represented */
        IronIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->index.array);
        if (sa_origin != IRON_IR_VALUE_INVALID) {
            /* Direct C indexing: result = array[index]; */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
            emit_val(sb, instr->index.array);
            iron_strbuf_appendf(sb, "[");
            emit_val(sb, instr->index.index);
            iron_strbuf_appendf(sb, "];\n");
        } else {
            /* result = Iron_List_<suffix>_get(&array, index) */
            const char *list_type = "Iron_List_int64_t"; /* default fallback */
            if (instr->index.array < (IronIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[instr->index.array]) {
                Iron_Type *arr_t = fn->value_table[instr->index.array]->type;
                if (arr_t) list_type = emit_type_to_c(arr_t, ctx);
            }
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = %s_get(&", list_type);
            emit_val(sb, instr->index.array);
            iron_strbuf_appendf(sb, ", ");
            emit_val(sb, instr->index.index);
            iron_strbuf_appendf(sb, ");\n");
        }
        break;
    }

    case IRON_IR_SET_INDEX: {
        /* ARR-02: Check if the target array is stack-represented */
        IronIR_ValueId sa_origin = get_stack_array_origin(ctx, instr->index.array);
        if (sa_origin != IRON_IR_VALUE_INVALID) {
            /* Direct C indexing: array[index] = value; */
            emit_indent(sb, ind);
            emit_val(sb, instr->index.array);
            iron_strbuf_appendf(sb, "[");
            emit_val(sb, instr->index.index);
            iron_strbuf_appendf(sb, "] = ");
            emit_val(sb, instr->index.value);
            iron_strbuf_appendf(sb, ";\n");
        } else {
            /* Iron_List_<suffix>_set(&array, index, value) */
            const char *list_type = "Iron_List_int64_t"; /* default fallback */
            if (instr->index.array < (IronIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[instr->index.array]) {
                Iron_Type *arr_t = fn->value_table[instr->index.array]->type;
                if (arr_t) list_type = emit_type_to_c(arr_t, ctx);
            }
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s_set(&", list_type);
            emit_val(sb, instr->index.array);
            iron_strbuf_appendf(sb, ", ");
            emit_val(sb, instr->index.index);
            iron_strbuf_appendf(sb, ", ");
            emit_val(sb, instr->index.value);
            iron_strbuf_appendf(sb, ");\n");
        }
        break;
    }

    /* ── Call ───────────────────────────────────────────────────────────── */

    case IRON_IR_CALL: {
        /* Check for __builtin_fill(count, value) -> stack array or Iron_List_T */
        {
            IronIR_ValueId fptr = instr->call.func_ptr;
            if (fptr != IRON_IR_VALUE_INVALID &&
                fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[fptr] != NULL &&
                fn->value_table[fptr]->kind == IRON_IR_FUNC_REF &&
                strcmp(fn->value_table[fptr]->func_ref.func_name,
                       "__builtin_fill") == 0 &&
                instr->call.arg_count == 2) {

                /* Check if count is a compile-time constant <= 256 AND
                 * this fill() result is NOT used as an escaping value
                 * (i.e., it's in the stack_array_ids map from pre-scan). */
                IronIR_ValueId count_id = instr->call.args[0];
                IronIR_Instr *count_instr = NULL;
                if (count_id < (IronIR_ValueId)arrlen(fn->value_table))
                    count_instr = fn->value_table[count_id];
                bool use_stack = false;
                int64_t fill_count = 0;
                if (count_instr && count_instr->kind == IRON_IR_CONST_INT &&
                    count_instr->const_int.value > 0 &&
                    count_instr->const_int.value <= 256) {
                    /* Check pre-scan marked this as stack-eligible */
                    IronIR_ValueId sa = get_stack_array_origin(ctx, instr->id);
                    if (sa != IRON_IR_VALUE_INVALID) {
                        use_stack = true;
                        fill_count = count_instr->const_int.value;
                    }
                }

                if (use_stack) {
                    /* Emit as stack array with fill loop (constant count) */
                    const char *elem_type = "int64_t";
                    if (instr->type && instr->type->kind == IRON_TYPE_ARRAY &&
                        instr->type->array.elem)
                        elem_type = emit_type_to_c(instr->type->array.elem, ctx);
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "%s ", elem_type);
                    emit_val(sb, instr->id);
                    iron_strbuf_appendf(sb, "[%lld];\n", (long long)fill_count);
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "for (int64_t _fill_i = 0; _fill_i < %lld; _fill_i++) ",
                                         (long long)fill_count);
                    emit_val(sb, instr->id);
                    iron_strbuf_appendf(sb, "[_fill_i] = ");
                    emit_val(sb, instr->call.args[1]);
                    iron_strbuf_appendf(sb, ";\n");
                    /* Emit companion length variable */
                    emit_indent(sb, ind);
                    iron_strbuf_appendf(sb, "int64_t ");
                    emit_val(sb, instr->id);
                    iron_strbuf_appendf(sb, "_len = %lld;\n", (long long)fill_count);
                    /* Mark as stack array */
                    mark_stack_array(ctx, instr->id, instr->id);
                } else {
                    /* Check if dynamic-count fill is stack-eligible (VLA) */
                    IronIR_ValueId sa = get_stack_array_origin(ctx, instr->id);
                    if (sa != IRON_IR_VALUE_INVALID) {
                        /* VLA path: stack-allocated variable-length array */
                        const char *elem_type = "int64_t";
                        if (instr->type && instr->type->kind == IRON_TYPE_ARRAY &&
                            instr->type->array.elem)
                            elem_type = emit_type_to_c(instr->type->array.elem, ctx);
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "%s ", elem_type);
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, "[");
                        emit_val(sb, instr->call.args[0]);
                        iron_strbuf_appendf(sb, "];\n");
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "for (int64_t _fill_i = 0; _fill_i < ");
                        emit_val(sb, instr->call.args[0]);
                        iron_strbuf_appendf(sb, "; _fill_i++) ");
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, "[_fill_i] = ");
                        emit_val(sb, instr->call.args[1]);
                        iron_strbuf_appendf(sb, ";\n");
                        /* Emit companion length variable */
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "int64_t ");
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, "_len = ");
                        emit_val(sb, instr->call.args[0]);
                        iron_strbuf_appendf(sb, ";\n");
                        /* Mark as stack array */
                        mark_stack_array(ctx, instr->id, instr->id);
                    } else {
                        /* Heap path: create() + push loop */
                        const char *list_type = emit_type_to_c(instr->type, ctx);
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "%s ", list_type);
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, " = %s_create();\n", list_type);
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "for (int64_t _fill_i = 0; _fill_i < ");
                        emit_val(sb, instr->call.args[0]);
                        iron_strbuf_appendf(sb, "; _fill_i++) {\n");
                        emit_indent(sb, ind + 1);
                        iron_strbuf_appendf(sb, "%s_push(&", list_type);
                        emit_val(sb, instr->id);
                        iron_strbuf_appendf(sb, ", ");
                        emit_val(sb, instr->call.args[1]);
                        iron_strbuf_appendf(sb, ");\n");
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "}\n");
                    }
                }
                break;
            }
        }

        bool is_void = (instr->type == NULL ||
                        instr->type->kind == IRON_TYPE_VOID);

        emit_indent(sb, ind);
        if (!is_void) {
            iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = ");
        }

        if (instr->call.func_decl) {
            /* Direct call: use the mangled function name */
            Iron_FuncDecl *fd = instr->call.func_decl;
            if (fd->is_extern && fd->extern_c_name) {
                iron_strbuf_appendf(sb, "%s(", fd->extern_c_name);
            } else {
                iron_strbuf_appendf(sb, "%s(", mangle_func_name(fd->name, ctx->arena));
            }
        } else {
            /* Indirect call: check if func_ptr is a FUNC_REF — if so, emit
             * a direct call to avoid the invalid (void (*)(...)) cast pattern. */
            IronIR_ValueId fptr = instr->call.func_ptr;
            bool emitted_direct = false;
            if (fptr != IRON_IR_VALUE_INVALID &&
                fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[fptr] != NULL &&
                fn->value_table[fptr]->kind == IRON_IR_FUNC_REF) {
                /* Direct call via known function name — honor extern_c_name */
                const char *c_name = resolve_func_c_name(
                    ctx, fn->value_table[fptr]->func_ref.func_name);
                iron_strbuf_appendf(sb, "%s(", c_name);
                emitted_direct = true;
            }
            if (!emitted_direct) {
                /* True indirect call through an arbitrary function pointer.
                 * Cast to (ret_type (*)(...)) to allow passing typed arguments
                 * without implicit conversions. Using variadic (...) avoids
                 * strict param-type checking while preserving the return type. */
                const char *ret_c = (instr->type && instr->type->kind != IRON_TYPE_VOID)
                    ? emit_type_to_c(instr->type, ctx)
                    : "void";
                /* Use empty-parameter function pointer cast: (ret_type (*)()) allows
                 * passing any arguments (C permits calling via unprototyped function
                 * pointer). This avoids strict type checking while preserving return type. */
                iron_strbuf_appendf(sb, "((%s (*)())", ret_c);
                emit_val(sb, fptr);
                iron_strbuf_appendf(sb, ")(");
            }
        }

        /* Determine if this is an extern call — extern calls need Iron_String
         * arguments converted to const char* via iron_string_cstr(). */
        bool is_extern_call = false;
        if (instr->call.func_decl && instr->call.func_decl->is_extern) {
            is_extern_call = true;
        } else if (!instr->call.func_decl) {
            /* Indirect call: check if the FUNC_REF target is extern */
            IronIR_ValueId fptr = instr->call.func_ptr;
            if (fptr != IRON_IR_VALUE_INVALID &&
                fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[fptr] != NULL &&
                fn->value_table[fptr]->kind == IRON_IR_FUNC_REF) {
                const char *ref_name = fn->value_table[fptr]->func_ref.func_name;
                for (int fi = 0; fi < ctx->module->func_count; fi++) {
                    if (strcmp(ctx->module->funcs[fi]->name, ref_name) == 0 &&
                        ctx->module->funcs[fi]->is_extern) {
                        is_extern_call = true;
                        break;
                    }
                }
            }
        }

        /* PARAM-01/02: Resolve callee IR name for pointer-mode check */
        const char *callee_ir_name = NULL;
        if (instr->call.func_decl && !instr->call.func_decl->is_extern) {
            callee_ir_name = instr->call.func_decl->name;
        } else if (!instr->call.func_decl) {
            IronIR_ValueId fptr2 = instr->call.func_ptr;
            if (fptr2 != IRON_IR_VALUE_INVALID &&
                fptr2 < (IronIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[fptr2] != NULL &&
                fn->value_table[fptr2]->kind == IRON_IR_FUNC_REF) {
                const char *rn2 = fn->value_table[fptr2]->func_ref.func_name;
                IronIR_Func *cf = find_ir_func(ctx, rn2);
                if (cf && !cf->is_extern) callee_ir_name = rn2;
            }
        }

        bool first_arg = true;
        for (int i = 0; i < instr->call.arg_count; i++) {
            if (!first_arg) iron_strbuf_appendf(sb, ", ");
            first_arg = false;
            IronIR_ValueId arg_id = instr->call.args[i];

            /* PARAM-01/02: Check if callee expects pointer+length */
            ArrayParamMode callee_pmode = ARRAY_PARAM_LIST;
            if (callee_ir_name)
                callee_pmode = get_array_param_mode(ctx, callee_ir_name, i);

            if (callee_pmode == ARRAY_PARAM_CONST_PTR ||
                callee_pmode == ARRAY_PARAM_MUT_PTR) {
                IronIR_ValueId sa_origin = get_stack_array_origin(ctx, arg_id);
                if (sa_origin != IRON_IR_VALUE_INVALID) {
                    /* Stack array: pass pointer + companion length */
                    emit_val(sb, arg_id);
                    iron_strbuf_appendf(sb, ", ");
                    emit_val(sb, arg_id);
                    iron_strbuf_appendf(sb, "_len");
                } else {
                    /* Iron_List_T: extract .items and .count */
                    emit_val(sb, arg_id);
                    iron_strbuf_appendf(sb, ".items, ");
                    emit_val(sb, arg_id);
                    iron_strbuf_appendf(sb, ".count");
                }
                continue;
            }

            /* For extern calls, convert Iron_String arguments to const char* */
            bool is_string_arg = false;
            if (is_extern_call && arg_id != IRON_IR_VALUE_INVALID &&
                arg_id < (IronIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[arg_id] != NULL) {
                Iron_Type *arg_type = fn->value_table[arg_id]->type;
                if (arg_type && arg_type->kind == IRON_TYPE_STRING) {
                    is_string_arg = true;
                }
            }
            if (is_string_arg) {
                iron_strbuf_appendf(sb, "iron_string_cstr(&");
                emit_val(sb, arg_id);
                iron_strbuf_appendf(sb, ")");
            } else {
                emit_val(sb, arg_id);
            }
        }
        iron_strbuf_appendf(sb, ");\n");
        break;
    }

    /* ── Control flow ───────────────────────────────────────────────────── */

    case IRON_IR_JUMP:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "goto %s;\n",
                            resolve_label(fn, instr->jump.target, ctx->arena));
        break;

    case IRON_IR_BRANCH:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "if (");
        emit_val(sb, instr->branch.cond);
        iron_strbuf_appendf(sb, ") goto %s; else goto %s;\n",
                            resolve_label(fn, instr->branch.then_block, ctx->arena),
                            resolve_label(fn, instr->branch.else_block, ctx->arena));
        break;

    case IRON_IR_SWITCH: {
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "switch (");
        emit_val(sb, instr->sw.subject);
        iron_strbuf_appendf(sb, ") {\n");
        for (int i = 0; i < instr->sw.case_count; i++) {
            emit_indent(sb, ind + 1);
            iron_strbuf_appendf(sb, "case %d: goto %s;\n",
                                instr->sw.case_values[i],
                                resolve_label(fn, instr->sw.case_blocks[i], ctx->arena));
        }
        emit_indent(sb, ind + 1);
        iron_strbuf_appendf(sb, "default: goto %s;\n",
                            resolve_label(fn, instr->sw.default_block, ctx->arena));
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "}\n");
        break;
    }

    case IRON_IR_RETURN: {
        /* COLL-04: Emit _free() for non-escaping heap arrays before return.
         * We iterate over all unique original ARRAY_LIT ids tracked in
         * heap_array_ids and free those that haven't escaped. */
        if (ctx->heap_array_ids) {
            /* Collect unique original ARRAY_LIT ids to avoid double-free */
            struct { IronIR_ValueId key; bool value; } *freed = NULL;
            for (ptrdiff_t hi = 0; hi < hmlen(ctx->heap_array_ids); hi++) {
                IronIR_ValueId orig = ctx->heap_array_ids[hi].value;
                /* Skip if already freed, if this array escapes, or if it's a stack array */
                if (hmgeti(freed, orig) >= 0) continue;
                if (hmgeti(ctx->escaped_heap_ids, orig) >= 0) continue;
                if (hmgeti(ctx->stack_array_ids, orig) >= 0) continue;
                hmput(freed, orig, true);

                /* Look up the original instruction to get the list type */
                if (orig < (IronIR_ValueId)arrlen(fn->value_table) &&
                    fn->value_table[orig] != NULL) {
                    IronIR_Instr *orig_instr = fn->value_table[orig];
                    /* (debug removed) */
                    const char *list_type = NULL;
                    if (orig_instr->kind == IRON_IR_ARRAY_LIT) {
                        Iron_Type *arr_type = iron_type_make_array(
                            ctx->arena, orig_instr->array_lit.elem_type, -1);
                        list_type = emit_type_to_c(arr_type, ctx);
                    } else if (orig_instr->type &&
                               orig_instr->type->kind == IRON_TYPE_ARRAY) {
                        /* __builtin_fill result */
                        list_type = emit_type_to_c(orig_instr->type, ctx);
                    }
                    if (list_type &&
                        get_stack_array_origin(ctx, orig) == IRON_IR_VALUE_INVALID) {
                        emit_indent(sb, ind);
                        iron_strbuf_appendf(sb, "%s_free(&_v%u);\n",
                                            list_type, (unsigned)orig);
                    }
                }
            }
            hmfree(freed);
        }

        emit_indent(sb, ind);
        if (instr->ret.is_void) {
            iron_strbuf_appendf(sb, "return;\n");
        } else {
            iron_strbuf_appendf(sb, "return ");
            emit_val(sb, instr->ret.value);
            iron_strbuf_appendf(sb, ";\n");
        }
        break;
    }

    /* ── Cast ───────────────────────────────────────────────────────────── */

    case IRON_IR_CAST: {
        const char *target_c = emit_type_to_c(instr->cast.target_type, ctx);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", target_c);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (%s)", target_c);
        emit_val(sb, instr->cast.value);
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    /* ── Memory management ──────────────────────────────────────────────── */

    case IRON_IR_HEAP_ALLOC: {
        /* The inner_val is an already-constructed value; wrap it in a pointer.
         * instr->type is the inner (value) type, e.g. Iron_Data.
         * The heap result is a pointer: Iron_Data *_vN = malloc(sizeof(Iron_Data));
         * *_vN = inner_val; */
        const char *val_type = emit_type_to_c(instr->type, ctx);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s *", val_type);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (%s *)malloc(sizeof(%s));\n", val_type, val_type);
        /* Store the inner value into the allocated memory */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "*");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->heap_alloc.inner_val);
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    case IRON_IR_RC_ALLOC: {
        /* rc allocates via malloc (simplified: no actual ref-count tracking yet).
         * instr->type is IRON_TYPE_RC wrapping an inner type; result is a pointer
         * to the inner type (e.g. rc Config -> Iron_Config *). */
        const char *val_type = NULL;
        if (instr->type && instr->type->kind == IRON_TYPE_RC && instr->type->rc.inner) {
            val_type = emit_type_to_c(instr->type->rc.inner, ctx);
        } else {
            val_type = emit_type_to_c(instr->type, ctx);
        }
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s *", val_type);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (%s *)malloc(sizeof(%s));\n", val_type, val_type);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "*");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->rc_alloc.inner_val);
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    case IRON_IR_FREE:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "free(");
        emit_val(sb, instr->free_instr.value);
        iron_strbuf_appendf(sb, ");\n");
        break;

    /* ── Construct ──────────────────────────────────────────────────────── */

    case IRON_IR_CONSTRUCT: {
        /* Emit: T _vN = { .field0 = _vA, .field1 = _vB, ... }; */
        const char *c_type = emit_type_to_c(instr->construct.type, ctx);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", c_type);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = {");

        /* Use field names from the object decl if available */
        if (instr->construct.type &&
            instr->construct.type->kind == IRON_TYPE_OBJECT &&
            instr->construct.type->object.decl) {
            Iron_ObjectDecl *od = instr->construct.type->object.decl;
            int field_start = 0;
            /* If the object has a parent, first field is _base */
            if (od->extends_name) {
                if (instr->construct.field_count > 0) {
                    iron_strbuf_appendf(sb, " ._base = ");
                    emit_val(sb, instr->construct.field_vals[0]);
                    field_start = 1;
                    if (instr->construct.field_count > 1)
                        iron_strbuf_appendf(sb, ",");
                }
            }
            int od_field_idx = 0;
            for (int i = field_start; i < instr->construct.field_count; i++) {
                if (i > field_start) iron_strbuf_appendf(sb, ",");
                if (od_field_idx < od->field_count) {
                    Iron_Field *f = (Iron_Field *)od->fields[od_field_idx++];
                    iron_strbuf_appendf(sb, " .%s = ", f->name);
                } else {
                    iron_strbuf_appendf(sb, " ");
                }
                emit_val(sb, instr->construct.field_vals[i]);
            }
        } else {
            /* Fallback: positional initialization */
            for (int i = 0; i < instr->construct.field_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ", ");
                else iron_strbuf_appendf(sb, " ");
                emit_val(sb, instr->construct.field_vals[i]);
            }
        }
        iron_strbuf_appendf(sb, " };\n");
        break;
    }

    /* ── Array literal ──────────────────────────────────────────────────── */

    case IRON_IR_ARRAY_LIT: {
        if (instr->array_lit.use_stack_repr) {
            /* ARR-01: Emit as C stack array for known-size arrays (<= 256 elements).
             * Produces: elem_type _vN[] = {v0, v1, ...};
             *           int64_t _vN_len = count; */
            const char *elem_c_type = emit_type_to_c(instr->array_lit.elem_type, ctx);
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", elem_c_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, "[] = {");
            for (int i = 0; i < instr->array_lit.element_count; i++) {
                if (i > 0) iron_strbuf_appendf(sb, ", ");
                emit_val(sb, instr->array_lit.elements[i]);
            }
            iron_strbuf_appendf(sb, "};\n");
            /* Emit companion length variable */
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "int64_t ");
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, "_len = %d;\n", instr->array_lit.element_count);
            /* Register this value as a stack array for downstream use */
            mark_stack_array(ctx, instr->id, instr->id);
        } else {
            /* Create a type-specific Iron_List_<suffix> and push each element.
             * e.g. [Int] -> Iron_List_int64_t_create(), Iron_List_int64_t_push() */
            Iron_Type *arr_type = iron_type_make_array(ctx->arena, instr->array_lit.elem_type, -1);
            const char *list_type = emit_type_to_c(arr_type, ctx);
            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s ", list_type);
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = %s_create();\n", list_type);
            for (int i = 0; i < instr->array_lit.element_count; i++) {
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "%s_push(&", list_type);
                emit_val(sb, instr->id);
                iron_strbuf_appendf(sb, ", ");
                emit_val(sb, instr->array_lit.elements[i]);
                iron_strbuf_appendf(sb, ");\n");
            }
        }
        break;
    }

    /* ── Slice ──────────────────────────────────────────────────────────── */

    case IRON_IR_SLICE: {
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "Iron_List ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = iron_list_slice(");
        emit_val(sb, instr->slice.array);
        iron_strbuf_appendf(sb, ", ");
        if (instr->slice.start == IRON_IR_VALUE_INVALID) {
            iron_strbuf_appendf(sb, "0");
        } else {
            emit_val(sb, instr->slice.start);
        }
        iron_strbuf_appendf(sb, ", ");
        if (instr->slice.end == IRON_IR_VALUE_INVALID) {
            iron_strbuf_appendf(sb, "-1");
        } else {
            emit_val(sb, instr->slice.end);
        }
        iron_strbuf_appendf(sb, ");\n");
        break;
    }

    /* ── Null checks ────────────────────────────────────────────────────── */

    case IRON_IR_IS_NULL:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = !");
        emit_val(sb, instr->null_check.value);
        iron_strbuf_appendf(sb, ".has_value;\n");
        break;

    case IRON_IR_IS_NOT_NULL:
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "bool ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->null_check.value);
        iron_strbuf_appendf(sb, ".has_value;\n");
        break;

    /* ── String interpolation ───────────────────────────────────────────── */

    case IRON_IR_INTERP_STRING: {
        /* Two-pass snprintf interpolation:
         *   Pass 1: measure with snprintf(NULL, 0, fmt, args...)
         *   Pass 2: allocate and fill with snprintf(buf, n+1, fmt, args...)
         * Each part is given a printf format specifier based on its type.
         * String parts use iron_string_cstr() to convert to C string.
         *
         * IMPORTANT: the result variable is declared BEFORE the inner block
         * so it remains in scope for subsequent instructions. */

        /* Declare result variable at outer scope */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "Iron_String ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, ";\n");

        /* Open temporary block for buf variables */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "{\n");
        ind++;

        /* Build format string and collect conversion expressions */
        Iron_StrBuf fmt_sb   = iron_strbuf_create(64);
        Iron_StrBuf args_sb  = iron_strbuf_create(128);
        bool        first_arg = true;

        for (int i = 0; i < instr->interp_string.part_count; i++) {
            IronIR_ValueId part_id = instr->interp_string.parts[i];
            /* Look up the type of this part via the function's value_table */
            Iron_Type *part_type = NULL;
            if (part_id < (IronIR_ValueId)arrlen(fn->value_table) &&
                fn->value_table[part_id]) {
                part_type = fn->value_table[part_id]->type;
            }

            /* Determine format specifier and conversion */
            const char *fmt_spec = "%s";  /* default: string */
            if (part_type) {
                switch (part_type->kind) {
                    case IRON_TYPE_INT:
                    case IRON_TYPE_INT8:
                    case IRON_TYPE_INT16:
                    case IRON_TYPE_INT32:
                    case IRON_TYPE_INT64:
                    case IRON_TYPE_UINT:
                    case IRON_TYPE_UINT8:
                    case IRON_TYPE_UINT16:
                    case IRON_TYPE_UINT32:
                    case IRON_TYPE_UINT64:
                        fmt_spec = "%lld";
                        break;
                    case IRON_TYPE_FLOAT:
                    case IRON_TYPE_FLOAT32:
                    case IRON_TYPE_FLOAT64:
                        fmt_spec = "%g";
                        break;
                    case IRON_TYPE_BOOL:
                        /* emitted as ternary inline */
                        fmt_spec = "%s";
                        break;
                    default:
                        fmt_spec = "%s";
                        break;
                }
            }

            iron_strbuf_appendf(&fmt_sb, "%s", fmt_spec);

            if (!first_arg) iron_strbuf_appendf(&args_sb, ", ");
            first_arg = false;

            if (part_type) {
                switch (part_type->kind) {
                    case IRON_TYPE_INT:
                    case IRON_TYPE_INT8:
                    case IRON_TYPE_INT16:
                    case IRON_TYPE_INT32:
                    case IRON_TYPE_INT64:
                    case IRON_TYPE_UINT:
                    case IRON_TYPE_UINT8:
                    case IRON_TYPE_UINT16:
                    case IRON_TYPE_UINT32:
                    case IRON_TYPE_UINT64: {
                        /* Cast to long long for %lld */
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "(long long)(%s)",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                    case IRON_TYPE_FLOAT:
                    case IRON_TYPE_FLOAT32:
                    case IRON_TYPE_FLOAT64: {
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "(double)(%s)",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                    case IRON_TYPE_BOOL: {
                        /* "true" / "false" ternary */
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "(%s ? \"true\" : \"false\")",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                    case IRON_TYPE_STRING: {
                        /* iron_string_cstr() converts to const char* */
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "iron_string_cstr(&%s)",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                    default: {
                        /* Generic: try cstr conversion */
                        Iron_StrBuf tmp = iron_strbuf_create(32);
                        emit_val(&tmp, part_id);
                        iron_strbuf_appendf(&args_sb, "iron_string_cstr(&%s)",
                                            iron_strbuf_get(&tmp));
                        iron_strbuf_free(&tmp);
                        break;
                    }
                }
            } else {
                /* No type info: fall back to cstr */
                Iron_StrBuf tmp = iron_strbuf_create(32);
                emit_val(&tmp, part_id);
                iron_strbuf_appendf(&args_sb, "iron_string_cstr(&%s)",
                                    iron_strbuf_get(&tmp));
                iron_strbuf_free(&tmp);
            }
        }

        const char *fmt_str  = iron_strbuf_get(&fmt_sb);
        const char *args_str = iron_strbuf_get(&args_sb);

        /* Pass 1: measure */
        emit_indent(sb, ind);
        if (instr->interp_string.part_count > 0) {
            iron_strbuf_appendf(sb,
                "int _interp_len_%u = snprintf(NULL, 0, \"%s\", %s);\n",
                instr->id, fmt_str, args_str);
        } else {
            iron_strbuf_appendf(sb,
                "int _interp_len_%u = 0;\n", instr->id);
        }

        /* Allocate buffer (len + 1 for NUL terminator) */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb,
            "char *_interp_buf_%u = (char *)malloc((size_t)(_interp_len_%u + 1));\n",
            instr->id, instr->id);

        /* Pass 2: fill */
        emit_indent(sb, ind);
        if (instr->interp_string.part_count > 0) {
            iron_strbuf_appendf(sb,
                "snprintf(_interp_buf_%u, (size_t)(_interp_len_%u + 1), \"%s\", %s);\n",
                instr->id, instr->id, fmt_str, args_str);
        } else {
            iron_strbuf_appendf(sb, "_interp_buf_%u[0] = '\\0';\n", instr->id);
        }

        iron_strbuf_free(&fmt_sb);
        iron_strbuf_free(&args_sb);

        /* Assign result to the outer-scoped variable */
        emit_indent(sb, ind);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb,
            " = iron_string_from_cstr(_interp_buf_%u, (size_t)_interp_len_%u);\n",
            instr->id, instr->id);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "free(_interp_buf_%u);\n", instr->id);

        ind--;
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "}\n");
        break;
    }

    /* ── Closures / function references ─────────────────────────────────── */

    case IRON_IR_MAKE_CLOSURE: {
        /* Allocate a closure env struct and store captures.
         * When there are no captures, skip the env struct entirely — just
         * store the function pointer as a void*. */
        const char *func_name = instr->make_closure.lifted_func_name;

        if (instr->make_closure.capture_count > 0) {
            /* Emit env struct name */
            Iron_StrBuf env_name_sb = iron_strbuf_create(64);
            iron_strbuf_appendf(&env_name_sb, "%s_env", func_name);
            const char *env_name = iron_arena_strdup(ctx->arena,
                                                      iron_strbuf_get(&env_name_sb),
                                                      env_name_sb.len);
            iron_strbuf_free(&env_name_sb);

            emit_indent(sb, ind);
            iron_strbuf_appendf(sb, "%s *_env_%u = (%s *)malloc(sizeof(%s));\n",
                                env_name, instr->id, env_name, env_name);
            for (int i = 0; i < instr->make_closure.capture_count; i++) {
                emit_indent(sb, ind);
                iron_strbuf_appendf(sb, "_env_%u->_cap%d = ", instr->id, i);
                emit_val(sb, instr->make_closure.captures[i]);
                iron_strbuf_appendf(sb, ";\n");
            }
        }

        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "void* ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (void*)%s;\n", func_name);
        break;
    }

    case IRON_IR_FUNC_REF: {
        /* FUNC_REF values are resolved directly from the value_table at CALL
         * sites (see the IRON_IR_CALL case), so the emitted void* cast is
         * dead code.  Skip emission entirely for all FUNC_REF instructions. */
        break;
    }

    /* ── Concurrency ────────────────────────────────────────────────────── */

    case IRON_IR_SPAWN: {
        /* Iron_pool_submit returns void; emit as a statement (no return value) */
        const char *func_name = instr->spawn.lifted_func_name;
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "Iron_pool_submit(");
        if (instr->spawn.pool_val == IRON_IR_VALUE_INVALID) {
            iron_strbuf_appendf(sb, "Iron_global_pool");
        } else {
            emit_val(sb, instr->spawn.pool_val);
        }
        iron_strbuf_appendf(sb, ", (void (*)(void *))%s, NULL);\n", func_name);
        break;
    }

    case IRON_IR_PARALLEL_FOR: {
        /* Range-splitting parallel-for pattern using Iron_pool_submit.
         *
         * The IR chunk function has signature:
         *   void chunk_func(int64_t loop_var)
         * Iron_pool_submit expects:
         *   void (*fn)(void *)
         *
         * We emit:
         *   1. A context struct typedef:  chunk_func_ctx { int64_t start; int64_t end; }
         *   2. A wrapper function:        chunk_func_wrapper(void*) that loops start..end
         *      calling the original chunk_func for each iteration, then frees the ctx.
         *   3. Inline range-splitting logic that malloc's a ctx per chunk and submits.
         *   4. Iron_pool_barrier at the end.
         */
        const char *chunk_func = instr->parallel_for.chunk_func_name;

        /* Derive context struct name and wrapper name from chunk func name */
        Iron_StrBuf ctx_type_sb = iron_strbuf_create(64);
        iron_strbuf_appendf(&ctx_type_sb, "%s_ctx", chunk_func);
        const char *ctx_type = iron_arena_strdup(ctx->arena,
                                                   iron_strbuf_get(&ctx_type_sb),
                                                   ctx_type_sb.len);
        iron_strbuf_free(&ctx_type_sb);

        Iron_StrBuf wrapper_sb = iron_strbuf_create(64);
        iron_strbuf_appendf(&wrapper_sb, "%s_wrapper", chunk_func);
        const char *wrapper_name = iron_arena_strdup(ctx->arena,
                                                       iron_strbuf_get(&wrapper_sb),
                                                       wrapper_sb.len);
        iron_strbuf_free(&wrapper_sb);

        /* Emit context struct typedef into lifted_funcs section */
        iron_strbuf_appendf(&ctx->lifted_funcs, "typedef struct {\n");
        iron_strbuf_appendf(&ctx->lifted_funcs, "    int64_t start;\n");
        iron_strbuf_appendf(&ctx->lifted_funcs, "    int64_t end;\n");
        iron_strbuf_appendf(&ctx->lifted_funcs, "} %s;\n\n", ctx_type);

        /* Resolve the mangled C name for the chunk function.
         * The chunk func (e.g., "__pfor_0") is a user function and gets
         * the Iron_ prefix via mangle_func_name. */
        const char *chunk_c_name = mangle_func_name(chunk_func, ctx->arena);

        /* Emit wrapper function into lifted_funcs section.
         * The wrapper loops [start, end) and calls the original chunk function. */
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "static void %s(void *_arg) {\n", wrapper_name);
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    %s *_c = (%s *)_arg;\n", ctx_type, ctx_type);
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    int64_t _start = _c->start;\n");
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    int64_t _end   = _c->end;\n");
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    free(_arg);\n");
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    for (int64_t _i = _start; _i < _end; _i++) {\n");
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "        %s(_i);\n", chunk_c_name);
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "    }\n");
        iron_strbuf_appendf(&ctx->lifted_funcs,
            "}\n\n");

        /* Emit inline range-splitting and submission loop */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "{\n");
        int inner = ind + 1;

        /* _total = range value */
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "int64_t _total = ");
        emit_val(sb, instr->parallel_for.range_val);
        iron_strbuf_appendf(sb, ";\n");

        /* Thread count and chunk size */
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb,
            "int64_t _nthreads = (int64_t)Iron_pool_thread_count(Iron_global_pool);\n");
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "if (_nthreads < 1) _nthreads = 1;\n");
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb,
            "int64_t _chunk_size = (_total + _nthreads - 1) / _nthreads;\n");
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "if (_chunk_size < 1) _chunk_size = 1;\n");

        /* Submission loop */
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb,
            "for (int64_t _c = 0; _c < _total; _c += _chunk_size) {\n");
        int inner2 = inner + 1;
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb,
            "int64_t _end = (_c + _chunk_size > _total) ? _total : _c + _chunk_size;\n");
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb,
            "%s *_pctx = (%s *)malloc(sizeof(%s));\n",
            ctx_type, ctx_type, ctx_type);
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb, "_pctx->start = _c;\n");
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb, "_pctx->end = _end;\n");
        emit_indent(sb, inner2);
        iron_strbuf_appendf(sb,
            "Iron_pool_submit(Iron_global_pool, %s, _pctx);\n", wrapper_name);
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "}\n");

        /* Barrier */
        emit_indent(sb, inner);
        iron_strbuf_appendf(sb, "Iron_pool_barrier(Iron_global_pool);\n");

        /* Close scope */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "}\n");
        break;
    }

    case IRON_IR_AWAIT:
        emit_indent(sb, ind);
        if (instr->type) {
            iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
            emit_val(sb, instr->id);
            iron_strbuf_appendf(sb, " = (%s)iron_future_await(",
                                emit_type_to_c(instr->type, ctx));
        } else {
            iron_strbuf_appendf(sb, "iron_future_await(");
        }
        emit_val(sb, instr->await.handle);
        iron_strbuf_appendf(sb, ");\n");
        break;

    /* ── SSA Phi (should be eliminated) ─────────────────────────────────── */

    case IRON_IR_PHI:
        /* Should never reach here — phi_eliminate() runs before emission */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "/* ERROR: phi not eliminated _v%u */\n",
                            instr->id);
        break;

    /* ── Poison ─────────────────────────────────────────────────────────── */

    case IRON_IR_POISON:
        /* Skip poison instructions — they represent undefined behavior */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "/* poison */\n");
        break;

    /* ── Sentinel ───────────────────────────────────────────────────────── */

    case IRON_IR_INSTR_COUNT:
        /* Should never appear */
        break;
    }

    (void)type_is_pointer; /* suppress unused warning */
}

/* ── Function emission ────────────────────────────────────────────────────── */

/* Determine if a function is a "lifted" function (lambda/spawn/parallel body) */
static bool is_lifted_func(const char *name) {
    if (!name) return false;
    return (strstr(name, "lambda_") != NULL ||
            strstr(name, "spawn_")  != NULL ||
            strstr(name, "parallel_") != NULL);
}

static void emit_func_signature(Iron_StrBuf *sb, IronIR_Func *fn,
                                 EmitCtx *ctx, bool with_newline) {
    const char *ret_c = fn->return_type
                        ? emit_type_to_c(fn->return_type, ctx)
                        : "void";
    const char *c_name = fn->is_extern && fn->extern_c_name
                        ? fn->extern_c_name
                        : mangle_func_name(fn->name, ctx->arena);
    iron_strbuf_appendf(sb, "%s %s(", ret_c, c_name);
    for (int i = 0; i < fn->param_count; i++) {
        if (i > 0) iron_strbuf_appendf(sb, ", ");
        /* Each param consumes two ValueIds: one synthetic (the incoming argument
         * value) and one alloca slot.  They are allocated in lower.c as:
         *   param_val_id = next_value_id++    (synthetic, odd: 1, 3, 5, …)
         *   alloca slot  = alloc_instr(…)     (even: 2, 4, 6, …)
         * so param i's synthetic id = i*2 + 1.
         * The C signature must use the synthetic id so the STORE in the entry
         * block (store alloca_slot = param_val_id) resolves correctly. */
        Iron_Type *pt = fn->params[i].type;
        int param_val_id = i * 2 + 1;
        /* PARAM-01/02: Check if this array param uses pointer mode */
        ArrayParamMode pmode = ARRAY_PARAM_LIST;
        if (pt && pt->kind == IRON_TYPE_ARRAY)
            pmode = get_array_param_mode(ctx, fn->name, i);
        if (pmode == ARRAY_PARAM_CONST_PTR) {
            const char *elem_c = emit_type_to_c(pt->array.elem, ctx);
            iron_strbuf_appendf(sb, "const %s *_v%d, int64_t _v%d_len",
                                elem_c, param_val_id, param_val_id);
        } else if (pmode == ARRAY_PARAM_MUT_PTR) {
            const char *elem_c = emit_type_to_c(pt->array.elem, ctx);
            iron_strbuf_appendf(sb, "%s *_v%d, int64_t _v%d_len",
                                elem_c, param_val_id, param_val_id);
        } else if (pt) {
            iron_strbuf_appendf(sb, "%s _v%d",
                                emit_type_to_c(pt, ctx), param_val_id);
        } else {
            iron_strbuf_appendf(sb, "void* _v%d", param_val_id);
        }
    }
    if (fn->param_count == 0) {
        iron_strbuf_appendf(sb, "void");
    }
    iron_strbuf_appendf(sb, ")");
    if (with_newline) {
        iron_strbuf_appendf(sb, ";\n");
    }
}

static void emit_func_body(EmitCtx *ctx, IronIR_Func *fn) {
    /* Choose target buffer: lifted functions go to lifted_funcs */
    Iron_StrBuf *sb = is_lifted_func(fn->name)
                      ? &ctx->lifted_funcs
                      : &ctx->implementations;

    /* Reset per-function stack-array tracking map */
    hmfree(ctx->stack_array_ids);
    ctx->stack_array_ids = NULL;

    /* Reset per-function heap-array lifecycle tracking */
    hmfree(ctx->heap_array_ids);
    ctx->heap_array_ids = NULL;
    hmfree(ctx->escaped_heap_ids);
    ctx->escaped_heap_ids = NULL;

    /* Reset per-function parameter alias tracking */
    hmfree(ctx->param_alias_ids);
    ctx->param_alias_ids = NULL;

    /* Pre-scan: identify allocas that receive stack arrays via STORE.
     * Build a mapping from STORE(alloca_ptr, stack_array_val) so that
     * the alloca can be emitted as elem_type* instead of Iron_List_T,
     * and LOADs from it are propagated as stack array references.
     * We also build the set: stack-array-literal IDs. */
    {
        /* Phase A: collect all stack-array literal IDs and fill() with constant count */
        struct { IronIR_ValueId key; IronIR_ValueId value; } *sa_pre = NULL;
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_ARRAY_LIT && instr->array_lit.use_stack_repr) {
                    hmput(sa_pre, instr->id, instr->id);
                }
                /* __builtin_fill — all calls (constant or dynamic count) */
                if (instr->kind == IRON_IR_CALL && instr->call.arg_count == 2 &&
                    instr->type && instr->type->kind == IRON_TYPE_ARRAY) {
                    IronIR_ValueId fptr = instr->call.func_ptr;
                    if (fptr != IRON_IR_VALUE_INVALID &&
                        fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                        fn->value_table[fptr] != NULL &&
                        fn->value_table[fptr]->kind == IRON_IR_FUNC_REF &&
                        strcmp(fn->value_table[fptr]->func_ref.func_name,
                               "__builtin_fill") == 0) {
                        /* Skip if revoked by escape analysis */
                        if (hmgeti(ctx->revoked_fill_ids, instr->id) < 0) {
                            hmput(sa_pre, instr->id, instr->id);
                        }
                    }
                }
            }
        }
        /* Phase A2 (PARAM-01/02): inject pointer-mode array params so
         * propagation in Phase B reaches allocas receiving param values. */
        for (int ppi = 0; ppi < fn->param_count; ppi++) {
            Iron_Type *ppt = fn->params[ppi].type;
            if (!ppt || ppt->kind != IRON_TYPE_ARRAY) continue;
            ArrayParamMode pmode = get_array_param_mode(ctx, fn->name, ppi);
            if (pmode == ARRAY_PARAM_CONST_PTR || pmode == ARRAY_PARAM_MUT_PTR) {
                IronIR_ValueId pvid = (IronIR_ValueId)(ppi * 2 + 1);
                hmput(sa_pre, pvid, pvid);
            }
        }
        /* Phase B: propagate through STORE and LOAD chains */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_STORE) {
                    ptrdiff_t vi = hmgeti(sa_pre, instr->store.value);
                    if (vi >= 0) hmput(sa_pre, instr->store.ptr, sa_pre[vi].value);
                } else if (instr->kind == IRON_IR_LOAD) {
                    ptrdiff_t vi = hmgeti(sa_pre, instr->load.ptr);
                    if (vi >= 0) hmput(sa_pre, instr->id, sa_pre[vi].value);
                }
            }
        }
        /* Seed the ctx map with the pre-scan results */
        for (ptrdiff_t i = 0; i < hmlen(sa_pre); i++) {
            hmput(ctx->stack_array_ids, sa_pre[i].key, sa_pre[i].value);
        }
        hmfree(sa_pre);
    }

    /* ── Heap-array lifecycle pre-scan (COLL-04) ────────────────────────────
     * Collect all heap-allocated ARRAY_LIT instructions (!use_stack_repr)
     * and __builtin_fill CALL instructions.  Propagate through STORE/LOAD
     * chains.  Then determine which escape (via RETURN, SET_FIELD,
     * CONSTRUCT, CALL arg, or MAKE_CLOSURE capture). */
    {
        struct { IronIR_ValueId key; IronIR_ValueId value; } *ha_pre = NULL;

        /* Phase A: collect all heap-array literal IDs and builtin_fill results */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_ARRAY_LIT && !instr->array_lit.use_stack_repr) {
                    hmput(ha_pre, instr->id, instr->id);
                }
                /* __builtin_fill calls produce heap lists ONLY if not stack-eligible */
                if (instr->kind == IRON_IR_CALL && instr->type &&
                    instr->type->kind == IRON_TYPE_ARRAY) {
                    IronIR_ValueId fptr = instr->call.func_ptr;
                    if (fptr != IRON_IR_VALUE_INVALID &&
                        fptr < (IronIR_ValueId)arrlen(fn->value_table) &&
                        fn->value_table[fptr] != NULL &&
                        fn->value_table[fptr]->kind == IRON_IR_FUNC_REF &&
                        strcmp(fn->value_table[fptr]->func_ref.func_name,
                               "__builtin_fill") == 0) {
                        /* Only track as heap if NOT in stack_array_ids (already stack) */
                        if (hmgeti(ctx->stack_array_ids, instr->id) < 0) {
                            hmput(ha_pre, instr->id, instr->id);
                        }
                    }
                }
            }
        }

        /* Phase B: propagate through STORE/LOAD chains */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                if (instr->kind == IRON_IR_STORE) {
                    ptrdiff_t vi = hmgeti(ha_pre, instr->store.value);
                    if (vi >= 0) hmput(ha_pre, instr->store.ptr, ha_pre[vi].value);
                } else if (instr->kind == IRON_IR_LOAD) {
                    ptrdiff_t vi = hmgeti(ha_pre, instr->load.ptr);
                    if (vi >= 0) hmput(ha_pre, instr->id, ha_pre[vi].value);
                }
            }
        }

        /* Phase B2: remove any ha_pre entries whose origin is a stack array.
         * This handles fill() calls that were promoted to stack arrays. */
        {
            IronIR_ValueId *to_remove = NULL;
            for (ptrdiff_t i = 0; i < hmlen(ha_pre); i++) {
                IronIR_ValueId orig = ha_pre[i].value;
                if (hmgeti(ctx->stack_array_ids, orig) >= 0) {
                    arrput(to_remove, ha_pre[i].key);
                }
            }
            for (int i = 0; i < (int)arrlen(to_remove); i++) {
                hmdel(ha_pre, to_remove[i]);
            }
            arrfree(to_remove);
        }

        /* Phase C: mark escaping arrays */
        for (int bi = 0; bi < fn->block_count; bi++) {
            IronIR_Block *block = fn->blocks[bi];
            for (int ii = 0; ii < block->instr_count; ii++) {
                IronIR_Instr *instr = block->instrs[ii];
                /* Escapes via RETURN */
                if (instr->kind == IRON_IR_RETURN && !instr->ret.is_void) {
                    ptrdiff_t vi = hmgeti(ha_pre, instr->ret.value);
                    if (vi >= 0) hmput(ctx->escaped_heap_ids, ha_pre[vi].value, true);
                }
                /* Escapes via SET_FIELD (stored into object) */
                if (instr->kind == IRON_IR_SET_FIELD) {
                    ptrdiff_t vi = hmgeti(ha_pre, instr->field.value);
                    if (vi >= 0) hmput(ctx->escaped_heap_ids, ha_pre[vi].value, true);
                }
                /* Escapes via CONSTRUCT (embedded in struct) */
                if (instr->kind == IRON_IR_CONSTRUCT) {
                    for (int fi2 = 0; fi2 < instr->construct.field_count; fi2++) {
                        ptrdiff_t vi = hmgeti(ha_pre, instr->construct.field_vals[fi2]);
                        if (vi >= 0) hmput(ctx->escaped_heap_ids, ha_pre[vi].value, true);
                    }
                }
                /* Escapes via CALL argument (passed to another function) */
                if (instr->kind == IRON_IR_CALL) {
                    for (int ai = 0; ai < instr->call.arg_count; ai++) {
                        ptrdiff_t vi = hmgeti(ha_pre, instr->call.args[ai]);
                        if (vi >= 0) hmput(ctx->escaped_heap_ids, ha_pre[vi].value, true);
                    }
                }
                /* Escapes via MAKE_CLOSURE capture */
                if (instr->kind == IRON_IR_MAKE_CLOSURE) {
                    for (int ci = 0; ci < instr->make_closure.capture_count; ci++) {
                        ptrdiff_t vi = hmgeti(ha_pre, instr->make_closure.captures[ci]);
                        if (vi >= 0) hmput(ctx->escaped_heap_ids, ha_pre[vi].value, true);
                    }
                }
            }
        }

        /* Seed the ctx heap_array_ids map (skip stack arrays) */
        for (ptrdiff_t i = 0; i < hmlen(ha_pre); i++) {
            IronIR_ValueId orig = ha_pre[i].value;
            if (hmgeti(ctx->stack_array_ids, orig) >= 0) continue;
            hmput(ctx->heap_array_ids, ha_pre[i].key, ha_pre[i].value);
        }
        hmfree(ha_pre);
    }

    /* ── Read-only parameter alias pre-scan ────────────────────────────────
     * Detect parameter allocas that are:
     *   1. Stored to exactly once (the initial param store in the entry block)
     *   2. Never stored to again in any other block
     *   3. Not already handled by pointer-mode array params (stack_array_ids)
     * For such allocas, we skip the alloca decl and store, and make loads
     * reference the parameter value ID directly. */
    {
        for (int pi = 0; pi < fn->param_count; pi++) {
            IronIR_ValueId param_val = (IronIR_ValueId)(pi * 2 + 1);
            IronIR_ValueId alloca_id = (IronIR_ValueId)(pi * 2 + 2);

            /* Skip if this alloca is already tracked as a stack array (pointer-mode param) */
            if (hmgeti(ctx->stack_array_ids, alloca_id) >= 0) continue;

            /* Count stores to this alloca across ALL blocks */
            int store_count = 0;
            for (int bi = 0; bi < fn->block_count; bi++) {
                IronIR_Block *block = fn->blocks[bi];
                for (int ii = 0; ii < block->instr_count; ii++) {
                    IronIR_Instr *instr = block->instrs[ii];
                    if (instr->kind == IRON_IR_STORE &&
                        instr->store.ptr == alloca_id) {
                        store_count++;
                    }
                }
            }

            /* Only alias if stored exactly once (the initial param store) */
            if (store_count == 1) {
                hmput(ctx->param_alias_ids, alloca_id, param_val);
            }
        }
    }

    emit_func_signature(sb, fn, ctx, false);
    iron_strbuf_appendf(sb, " {\n");

    ctx->indent = 1;

    for (int bi = 0; bi < fn->block_count; bi++) {
        IronIR_Block *block = fn->blocks[bi];

        /* Emit block label — unique per block to avoid duplicate-label errors */
        iron_strbuf_appendf(sb, "%s:;\n",
                            make_block_label(block->id, block->label, ctx->arena));

        if (block->instr_count == 0) {
            /* Dead / unreachable block with no instructions.
             * Emit __builtin_unreachable() so C compilers don't warn about
             * missing returns and optimizers can prune the dead path. */
            iron_strbuf_appendf(sb, "    __builtin_unreachable();\n");
        } else {
            for (int ii = 0; ii < block->instr_count; ii++) {
                emit_instr(sb, block->instrs[ii], fn, ctx);
            }
        }
    }

    ctx->indent = 0;
    iron_strbuf_appendf(sb, "}\n\n");
}

/* ── Type declaration emission ────────────────────────────────────────────── */

/* Topological sort for IR type declarations */
#define IR_TOPO_WHITE 0
#define IR_TOPO_GRAY  1
#define IR_TOPO_BLACK 2

typedef struct {
    IronIR_TypeDecl **sorted; /* stb_ds array */
    IronIR_Module    *module;
    int              *colors;
    bool              has_cycle;
} IrTopoState;

/* Find object type_decl index by type name */
static int find_ir_type_decl_idx(IronIR_Module *module, const char *name) {
    for (int i = 0; i < module->type_decl_count; i++) {
        if (module->type_decls[i]->kind == IRON_IR_TYPE_OBJECT &&
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

    IronIR_TypeDecl *td = state->module->type_decls[idx];
    if (td->kind == IRON_IR_TYPE_OBJECT && td->type &&
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
static bool ir_has_subtype(IronIR_Module *module, const char *name) {
    for (int i = 0; i < module->type_decl_count; i++) {
        IronIR_TypeDecl *td = module->type_decls[i];
        if (td->kind != IRON_IR_TYPE_OBJECT) continue;
        if (!td->type || td->type->kind != IRON_TYPE_OBJECT) continue;
        if (!td->type->object.decl) continue;
        if (td->type->object.decl->extends_name &&
            strcmp(td->type->object.decl->extends_name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Map a type annotation name to a C type string without needing Iron_Codegen */
static const char *annotation_to_c(const char *name, EmitCtx *ctx) {
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

static void emit_object_struct_body(EmitCtx *ctx, IronIR_TypeDecl *td,
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
                if (ta->is_nullable) {
                    /* Build Optional type name from annotation */
                    const char *inner_c = annotation_to_c(ta->name, ctx);
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
                    const char *elem_c = annotation_to_c(ta->name, ctx);
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
                    c_type = annotation_to_c(ta->name, ctx);
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

static void emit_type_decls(EmitCtx *ctx) {
    IronIR_Module *module = ctx->module;

    /* Forward declarations for all object and interface types */
    for (int i = 0; i < module->type_decl_count; i++) {
        IronIR_TypeDecl *td = module->type_decls[i];
        if (td->kind == IRON_IR_TYPE_OBJECT ||
            td->kind == IRON_IR_TYPE_INTERFACE) {
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
        if (module->type_decls[i]->kind == IRON_IR_TYPE_OBJECT) obj_count++;
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
            if (module->type_decls[i]->kind == IRON_IR_TYPE_OBJECT &&
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

    /* Interface vtable structs */
    for (int i = 0; i < module->type_decl_count; i++) {
        IronIR_TypeDecl *td = module->type_decls[i];
        if (td->kind != IRON_IR_TYPE_INTERFACE) continue;
        if (!td->type || td->type->kind != IRON_TYPE_INTERFACE) continue;

        Iron_InterfaceDecl *iface = td->type->interface.decl;
        if (!iface) continue;

        const char *iface_mangled = emit_mangle_name(iface->name, ctx->arena);
        Iron_StrBuf *sb = &ctx->struct_bodies;

        iron_strbuf_appendf(sb, "typedef struct %s_vtable {\n", iface_mangled);
        for (int j = 0; j < iface->method_count; j++) {
            Iron_Node *sig_node = iface->method_sigs[j];
            if (!sig_node || sig_node->kind != IRON_NODE_FUNC_DECL) continue;
            Iron_FuncDecl *sig = (Iron_FuncDecl *)sig_node;

            const char *ret_type = "void";
            if (sig->resolved_return_type) {
                ret_type = emit_type_to_c(sig->resolved_return_type, ctx);
            }
            iron_strbuf_appendf(sb, "    %s (*%s)(void* self",
                                ret_type, sig->name);
            for (int k = 0; k < sig->param_count; k++) {
                Iron_Param *p = (Iron_Param *)sig->params[k];
                const char *pt = "void*";
                if (p->type_ann) {
                    Iron_TypeAnnotation *ta = (Iron_TypeAnnotation *)p->type_ann;
                    pt = annotation_to_c(ta->name, ctx);
                }
                iron_strbuf_appendf(sb, ", %s %s", pt, p->name);
            }
            iron_strbuf_appendf(sb, ");\n");
        }
        iron_strbuf_appendf(sb, "} %s_vtable;\n", iface_mangled);
        iron_strbuf_appendf(sb,
            "typedef struct { void *object; %s_vtable *vtable; } %s_ref;\n",
            iface_mangled, iface_mangled);
    }

    /* Enum definitions */
    for (int i = 0; i < module->type_decl_count; i++) {
        IronIR_TypeDecl *td = module->type_decls[i];
        if (td->kind != IRON_IR_TYPE_ENUM) continue;
        if (!td->type || td->type->kind != IRON_TYPE_ENUM) continue;

        Iron_EnumDecl *ed = td->type->enu.decl;
        if (!ed) continue;

        const char *mangled = emit_mangle_name(ed->name, ctx->arena);
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

/* ── Main entry point ─────────────────────────────────────────────────────── */

const char *iron_ir_emit_c(IronIR_Module *module, Iron_Arena *arena,
                            Iron_DiagList *diags) {
    if (!module) return NULL;
    if (diags && diags->error_count > 0) return NULL;

    /* Initialize context */
    EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.arena         = arena;
    ctx.diags         = diags;
    ctx.module        = module;
    ctx.next_type_tag = 1;

    ctx.includes        = iron_strbuf_create(512);
    ctx.forward_decls   = iron_strbuf_create(256);
    ctx.struct_bodies   = iron_strbuf_create(1024);
    ctx.enum_defs       = iron_strbuf_create(256);
    ctx.global_consts   = iron_strbuf_create(64);
    ctx.prototypes      = iron_strbuf_create(512);
    ctx.lifted_funcs    = iron_strbuf_create(1024);
    ctx.implementations = iron_strbuf_create(4096);
    ctx.main_wrapper    = iron_strbuf_create(256);

    ctx.emitted_optionals = NULL;
    ctx.mono_registry     = NULL;
    ctx.indent            = 0;

    /* ── Phase 0: Phi elimination ─────────────────────────────────────────── */
    phi_eliminate(module);

    /* ── Phase 0a: Array parameter mode analysis (PARAM-01/02) ───────────── */
    analyze_array_param_modes(&ctx);

    /* ── Phase 0b: Array representation optimization (ARR-01) ────────────── */
    optimize_array_repr(module, &ctx);

    /* ── Phase 1: Includes ───────────────────────────────────────────────── */
    iron_strbuf_appendf(&ctx.includes,
                         "#include \"runtime/iron_runtime.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdint.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdbool.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdlib.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <string.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include <stdio.h>\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_math.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_io.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#define IRON_TIMER_STRUCT_DEFINED\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_time.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "#include \"stdlib/iron_log.h\"\n");
    iron_strbuf_appendf(&ctx.includes, "\n");

    /* ── Phase 2: Type declarations (forward decls, structs, enums) ───────── */
    emit_type_decls(&ctx);

    /* ── Phase 3: Function prototypes ────────────────────────────────────── */
    for (int i = 0; i < module->func_count; i++) {
        IronIR_Func *fn = module->funcs[i];
        if (fn->is_extern) continue;
        emit_func_signature(&ctx.prototypes, fn, &ctx, true);
    }
    if (ctx.prototypes.len > 0) {
        iron_strbuf_appendf(&ctx.prototypes, "\n");
    }

    /* ── Phase 4: Function bodies ─────────────────────────────────────────── */
    bool has_main = false;
    for (int i = 0; i < module->func_count; i++) {
        IronIR_Func *fn = module->funcs[i];
        if (fn->is_extern) continue;
        /* Accept "main" (from IR lowerer) or "Iron_main" (from unit tests / direct IR) */
        if (strcmp(fn->name, "main") == 0 ||
            strcmp(fn->name, "Iron_main") == 0) {
            has_main = true;
        }
        emit_func_body(&ctx, fn);
    }

    /* ── Phase 5: main() wrapper ──────────────────────────────────────────── */
    if (has_main) {
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "int main(int argc, char** argv) {\n");
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "    (void)argc; (void)argv;\n");
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "    iron_runtime_init();\n");
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "    Iron_main();\n");
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "    iron_runtime_shutdown();\n");
        iron_strbuf_appendf(&ctx.main_wrapper,
                             "    return 0;\n");
        iron_strbuf_appendf(&ctx.main_wrapper, "}\n");
    }

    /* ── Concatenate all sections ─────────────────────────────────────────── */
    Iron_StrBuf output = iron_strbuf_create(8192);

    iron_strbuf_appendf(&output, "/* Generated by Iron compiler (IR backend) */\n\n");
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.includes),
                        ctx.includes.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.forward_decls),
                        ctx.forward_decls.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.struct_bodies),
                        ctx.struct_bodies.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.enum_defs),
                        ctx.enum_defs.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.global_consts),
                        ctx.global_consts.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.prototypes),
                        ctx.prototypes.len);
    if (ctx.lifted_funcs.len > 0) {
        iron_strbuf_append(&output, iron_strbuf_get(&ctx.lifted_funcs),
                            ctx.lifted_funcs.len);
    }
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.implementations),
                        ctx.implementations.len);
    iron_strbuf_append(&output, iron_strbuf_get(&ctx.main_wrapper),
                        ctx.main_wrapper.len);

    /* Arena-dup the final string */
    const char *result = iron_arena_strdup(arena, iron_strbuf_get(&output),
                                            output.len);

    /* Free working buffers */
    iron_strbuf_free(&ctx.includes);
    iron_strbuf_free(&ctx.forward_decls);
    iron_strbuf_free(&ctx.struct_bodies);
    iron_strbuf_free(&ctx.enum_defs);
    iron_strbuf_free(&ctx.global_consts);
    iron_strbuf_free(&ctx.prototypes);
    iron_strbuf_free(&ctx.lifted_funcs);
    iron_strbuf_free(&ctx.implementations);
    iron_strbuf_free(&ctx.main_wrapper);
    iron_strbuf_free(&output);

    arrfree(ctx.emitted_optionals);
    shfree(ctx.mono_registry);
    hmfree(ctx.stack_array_ids);
    hmfree(ctx.heap_array_ids);
    hmfree(ctx.escaped_heap_ids);
    shfree(ctx.array_param_modes);
    hmfree(ctx.revoked_fill_ids);
    hmfree(ctx.param_alias_ids);

    return result;
}
