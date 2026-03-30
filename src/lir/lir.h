#ifndef IRON_LIR_H
#define IRON_LIR_H

#include "diagnostics/diagnostics.h"
#include "analyzer/types.h"
#include "util/arena.h"
#include "parser/ast.h"
#include "vendor/stb_ds.h"
#include <stdbool.h>
#include <stdint.h>

/* ── ID types ─────────────────────────────────────────────────────────────── */

typedef uint32_t IronLIR_ValueId;
typedef uint32_t IronLIR_BlockId;

#define IRON_LIR_VALUE_INVALID 0
#define IRON_LIR_BLOCK_INVALID 0

/* ── Forward declarations ─────────────────────────────────────────────────── */

typedef struct IronLIR_Instr  IronLIR_Instr;
typedef struct IronLIR_Block  IronLIR_Block;
typedef struct IronLIR_Func   IronLIR_Func;
typedef struct IronLIR_Module IronLIR_Module;

/* ── Instruction kind enum ────────────────────────────────────────────────── */

typedef enum {
    /* Constants */
    IRON_LIR_CONST_INT,
    IRON_LIR_CONST_FLOAT,
    IRON_LIR_CONST_BOOL,
    IRON_LIR_CONST_STRING,
    IRON_LIR_CONST_NULL,

    /* Arithmetic */
    IRON_LIR_ADD,
    IRON_LIR_SUB,
    IRON_LIR_MUL,
    IRON_LIR_DIV,
    IRON_LIR_MOD,

    /* Comparison */
    IRON_LIR_EQ,
    IRON_LIR_NEQ,
    IRON_LIR_LT,
    IRON_LIR_LTE,
    IRON_LIR_GT,
    IRON_LIR_GTE,

    /* Logical */
    IRON_LIR_AND,
    IRON_LIR_OR,

    /* Unary */
    IRON_LIR_NEG,
    IRON_LIR_NOT,

    /* Memory */
    IRON_LIR_ALLOCA,
    IRON_LIR_LOAD,
    IRON_LIR_STORE,

    /* Field / Index */
    IRON_LIR_GET_FIELD,
    IRON_LIR_SET_FIELD,
    IRON_LIR_GET_INDEX,
    IRON_LIR_SET_INDEX,

    /* Control flow (terminators) */
    IRON_LIR_JUMP,
    IRON_LIR_BRANCH,
    IRON_LIR_SWITCH,
    IRON_LIR_RETURN,

    /* High-level */
    IRON_LIR_CALL,
    IRON_LIR_CAST,
    IRON_LIR_CONSTRUCT,
    IRON_LIR_ARRAY_LIT,
    IRON_LIR_SLICE,
    IRON_LIR_IS_NULL,
    IRON_LIR_IS_NOT_NULL,
    IRON_LIR_INTERP_STRING,

    /* Memory management */
    IRON_LIR_HEAP_ALLOC,
    IRON_LIR_RC_ALLOC,
    IRON_LIR_FREE,

    /* Concurrency */
    IRON_LIR_MAKE_CLOSURE,
    IRON_LIR_FUNC_REF,
    IRON_LIR_SPAWN,
    IRON_LIR_PARALLEL_FOR,
    IRON_LIR_AWAIT,

    /* SSA */
    IRON_LIR_PHI,

    /* Error placeholder */
    IRON_LIR_POISON,

    /* Sentinel */
    IRON_LIR_INSTR_COUNT
} IronLIR_InstrKind;

/* ── Instruction (tagged union) ───────────────────────────────────────────── */

struct IronLIR_Instr {
    IronLIR_InstrKind   kind;
    IronLIR_ValueId     id;       /* 0 = instruction produces no value (store, jump, etc.) */
    Iron_Type         *type;     /* result type; NULL for void-result instructions */
    Iron_Span          span;     /* source location -- carried from AST node */

    union {
        /* IRON_LIR_CONST_INT */
        struct { int64_t value; } const_int;
        /* IRON_LIR_CONST_FLOAT */
        struct { double value; } const_float;
        /* IRON_LIR_CONST_BOOL */
        struct { bool value; } const_bool;
        /* IRON_LIR_CONST_STRING */
        struct { const char *value; } const_str;
        /* IRON_LIR_CONST_NULL */
        struct { int _pad; } const_null;

        /* IRON_LIR_ADD, IRON_LIR_SUB, IRON_LIR_MUL, IRON_LIR_DIV, IRON_LIR_MOD */
        /* IRON_LIR_EQ, IRON_LIR_NEQ, IRON_LIR_LT, IRON_LIR_LTE, IRON_LIR_GT, IRON_LIR_GTE */
        /* IRON_LIR_AND, IRON_LIR_OR */
        struct { IronLIR_ValueId left; IronLIR_ValueId right; } binop;

        /* IRON_LIR_NEG, IRON_LIR_NOT */
        struct { IronLIR_ValueId operand; } unop;

        /* IRON_LIR_ALLOCA */
        struct {
            Iron_Type  *alloc_type;
            const char *name_hint;
        } alloca;

        /* IRON_LIR_LOAD */
        struct { IronLIR_ValueId ptr; } load;

        /* IRON_LIR_STORE */
        struct { IronLIR_ValueId ptr; IronLIR_ValueId value; } store;

        /* IRON_LIR_GET_FIELD, IRON_LIR_SET_FIELD */
        struct {
            IronLIR_ValueId  object;
            const char     *field;
            IronLIR_ValueId  value;   /* SET_FIELD only; ignored for GET_FIELD */
        } field;

        /* IRON_LIR_GET_INDEX, IRON_LIR_SET_INDEX */
        struct {
            IronLIR_ValueId array;
            IronLIR_ValueId index;
            IronLIR_ValueId value;   /* SET_INDEX only */
        } index;

        /* IRON_LIR_CALL */
        struct {
            Iron_FuncDecl  *func_decl;   /* for direct calls; NULL for indirect */
            IronLIR_ValueId  func_ptr;    /* for indirect calls (lambda/fn pointer) */
            IronLIR_ValueId *args;        /* stb_ds array */
            int             arg_count;
        } call;

        /* IRON_LIR_JUMP */
        struct { IronLIR_BlockId target; } jump;

        /* IRON_LIR_BRANCH */
        struct {
            IronLIR_ValueId cond;
            IronLIR_BlockId then_block;
            IronLIR_BlockId else_block;
        } branch;

        /* IRON_LIR_SWITCH */
        struct {
            IronLIR_ValueId  subject;
            IronLIR_BlockId  default_block;
            int            *case_values;   /* stb_ds array of discriminant ints */
            IronLIR_BlockId *case_blocks;   /* stb_ds array, parallel to case_values */
            int             case_count;
        } sw;

        /* IRON_LIR_RETURN */
        struct { IronLIR_ValueId value; bool is_void; } ret;

        /* IRON_LIR_CAST */
        struct { IronLIR_ValueId value; Iron_Type *target_type; } cast;

        /* IRON_LIR_HEAP_ALLOC */
        struct {
            IronLIR_ValueId inner_val;
            bool           auto_free;
            bool           escapes;
        } heap_alloc;

        /* IRON_LIR_RC_ALLOC */
        struct { IronLIR_ValueId inner_val; } rc_alloc;

        /* IRON_LIR_FREE -- named free_instr to avoid conflict with C stdlib free() */
        struct { IronLIR_ValueId value; } free_instr;

        /* IRON_LIR_CONSTRUCT */
        struct {
            Iron_Type      *type;
            IronLIR_ValueId *field_vals;   /* stb_ds array, in declaration order */
            int             field_count;
        } construct;

        /* IRON_LIR_ARRAY_LIT */
        struct {
            Iron_Type      *elem_type;
            IronLIR_ValueId *elements;    /* stb_ds array */
            int             element_count;
            bool            use_stack_repr;  /* emit as C stack array instead of Iron_List_T */
        } array_lit;

        /* IRON_LIR_SLICE */
        struct {
            IronLIR_ValueId array;
            IronLIR_ValueId start;
            IronLIR_ValueId end;
        } slice;

        /* IRON_LIR_IS_NULL, IRON_LIR_IS_NOT_NULL */
        struct { IronLIR_ValueId value; } null_check;

        /* IRON_LIR_INTERP_STRING */
        struct {
            IronLIR_ValueId *parts;   /* stb_ds array */
            int             part_count;
        } interp_string;

        /* IRON_LIR_MAKE_CLOSURE */
        struct {
            const char     *lifted_func_name;
            IronLIR_ValueId *captures;    /* stb_ds array */
            int             capture_count;
        } make_closure;

        /* IRON_LIR_FUNC_REF */
        struct { const char *func_name; } func_ref;

        /* IRON_LIR_SPAWN */
        struct {
            const char    *lifted_func_name;
            IronLIR_ValueId pool_val;
            const char    *handle_name;
        } spawn;

        /* IRON_LIR_PARALLEL_FOR */
        struct {
            const char     *loop_var_name;
            IronLIR_ValueId  range_val;
            const char     *chunk_func_name;
            IronLIR_ValueId  pool_val;
            IronLIR_ValueId *captures;    /* stb_ds array */
            int             capture_count;
        } parallel_for;

        /* IRON_LIR_AWAIT */
        struct { IronLIR_ValueId handle; } await;

        /* IRON_LIR_PHI */
        struct {
            IronLIR_ValueId *values;       /* stb_ds array, parallel with pred_blocks */
            IronLIR_BlockId *pred_blocks;  /* stb_ds array */
            int             count;
        } phi;

        /* IRON_LIR_POISON */
        struct { int _pad; } poison;
    };
};

/* ── Parameter ────────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    Iron_Type  *type;
} IronLIR_Param;

/* ── Basic block ──────────────────────────────────────────────────────────── */

struct IronLIR_Block {
    IronLIR_BlockId   id;
    const char      *label;

    /* Instruction list */
    IronLIR_Instr   **instrs;       /* stb_ds array */
    int              instr_count;

    /* CFG edges */
    IronLIR_BlockId  *preds;        /* stb_ds array */
    IronLIR_BlockId  *succs;        /* stb_ds array */

    /* Braun SSA construction state */
    bool             is_sealed;    /* all predecessors known */
    bool             is_filled;    /* block has a terminator */

    /* Current reaching definition for each variable name */
    struct { char *key; IronLIR_ValueId value; } *var_defs;       /* stb_ds string map */

    /* Incomplete phi nodes awaiting predecessor resolution */
    struct { char *key; IronLIR_Instr *value; } *incomplete_phis; /* stb_ds string map */
};

/* ── Type declarations ────────────────────────────────────────────────────── */

typedef enum {
    IRON_LIR_TYPE_OBJECT,
    IRON_LIR_TYPE_ENUM,
    IRON_LIR_TYPE_INTERFACE
} IronLIR_TypeDeclKind;

typedef struct {
    IronLIR_TypeDeclKind kind;
    const char         *name;
    Iron_Type          *type;
} IronLIR_TypeDecl;

/* ── Extern declaration ───────────────────────────────────────────────────── */

typedef struct {
    const char  *iron_name;
    const char  *c_name;
    Iron_Type  **param_types;
    int          param_count;
    Iron_Type   *return_type;
} IronLIR_ExternDecl;

/* ── Function ─────────────────────────────────────────────────────────────── */

struct IronLIR_Func {
    const char     *name;
    Iron_Type      *return_type;
    IronLIR_Param   *params;
    int             param_count;

    bool            is_extern;
    const char     *extern_c_name;
    bool            ssa_done;    /* true if ssa_construct_func already ran */

    IronLIR_Block  **blocks;          /* stb_ds array */
    int             block_count;

    IronLIR_ValueId  next_value_id;   /* starts at 1 */
    IronLIR_BlockId  next_block_id;   /* starts at 1 */

    IronLIR_Instr  **value_table;     /* stb_ds array indexed by value id */

    Iron_Arena     *arena;           /* pointer to owning arena */
};

/* ── Module ───────────────────────────────────────────────────────────────── */

struct IronLIR_Module {
    const char        *name;

    IronLIR_TypeDecl  **type_decls;    /* stb_ds array */
    int                type_decl_count;

    IronLIR_ExternDecl **extern_decls; /* stb_ds array */
    int                extern_decl_count;

    IronLIR_Func      **funcs;         /* stb_ds array */
    int                func_count;

    /* Monomorphization registry: maps mangled name -> seen */
    struct { char *key; bool value; } *mono_registry; /* stb_ds string map */

    int lambda_counter;
    int spawn_counter;
    int parallel_counter;

    Iron_Arena *arena; /* dedicated ir_arena, separate from ast_arena */
};

/* ── Constructor declarations ─────────────────────────────────────────────── */

IronLIR_Module *iron_lir_module_create(Iron_Arena *ir_arena, const char *name);
void           iron_lir_module_destroy(IronLIR_Module *mod);

IronLIR_Func   *iron_lir_func_create(IronLIR_Module *mod, const char *name,
                                    IronLIR_Param *params, int param_count,
                                    Iron_Type *return_type);

IronLIR_Block  *iron_lir_block_create(IronLIR_Func *func, const char *label);

/* Instruction constructors */
IronLIR_Instr *iron_lir_const_int(IronLIR_Func *fn, IronLIR_Block *block,
                                 int64_t value, Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_const_float(IronLIR_Func *fn, IronLIR_Block *block,
                                   double value, Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_const_bool(IronLIR_Func *fn, IronLIR_Block *block,
                                  bool value, Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_const_string(IronLIR_Func *fn, IronLIR_Block *block,
                                    const char *value, Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_const_null(IronLIR_Func *fn, IronLIR_Block *block,
                                  Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_binop(IronLIR_Func *fn, IronLIR_Block *block,
                             IronLIR_InstrKind kind,
                             IronLIR_ValueId left, IronLIR_ValueId right,
                             Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_unop(IronLIR_Func *fn, IronLIR_Block *block,
                            IronLIR_InstrKind kind, IronLIR_ValueId operand,
                            Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_alloca(IronLIR_Func *fn, IronLIR_Block *block,
                              Iron_Type *alloc_type, const char *name_hint,
                              Iron_Span span);
IronLIR_Instr *iron_lir_load(IronLIR_Func *fn, IronLIR_Block *block,
                            IronLIR_ValueId ptr, Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_store(IronLIR_Func *fn, IronLIR_Block *block,
                             IronLIR_ValueId ptr, IronLIR_ValueId value,
                             Iron_Span span);
IronLIR_Instr *iron_lir_get_field(IronLIR_Func *fn, IronLIR_Block *block,
                                 IronLIR_ValueId object, const char *field,
                                 Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_set_field(IronLIR_Func *fn, IronLIR_Block *block,
                                 IronLIR_ValueId object, const char *field,
                                 IronLIR_ValueId value, Iron_Span span);
IronLIR_Instr *iron_lir_get_index(IronLIR_Func *fn, IronLIR_Block *block,
                                 IronLIR_ValueId array, IronLIR_ValueId idx,
                                 Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_set_index(IronLIR_Func *fn, IronLIR_Block *block,
                                 IronLIR_ValueId array, IronLIR_ValueId idx,
                                 IronLIR_ValueId value, Iron_Span span);
IronLIR_Instr *iron_lir_call(IronLIR_Func *fn, IronLIR_Block *block,
                            Iron_FuncDecl *func_decl, IronLIR_ValueId func_ptr,
                            IronLIR_ValueId *args, int arg_count,
                            Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_jump(IronLIR_Func *fn, IronLIR_Block *block,
                            IronLIR_BlockId target, Iron_Span span);
IronLIR_Instr *iron_lir_branch(IronLIR_Func *fn, IronLIR_Block *block,
                              IronLIR_ValueId cond,
                              IronLIR_BlockId then_block, IronLIR_BlockId else_block,
                              Iron_Span span);
IronLIR_Instr *iron_lir_switch(IronLIR_Func *fn, IronLIR_Block *block,
                              IronLIR_ValueId subject, IronLIR_BlockId default_block,
                              int *case_values, IronLIR_BlockId *case_blocks,
                              int case_count, Iron_Span span);
IronLIR_Instr *iron_lir_return(IronLIR_Func *fn, IronLIR_Block *block,
                              IronLIR_ValueId value, bool is_void,
                              Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_cast(IronLIR_Func *fn, IronLIR_Block *block,
                            IronLIR_ValueId value, Iron_Type *target_type,
                            Iron_Span span);
IronLIR_Instr *iron_lir_heap_alloc(IronLIR_Func *fn, IronLIR_Block *block,
                                  IronLIR_ValueId inner_val,
                                  bool auto_free, bool escapes,
                                  Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_rc_alloc(IronLIR_Func *fn, IronLIR_Block *block,
                                IronLIR_ValueId inner_val,
                                Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_free(IronLIR_Func *fn, IronLIR_Block *block,
                            IronLIR_ValueId value, Iron_Span span);
IronLIR_Instr *iron_lir_construct(IronLIR_Func *fn, IronLIR_Block *block,
                                 Iron_Type *type,
                                 IronLIR_ValueId *field_vals, int field_count,
                                 Iron_Span span);
IronLIR_Instr *iron_lir_array_lit(IronLIR_Func *fn, IronLIR_Block *block,
                                 Iron_Type *elem_type,
                                 IronLIR_ValueId *elements, int element_count,
                                 Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_slice(IronLIR_Func *fn, IronLIR_Block *block,
                             IronLIR_ValueId array,
                             IronLIR_ValueId start, IronLIR_ValueId end,
                             Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_is_null(IronLIR_Func *fn, IronLIR_Block *block,
                               IronLIR_ValueId value, Iron_Span span);
IronLIR_Instr *iron_lir_is_not_null(IronLIR_Func *fn, IronLIR_Block *block,
                                   IronLIR_ValueId value, Iron_Span span);
IronLIR_Instr *iron_lir_interp_string(IronLIR_Func *fn, IronLIR_Block *block,
                                     IronLIR_ValueId *parts, int part_count,
                                     Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_make_closure(IronLIR_Func *fn, IronLIR_Block *block,
                                    const char *lifted_func_name,
                                    IronLIR_ValueId *captures, int capture_count,
                                    Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_func_ref(IronLIR_Func *fn, IronLIR_Block *block,
                                const char *func_name,
                                Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_spawn(IronLIR_Func *fn, IronLIR_Block *block,
                             const char *lifted_func_name,
                             IronLIR_ValueId pool_val, const char *handle_name,
                             Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_parallel_for(IronLIR_Func *fn, IronLIR_Block *block,
                                    const char *loop_var_name,
                                    IronLIR_ValueId range_val,
                                    const char *chunk_func_name,
                                    IronLIR_ValueId pool_val,
                                    IronLIR_ValueId *captures, int capture_count,
                                    Iron_Span span);
IronLIR_Instr *iron_lir_await(IronLIR_Func *fn, IronLIR_Block *block,
                             IronLIR_ValueId handle,
                             Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_phi(IronLIR_Func *fn, IronLIR_Block *block,
                           Iron_Type *type, Iron_Span span);
IronLIR_Instr *iron_lir_poison(IronLIR_Func *fn, IronLIR_Block *block,
                              Iron_Type *type, Iron_Span span);

/* Phi manipulation */
void iron_lir_phi_add_incoming(IronLIR_Instr *phi, IronLIR_ValueId value,
                               IronLIR_BlockId pred_block);

/* Helpers */
bool iron_lir_is_terminator(IronLIR_InstrKind kind);

/* Module helpers */
void iron_lir_module_add_type_decl(IronLIR_Module *mod, IronLIR_TypeDeclKind kind,
                                   const char *name, Iron_Type *type);
void iron_lir_module_add_extern(IronLIR_Module *mod,
                                const char *iron_name, const char *c_name,
                                Iron_Type **param_types, int param_count,
                                Iron_Type *return_type);

#endif /* IRON_LIR_H */
