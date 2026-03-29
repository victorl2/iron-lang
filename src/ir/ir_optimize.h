#ifndef IRON_IR_OPTIMIZE_H
#define IRON_IR_OPTIMIZE_H

#include "ir/ir.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

/* ── Expression inlining map types ─────────────────────────────────────────── */

/* Named typedefs required for stb_ds hmput with struct map fields (Phase 15-02 pattern).
 * C prohibits assignment between distinct anonymous struct types. */
typedef struct { IronIR_ValueId key; int32_t value; }  IronIR_UseCountEntry;
typedef struct { IronIR_ValueId key; bool value; }     IronIR_InlineEligEntry;
typedef struct { char *key; bool value; }              IronIR_FuncPurityEntry;
typedef struct { IronIR_ValueId key; uint32_t value; } IronIR_ValueBlockEntry;

/* ── Store/load elimination types (Phase 17) ───────────────────────────────── */

/* Maps alloca ValueId -> last-stored ValueId (per-block tracking). */
typedef struct { IronIR_ValueId key; IronIR_ValueId value; } IronIR_StoreTrackEntry;
/* Maps alloca ValueId -> escaped (passed to call/stored into memory/returned). */
typedef struct { IronIR_ValueId key; bool value; }           IronIR_EscapeEntry;

/* ── Array parameter passing mode ──────────────────────────────────────────── */

typedef enum {
    ARRAY_PARAM_LIST,      /* keep as Iron_List_T (default, safe fallback) */
    ARRAY_PARAM_CONST_PTR, /* const T* + len (read-only parameter) */
    ARRAY_PARAM_MUT_PTR    /* T* + len (mutable, no resize) */
} ArrayParamMode;

/* ── Optimization result context ───────────────────────────────────────────── */

/* Data computed by the optimizer that the emitter consumes.
 * All stb_ds maps — caller must free with iron_ir_optimize_info_free(). */
typedef struct {
    struct { IronIR_ValueId key; IronIR_ValueId value; } *stack_array_ids;
    struct { IronIR_ValueId key; IronIR_ValueId value; } *heap_array_ids;
    struct { IronIR_ValueId key; bool value; }           *escaped_heap_ids;
    struct { char *key; int value; }                     *array_param_modes;
    struct { IronIR_ValueId key; bool value; }           *revoked_fill_ids;

    /* Expression inlining analysis — computed by iron_ir_compute_inline_info().
     * use_counts/inline_eligible/value_block are per-function and reset each func in emit_func_body.
     * func_purity is per-module and computed once. */
    IronIR_UseCountEntry   *use_counts;       /* per-function: ValueId -> use count */
    IronIR_InlineEligEntry *inline_eligible;  /* per-function: ValueId -> inlineable */
    IronIR_FuncPurityEntry *func_purity;      /* per-module: func_name -> pure */
    IronIR_ValueBlockEntry *value_block;      /* per-function: ValueId -> defining BlockId */
} IronIR_OptimizeInfo;

/* Run all optimization passes on the module in place.
 * Populates info with data the emitter needs.
 * arena: used for string key allocation in analyze_array_param_modes.
 * dump_passes: print IR after each pass (--dump-ir-passes).
 * skip_new_passes: skip copy-prop/const-fold/DCE (--no-optimize).
 * Returns true if any new optimization pass made changes. */
bool iron_ir_optimize(IronIR_Module *module, IronIR_OptimizeInfo *info,
                      Iron_Arena *arena, bool dump_passes, bool skip_new_passes);

/* Free stb_ds maps inside an OptimizeInfo. */
void iron_ir_optimize_info_free(IronIR_OptimizeInfo *info);

/* Compute module-wide function purity analysis and store on info->func_purity.
 * Must be called after iron_ir_optimize() for all passes to have run. */
void iron_ir_compute_inline_info(IronIR_Module *module, IronIR_OptimizeInfo *info);

/* Per-function analysis helpers — called from emit_func_body in emit_c.c.
 * Each resets and rebuilds the corresponding map on info. */
void iron_ir_compute_use_counts(IronIR_Func *fn, IronIR_OptimizeInfo *info);
void iron_ir_compute_value_block(IronIR_Func *fn, IronIR_OptimizeInfo *info);
void iron_ir_compute_inline_eligible(IronIR_Func *fn, IronIR_OptimizeInfo *info);

/* Returns true if the instruction kind has no observable side effects. */
bool iron_ir_instr_is_pure(IronIR_InstrKind kind);

/* Look up the array parameter mode. Non-static so emit_c.c can call it.
 * Note: stb_ds shgeti modifies the map header internally, so info cannot be const. */
ArrayParamMode iron_ir_get_array_param_mode(IronIR_OptimizeInfo *info,
                                             const char *func_name,
                                             int param_index,
                                             Iron_Arena *arena);

#endif /* IRON_IR_OPTIMIZE_H */
