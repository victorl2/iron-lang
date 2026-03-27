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
} EmitCtx;

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

            /* 3. Replace phi with a LOAD from the alloca */
            phi->kind     = IRON_IR_LOAD;
            phi->load.ptr = alloca_id;
            /* phi->id and phi->type remain the same — the load produces the
             * same ValueId so all existing uses are automatically valid */
        }

        arrfree(phis);
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
        /* Declare a C variable of the alloc_type */
        const char *c_type = emit_type_to_c(instr->alloca.alloc_type, ctx);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", c_type);
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, ";\n");
        break;
    }

    case IRON_IR_LOAD:
        /* Load from the alloca variable — just copy it */
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "%s ", emit_type_to_c(instr->type, ctx));
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->load.ptr);
        iron_strbuf_appendf(sb, ";\n");
        break;

    case IRON_IR_STORE:
        /* Write into alloca variable: ptr = value */
        emit_indent(sb, ind);
        emit_val(sb, instr->store.ptr);
        iron_strbuf_appendf(sb, " = ");
        emit_val(sb, instr->store.value);
        iron_strbuf_appendf(sb, ";\n");
        break;

    /* ── Field / Index ──────────────────────────────────────────────────── */

    case IRON_IR_GET_FIELD: {
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
        break;
    }

    case IRON_IR_SET_INDEX: {
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
        break;
    }

    /* ── Call ───────────────────────────────────────────────────────────── */

    case IRON_IR_CALL: {
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

        for (int i = 0; i < instr->call.arg_count; i++) {
            if (i > 0) iron_strbuf_appendf(sb, ", ");
            IronIR_ValueId arg_id = instr->call.args[i];
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

    case IRON_IR_RETURN:
        emit_indent(sb, ind);
        if (instr->ret.is_void) {
            iron_strbuf_appendf(sb, "return;\n");
        } else {
            iron_strbuf_appendf(sb, "return ");
            emit_val(sb, instr->ret.value);
            iron_strbuf_appendf(sb, ";\n");
        }
        break;

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
        const char *c_name = resolve_func_c_name(ctx, instr->func_ref.func_name);
        emit_indent(sb, ind);
        iron_strbuf_appendf(sb, "void* ");
        emit_val(sb, instr->id);
        iron_strbuf_appendf(sb, " = (void*)%s;\n", c_name);
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
        if (pt) {
            iron_strbuf_appendf(sb, "%s _v%d",
                                emit_type_to_c(pt, ctx), param_val_id);
        } else {
            /* Unknown type — use void* */
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

    return result;
}
