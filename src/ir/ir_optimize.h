#ifndef IRON_IR_OPTIMIZE_H
#define IRON_IR_OPTIMIZE_H

#include "ir/ir.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

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

/* Returns true if the instruction kind has no observable side effects. */
bool iron_ir_instr_is_pure(IronIR_InstrKind kind);

/* Look up the array parameter mode. Non-static so emit_c.c can call it.
 * Note: stb_ds shgeti modifies the map header internally, so info cannot be const. */
ArrayParamMode iron_ir_get_array_param_mode(IronIR_OptimizeInfo *info,
                                             const char *func_name,
                                             int param_index,
                                             Iron_Arena *arena);

#endif /* IRON_IR_OPTIMIZE_H */
