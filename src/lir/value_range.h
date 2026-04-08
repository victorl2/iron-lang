/* value_range.h -- Phase 50: Whole-program value range analysis for field compression.
 *
 * Scans LIR CONST_INT, CONSTRUCT, SET_FIELD, ADD, SUB, MUL, PHI instructions
 * across all functions to compute per-field value ranges.  When a field is proven
 * to fit in a narrower C type (e.g., [0,7] -> uint8_t), the emitter can compress
 * collection storage structs to reduce memory footprint.
 *
 * Conservative: any unknown path (CALL, untracked LOAD, etc.) produces TOP,
 * meaning no compression.  Only collection storage structs (Iron_<Type>_Stor)
 * are affected -- standalone object structs keep their original types.
 */

#ifndef IRON_VALUE_RANGE_H
#define IRON_VALUE_RANGE_H

#include "lir.h"
#include "analyzer/iface_collect.h"

/* ── Value range representation ─────────────────────────────────────────── */

typedef struct {
    int64_t min;
    int64_t max;
    bool is_top;  /* true = unknown range, no compression possible */
} ValueRange;

/* ── Value Range Analysis Context ───────────────────────────────────────── */

typedef struct {
    /* Per-field ranges: key = "type_name:field_name" -> ValueRange */
    struct { char *key; ValueRange value; } *field_ranges;
    Iron_Arena *arena;
} ValueRangeAnalysis;

/* Analyze all functions in the module for value ranges flowing into
 * collection storage fields.  Populates vra->field_ranges.
 *
 * @param vra       Value range analysis context (caller-allocated, zeroed)
 * @param module    LIR module to scan
 * @param iface_reg Interface registry (for implementor type info)
 */
void iron_vr_analyze(ValueRangeAnalysis *vra,
                     IronLIR_Module *module,
                     Iron_IfaceRegistry *iface_reg);

/* Query: returns narrower C type string if field can be compressed, or NULL.
 * e.g., for range [0, 255] returns "uint8_t", for [-100, 100] returns "int8_t".
 * Returns NULL when range is TOP, absent, or already full-width (no savings). */
const char *iron_vr_get_narrowed_type(ValueRangeAnalysis *vra,
                                       const char *type_name,
                                       const char *field_name);

/* Free internal hash map resources. */
void iron_vr_free(ValueRangeAnalysis *vra);

#endif /* IRON_VALUE_RANGE_H */
