#ifndef IRON_LIR_OPTIMIZE_H
#define IRON_LIR_OPTIMIZE_H

#include "lir/lir.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

/* ── Expression inlining map types ─────────────────────────────────────────── */

/* Named typedefs required for stb_ds hmput with struct map fields (Phase 15-02 pattern).
 * C prohibits assignment between distinct anonymous struct types. */
typedef struct { IronLIR_ValueId key; int32_t value; }  IronLIR_UseCountEntry;
typedef struct { IronLIR_ValueId key; bool value; }     IronLIR_InlineEligEntry;
typedef struct { char *key; bool value; }              IronLIR_FuncPurityEntry;
typedef struct { IronLIR_ValueId key; uint32_t value; } IronLIR_ValueBlockEntry;

/* ── Store/load elimination types (Phase 17) ───────────────────────────────── */

/* Maps alloca ValueId -> last-stored ValueId (per-block tracking). */
typedef struct { IronLIR_ValueId key; IronLIR_ValueId value; } IronLIR_StoreTrackEntry;
/* Maps alloca ValueId -> escaped (passed to call/stored into memory/returned). */
typedef struct { IronLIR_ValueId key; bool value; }           IronLIR_EscapeEntry;

/* ── Strength reduction types (Phase 17-02) ────────────────────────────────── */

/* Dominator tree entry: block_id -> immediate dominator block_id */
typedef struct { IronLIR_BlockId key; IronLIR_BlockId value; } IronLIR_DomEntry;

/* Loop membership entry: block_id -> true if block is in a loop body */
typedef struct { IronLIR_BlockId key; bool value; } IronLIR_LoopMemberEntry;

/* Natural loop info */
typedef struct IronLIR_LoopInfo {
    IronLIR_BlockId          header;         /* loop header block */
    IronLIR_BlockId          latch;          /* back-edge source block */
    IronLIR_BlockId          preheader;      /* block that jumps to header from outside loop; 0 if none */
    IronLIR_LoopMemberEntry *body_blocks;    /* stb_ds hashmap: block_id -> true */
    IronLIR_ValueId          indvar_alloca;  /* induction variable alloca ID */
    IronLIR_ValueId          indvar_step;    /* step value ID (usually CONST_INT 1) */
    IronLIR_ValueId          indvar_init;    /* initial value ID (stored before loop) */
    struct IronLIR_LoopInfo *parent;         /* enclosing loop (NULL for outermost) */
} IronLIR_LoopInfo;

/* ── Array parameter passing mode ──────────────────────────────────────────── */

typedef enum {
    ARRAY_PARAM_LIST,      /* keep as Iron_List_T (default, safe fallback) */
    ARRAY_PARAM_CONST_PTR, /* const T* + len (read-only parameter) */
    ARRAY_PARAM_MUT_PTR    /* T* + len (mutable, no resize) */
} ArrayParamMode;

/* ── Optimization result context ───────────────────────────────────────────── */

/* Data computed by the optimizer that the emitter consumes.
 * All stb_ds maps — caller must free with iron_lir_optimize_info_free(). */
typedef struct {
    struct { IronLIR_ValueId key; IronLIR_ValueId value; } *stack_array_ids;
    struct { IronLIR_ValueId key; IronLIR_ValueId value; } *heap_array_ids;
    struct { IronLIR_ValueId key; bool value; }           *escaped_heap_ids;
    struct { char *key; int value; }                     *array_param_modes;
    struct { IronLIR_ValueId key; bool value; }           *revoked_fill_ids;

    /* Expression inlining analysis — computed by iron_lir_compute_inline_info().
     * use_counts/inline_eligible/value_block are per-function and reset each func in emit_func_body.
     * func_purity is per-module and computed once. */
    IronLIR_UseCountEntry   *use_counts;       /* per-function: ValueId -> use count */
    IronLIR_InlineEligEntry *inline_eligible;  /* per-function: ValueId -> inlineable */
    IronLIR_FuncPurityEntry *func_purity;      /* per-module: func_name -> pure */
    IronLIR_ValueBlockEntry *value_block;      /* per-function: ValueId -> defining BlockId */
} IronLIR_OptimizeInfo;

/* Run all optimization passes on the module in place.
 * Populates info with data the emitter needs.
 * arena: used for string key allocation in analyze_array_param_modes.
 * dump_passes: print IR after each pass (--dump-ir-passes).
 * skip_new_passes: skip copy-prop/const-fold/DCE (--no-optimize).
 * Returns true if any new optimization pass made changes. */
bool iron_lir_optimize(IronLIR_Module *module, IronLIR_OptimizeInfo *info,
                      Iron_Arena *arena, bool dump_passes, bool skip_new_passes);

/* Free stb_ds maps inside an OptimizeInfo. */
void iron_lir_optimize_info_free(IronLIR_OptimizeInfo *info);

/* Compute module-wide function purity analysis and store on info->func_purity.
 * Must be called after iron_lir_optimize() for all passes to have run. */
void iron_lir_compute_inline_info(IronLIR_Module *module, IronLIR_OptimizeInfo *info);

/* Per-function analysis helpers — called from emit_func_body in emit_c.c.
 * Each resets and rebuilds the corresponding map on info. */
void iron_lir_compute_use_counts(IronLIR_Func *fn, IronLIR_OptimizeInfo *info);
void iron_lir_compute_value_block(IronLIR_Func *fn, IronLIR_OptimizeInfo *info);
void iron_lir_compute_inline_eligible(IronLIR_Func *fn, IronLIR_OptimizeInfo *info);

/* Returns true if the instruction kind has no observable side effects. */
bool iron_lir_instr_is_pure(IronLIR_InstrKind kind);

/* Look up the array parameter mode. Non-static so emit_c.c can call it.
 * Note: stb_ds shgeti modifies the map header internally, so info cannot be const. */
ArrayParamMode iron_lir_get_array_param_mode(IronLIR_OptimizeInfo *info,
                                             const char *func_name,
                                             int param_index,
                                             Iron_Arena *arena);

#endif /* IRON_LIR_OPTIMIZE_H */
