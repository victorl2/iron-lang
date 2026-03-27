#ifndef IRON_IR_H
#define IRON_IR_H

#include "diagnostics/diagnostics.h"
#include "analyzer/types.h"
#include "util/arena.h"
#include "parser/ast.h"
#include "vendor/stb_ds.h"
#include <stdbool.h>
#include <stdint.h>

/* ── ID types ─────────────────────────────────────────────────────────────── */

typedef uint32_t IronIR_ValueId;
typedef uint32_t IronIR_BlockId;

#define IRON_IR_VALUE_INVALID 0
#define IRON_IR_BLOCK_INVALID 0

/* ── Forward declarations ─────────────────────────────────────────────────── */

typedef struct IronIR_Instr  IronIR_Instr;
typedef struct IronIR_Block  IronIR_Block;
typedef struct IronIR_Func   IronIR_Func;
typedef struct IronIR_Module IronIR_Module;

/* ── Instruction kind enum ────────────────────────────────────────────────── */

typedef enum {
    /* Constants */
    IRON_IR_CONST_INT,
    IRON_IR_CONST_FLOAT,
    IRON_IR_CONST_BOOL,
    IRON_IR_CONST_STRING,
    IRON_IR_CONST_NULL,

    /* Arithmetic */
    IRON_IR_ADD,
    IRON_IR_SUB,
    IRON_IR_MUL,
    IRON_IR_DIV,
    IRON_IR_MOD,

    /* Comparison */
    IRON_IR_EQ,
    IRON_IR_NEQ,
    IRON_IR_LT,
    IRON_IR_LTE,
    IRON_IR_GT,
    IRON_IR_GTE,

    /* Logical */
    IRON_IR_AND,
    IRON_IR_OR,

    /* Unary */
    IRON_IR_NEG,
    IRON_IR_NOT,

    /* Memory */
    IRON_IR_ALLOCA,
    IRON_IR_LOAD,
    IRON_IR_STORE,

    /* Field / Index */
    IRON_IR_GET_FIELD,
    IRON_IR_SET_FIELD,
    IRON_IR_GET_INDEX,
    IRON_IR_SET_INDEX,

    /* Control flow (terminators) */
    IRON_IR_JUMP,
    IRON_IR_BRANCH,
    IRON_IR_SWITCH,
    IRON_IR_RETURN,

    /* High-level */
    IRON_IR_CALL,
    IRON_IR_CAST,
    IRON_IR_CONSTRUCT,
    IRON_IR_ARRAY_LIT,
    IRON_IR_SLICE,
    IRON_IR_IS_NULL,
    IRON_IR_IS_NOT_NULL,
    IRON_IR_INTERP_STRING,

    /* Memory management */
    IRON_IR_HEAP_ALLOC,
    IRON_IR_RC_ALLOC,
    IRON_IR_FREE,

    /* Concurrency */
    IRON_IR_MAKE_CLOSURE,
    IRON_IR_FUNC_REF,
    IRON_IR_SPAWN,
    IRON_IR_PARALLEL_FOR,
    IRON_IR_AWAIT,

    /* SSA */
    IRON_IR_PHI,

    /* Sentinel */
    IRON_IR_INSTR_COUNT
} IronIR_InstrKind;

/* ── Instruction (tagged union) ───────────────────────────────────────────── */

struct IronIR_Instr {
    IronIR_InstrKind   kind;
    IronIR_ValueId     id;       /* 0 = instruction produces no value (store, jump, etc.) */
    Iron_Type         *type;     /* result type; NULL for void-result instructions */
    Iron_Span          span;     /* source location -- carried from AST node */

    union {
        /* IRON_IR_CONST_INT */
        struct { int64_t value; } const_int;
        /* IRON_IR_CONST_FLOAT */
        struct { double value; } const_float;
        /* IRON_IR_CONST_BOOL */
        struct { bool value; } const_bool;
        /* IRON_IR_CONST_STRING */
        struct { const char *value; } const_str;
        /* IRON_IR_CONST_NULL */
        struct { int _pad; } const_null;

        /* IRON_IR_ADD, IRON_IR_SUB, IRON_IR_MUL, IRON_IR_DIV, IRON_IR_MOD */
        /* IRON_IR_EQ, IRON_IR_NEQ, IRON_IR_LT, IRON_IR_LTE, IRON_IR_GT, IRON_IR_GTE */
        /* IRON_IR_AND, IRON_IR_OR */
        struct { IronIR_ValueId left; IronIR_ValueId right; } binop;

        /* IRON_IR_NEG, IRON_IR_NOT */
        struct { IronIR_ValueId operand; } unop;

        /* IRON_IR_ALLOCA */
        struct {
            Iron_Type  *alloc_type;
            const char *name_hint;
        } alloca;

        /* IRON_IR_LOAD */
        struct { IronIR_ValueId ptr; } load;

        /* IRON_IR_STORE */
        struct { IronIR_ValueId ptr; IronIR_ValueId value; } store;

        /* IRON_IR_GET_FIELD, IRON_IR_SET_FIELD */
        struct {
            IronIR_ValueId  object;
            const char     *field;
            IronIR_ValueId  value;   /* SET_FIELD only; ignored for GET_FIELD */
        } field;

        /* IRON_IR_GET_INDEX, IRON_IR_SET_INDEX */
        struct {
            IronIR_ValueId array;
            IronIR_ValueId index;
            IronIR_ValueId value;   /* SET_INDEX only */
        } index;

        /* IRON_IR_CALL */
        struct {
            Iron_FuncDecl  *func_decl;   /* for direct calls; NULL for indirect */
            IronIR_ValueId  func_ptr;    /* for indirect calls (lambda/fn pointer) */
            IronIR_ValueId *args;        /* stb_ds array */
            int             arg_count;
        } call;

        /* IRON_IR_JUMP */
        struct { IronIR_BlockId target; } jump;

        /* IRON_IR_BRANCH */
        struct {
            IronIR_ValueId cond;
            IronIR_BlockId then_block;
            IronIR_BlockId else_block;
        } branch;

        /* IRON_IR_SWITCH */
        struct {
            IronIR_ValueId  subject;
            IronIR_BlockId  default_block;
            int            *case_values;   /* stb_ds array of discriminant ints */
            IronIR_BlockId *case_blocks;   /* stb_ds array, parallel to case_values */
            int             case_count;
        } sw;

        /* IRON_IR_RETURN */
        struct { IronIR_ValueId value; bool is_void; } ret;

        /* IRON_IR_CAST */
        struct { IronIR_ValueId value; Iron_Type *target_type; } cast;

        /* IRON_IR_HEAP_ALLOC */
        struct {
            IronIR_ValueId inner_val;
            bool           auto_free;
            bool           escapes;
        } heap_alloc;

        /* IRON_IR_RC_ALLOC */
        struct { IronIR_ValueId inner_val; } rc_alloc;

        /* IRON_IR_FREE -- named free_instr to avoid conflict with C stdlib free() */
        struct { IronIR_ValueId value; } free_instr;

        /* IRON_IR_CONSTRUCT */
        struct {
            Iron_Type      *type;
            IronIR_ValueId *field_vals;   /* stb_ds array, in declaration order */
            int             field_count;
        } construct;

        /* IRON_IR_ARRAY_LIT */
        struct {
            Iron_Type      *elem_type;
            IronIR_ValueId *elements;    /* stb_ds array */
            int             element_count;
        } array_lit;

        /* IRON_IR_SLICE */
        struct {
            IronIR_ValueId array;
            IronIR_ValueId start;
            IronIR_ValueId end;
        } slice;

        /* IRON_IR_IS_NULL, IRON_IR_IS_NOT_NULL */
        struct { IronIR_ValueId value; } null_check;

        /* IRON_IR_INTERP_STRING */
        struct {
            IronIR_ValueId *parts;   /* stb_ds array */
            int             part_count;
        } interp_string;

        /* IRON_IR_MAKE_CLOSURE */
        struct {
            const char     *lifted_func_name;
            IronIR_ValueId *captures;    /* stb_ds array */
            int             capture_count;
        } make_closure;

        /* IRON_IR_FUNC_REF */
        struct { const char *func_name; } func_ref;

        /* IRON_IR_SPAWN */
        struct {
            const char    *lifted_func_name;
            IronIR_ValueId pool_val;
            const char    *handle_name;
        } spawn;

        /* IRON_IR_PARALLEL_FOR */
        struct {
            const char     *loop_var_name;
            IronIR_ValueId  range_val;
            const char     *chunk_func_name;
            IronIR_ValueId  pool_val;
            IronIR_ValueId *captures;    /* stb_ds array */
            int             capture_count;
        } parallel_for;

        /* IRON_IR_AWAIT */
        struct { IronIR_ValueId handle; } await;

        /* IRON_IR_PHI */
        struct {
            IronIR_ValueId *values;       /* stb_ds array, parallel with pred_blocks */
            IronIR_BlockId *pred_blocks;  /* stb_ds array */
            int             count;
        } phi;
    };
};

/* ── Parameter ────────────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    Iron_Type  *type;
} IronIR_Param;

/* ── Basic block ──────────────────────────────────────────────────────────── */

struct IronIR_Block {
    IronIR_BlockId   id;
    const char      *label;

    /* Instruction list */
    IronIR_Instr   **instrs;       /* stb_ds array */
    int              instr_count;

    /* CFG edges */
    IronIR_BlockId  *preds;        /* stb_ds array */
    IronIR_BlockId  *succs;        /* stb_ds array */

    /* Braun SSA construction state */
    bool             is_sealed;    /* all predecessors known */
    bool             is_filled;    /* block has a terminator */

    /* Current reaching definition for each variable name */
    struct { char *key; IronIR_ValueId value; } *var_defs;       /* stb_ds string map */

    /* Incomplete phi nodes awaiting predecessor resolution */
    struct { char *key; IronIR_Instr *value; } *incomplete_phis; /* stb_ds string map */
};

/* ── Type declarations ────────────────────────────────────────────────────── */

typedef enum {
    IRON_IR_TYPE_OBJECT,
    IRON_IR_TYPE_ENUM,
    IRON_IR_TYPE_INTERFACE
} IronIR_TypeDeclKind;

typedef struct {
    IronIR_TypeDeclKind kind;
    const char         *name;
    Iron_Type          *type;
} IronIR_TypeDecl;

/* ── Extern declaration ───────────────────────────────────────────────────── */

typedef struct {
    const char  *iron_name;
    const char  *c_name;
    Iron_Type  **param_types;
    int          param_count;
    Iron_Type   *return_type;
} IronIR_ExternDecl;

/* ── Function ─────────────────────────────────────────────────────────────── */

struct IronIR_Func {
    const char     *name;
    Iron_Type      *return_type;
    IronIR_Param   *params;
    int             param_count;

    bool            is_extern;
    const char     *extern_c_name;

    IronIR_Block  **blocks;          /* stb_ds array */
    int             block_count;

    IronIR_ValueId  next_value_id;   /* starts at 1 */
    IronIR_BlockId  next_block_id;   /* starts at 1 */

    IronIR_Instr  **value_table;     /* stb_ds array indexed by value id */

    Iron_Arena     *arena;           /* pointer to owning arena */
};

/* ── Module ───────────────────────────────────────────────────────────────── */

struct IronIR_Module {
    const char        *name;

    IronIR_TypeDecl  **type_decls;    /* stb_ds array */
    int                type_decl_count;

    IronIR_ExternDecl **extern_decls; /* stb_ds array */
    int                extern_decl_count;

    IronIR_Func      **funcs;         /* stb_ds array */
    int                func_count;

    /* Monomorphization registry: maps mangled name -> seen */
    struct { char *key; bool value; } *mono_registry; /* stb_ds string map */

    int lambda_counter;
    int spawn_counter;
    int parallel_counter;

    Iron_Arena *arena; /* dedicated ir_arena, separate from ast_arena */
};

/* ── Constructor declarations ─────────────────────────────────────────────── */

IronIR_Module *iron_ir_module_create(Iron_Arena *ir_arena, const char *name);
void           iron_ir_module_destroy(IronIR_Module *mod);

IronIR_Func   *iron_ir_func_create(IronIR_Module *mod, const char *name,
                                    IronIR_Param *params, int param_count,
                                    Iron_Type *return_type);

IronIR_Block  *iron_ir_block_create(IronIR_Func *func, const char *label);

/* Instruction constructors */
IronIR_Instr *iron_ir_const_int(IronIR_Func *fn, IronIR_Block *block,
                                 int64_t value, Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_const_float(IronIR_Func *fn, IronIR_Block *block,
                                   double value, Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_const_bool(IronIR_Func *fn, IronIR_Block *block,
                                  bool value, Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_const_string(IronIR_Func *fn, IronIR_Block *block,
                                    const char *value, Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_const_null(IronIR_Func *fn, IronIR_Block *block,
                                  Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_binop(IronIR_Func *fn, IronIR_Block *block,
                             IronIR_InstrKind kind,
                             IronIR_ValueId left, IronIR_ValueId right,
                             Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_unop(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_InstrKind kind, IronIR_ValueId operand,
                            Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_alloca(IronIR_Func *fn, IronIR_Block *block,
                              Iron_Type *alloc_type, const char *name_hint,
                              Iron_Span span);
IronIR_Instr *iron_ir_load(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_ValueId ptr, Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_store(IronIR_Func *fn, IronIR_Block *block,
                             IronIR_ValueId ptr, IronIR_ValueId value,
                             Iron_Span span);
IronIR_Instr *iron_ir_get_field(IronIR_Func *fn, IronIR_Block *block,
                                 IronIR_ValueId object, const char *field,
                                 Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_set_field(IronIR_Func *fn, IronIR_Block *block,
                                 IronIR_ValueId object, const char *field,
                                 IronIR_ValueId value, Iron_Span span);
IronIR_Instr *iron_ir_get_index(IronIR_Func *fn, IronIR_Block *block,
                                 IronIR_ValueId array, IronIR_ValueId idx,
                                 Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_set_index(IronIR_Func *fn, IronIR_Block *block,
                                 IronIR_ValueId array, IronIR_ValueId idx,
                                 IronIR_ValueId value, Iron_Span span);
IronIR_Instr *iron_ir_call(IronIR_Func *fn, IronIR_Block *block,
                            Iron_FuncDecl *func_decl, IronIR_ValueId func_ptr,
                            IronIR_ValueId *args, int arg_count,
                            Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_jump(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_BlockId target, Iron_Span span);
IronIR_Instr *iron_ir_branch(IronIR_Func *fn, IronIR_Block *block,
                              IronIR_ValueId cond,
                              IronIR_BlockId then_block, IronIR_BlockId else_block,
                              Iron_Span span);
IronIR_Instr *iron_ir_switch(IronIR_Func *fn, IronIR_Block *block,
                              IronIR_ValueId subject, IronIR_BlockId default_block,
                              int *case_values, IronIR_BlockId *case_blocks,
                              int case_count, Iron_Span span);
IronIR_Instr *iron_ir_return(IronIR_Func *fn, IronIR_Block *block,
                              IronIR_ValueId value, bool is_void,
                              Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_cast(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_ValueId value, Iron_Type *target_type,
                            Iron_Span span);
IronIR_Instr *iron_ir_heap_alloc(IronIR_Func *fn, IronIR_Block *block,
                                  IronIR_ValueId inner_val,
                                  bool auto_free, bool escapes,
                                  Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_rc_alloc(IronIR_Func *fn, IronIR_Block *block,
                                IronIR_ValueId inner_val,
                                Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_free(IronIR_Func *fn, IronIR_Block *block,
                            IronIR_ValueId value, Iron_Span span);
IronIR_Instr *iron_ir_construct(IronIR_Func *fn, IronIR_Block *block,
                                 Iron_Type *type,
                                 IronIR_ValueId *field_vals, int field_count,
                                 Iron_Span span);
IronIR_Instr *iron_ir_array_lit(IronIR_Func *fn, IronIR_Block *block,
                                 Iron_Type *elem_type,
                                 IronIR_ValueId *elements, int element_count,
                                 Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_slice(IronIR_Func *fn, IronIR_Block *block,
                             IronIR_ValueId array,
                             IronIR_ValueId start, IronIR_ValueId end,
                             Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_is_null(IronIR_Func *fn, IronIR_Block *block,
                               IronIR_ValueId value, Iron_Span span);
IronIR_Instr *iron_ir_is_not_null(IronIR_Func *fn, IronIR_Block *block,
                                   IronIR_ValueId value, Iron_Span span);
IronIR_Instr *iron_ir_interp_string(IronIR_Func *fn, IronIR_Block *block,
                                     IronIR_ValueId *parts, int part_count,
                                     Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_make_closure(IronIR_Func *fn, IronIR_Block *block,
                                    const char *lifted_func_name,
                                    IronIR_ValueId *captures, int capture_count,
                                    Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_func_ref(IronIR_Func *fn, IronIR_Block *block,
                                const char *func_name,
                                Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_spawn(IronIR_Func *fn, IronIR_Block *block,
                             const char *lifted_func_name,
                             IronIR_ValueId pool_val, const char *handle_name,
                             Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_parallel_for(IronIR_Func *fn, IronIR_Block *block,
                                    const char *loop_var_name,
                                    IronIR_ValueId range_val,
                                    const char *chunk_func_name,
                                    IronIR_ValueId pool_val,
                                    IronIR_ValueId *captures, int capture_count,
                                    Iron_Span span);
IronIR_Instr *iron_ir_await(IronIR_Func *fn, IronIR_Block *block,
                             IronIR_ValueId handle,
                             Iron_Type *type, Iron_Span span);
IronIR_Instr *iron_ir_phi(IronIR_Func *fn, IronIR_Block *block,
                           Iron_Type *type, Iron_Span span);

/* Phi manipulation */
void iron_ir_phi_add_incoming(IronIR_Instr *phi, IronIR_ValueId value,
                               IronIR_BlockId pred_block);

/* Helpers */
bool iron_ir_is_terminator(IronIR_InstrKind kind);

/* Module helpers */
void iron_ir_module_add_type_decl(IronIR_Module *mod, IronIR_TypeDeclKind kind,
                                   const char *name, Iron_Type *type);
void iron_ir_module_add_extern(IronIR_Module *mod,
                                const char *iron_name, const char *c_name,
                                Iron_Type **param_types, int param_count,
                                Iron_Type *return_type);

#endif /* IRON_IR_H */
