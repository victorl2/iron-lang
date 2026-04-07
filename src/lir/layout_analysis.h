/* layout_analysis.h -- Phase 48: Field access analysis for dead field elimination.
 *
 * Scans LIR GET_FIELD/SET_FIELD instructions across all functions to build
 * per-collection used-field sets.  The query function iron_layout_is_field_used()
 * lets the C emitter skip unused fields in split collection storage structs.
 */

#ifndef IRON_LAYOUT_ANALYSIS_H
#define IRON_LAYOUT_ANALYSIS_H

#include "lir.h"
#include "analyzer/iface_collect.h"

/* ── Split collection ID map entry (stb_ds hash map: ValueId -> iface name) */

typedef struct {
    IronLIR_ValueId key;
    const char *value;
} Iron_SplitCollectionId;

/* ── Layout Analysis Context ─────────────────────────────────────────────── */

typedef struct {
    /* stb_ds string-keyed hash map: key = "collectionVid:fieldName" -> used */
    struct { char *key; bool value; } *field_used;
    Iron_Arena *arena;
} LayoutAnalysis;

/* Analyze all functions in the module for field accesses on split collections.
 * Populates la->field_used with entries for every accessed field.
 *
 * @param la                 Layout analysis context (caller-allocated, zeroed)
 * @param module             LIR module to scan
 * @param split_collection_ids  stb_ds hash map: ValueId -> interface name
 * @param iface_reg          Interface registry (for implementor info)
 */
void iron_layout_analyze(LayoutAnalysis *la,
                         IronLIR_Module *module,
                         Iron_SplitCollectionId *split_collection_ids,
                         Iron_IfaceRegistry *iface_reg);

/* Query whether a specific field is used on a given split collection.
 *
 * Returns true  if the field was accessed (GET_FIELD or SET_FIELD).
 * Returns true  if no analysis was performed (conservative fallback).
 * Returns false if analysis ran and the field was NOT accessed (dead field).
 */
bool iron_layout_is_field_used(LayoutAnalysis *la,
                               IronLIR_ValueId collection_vid,
                               const char *field_name);

/* Free internal hash map resources. */
void iron_layout_free(LayoutAnalysis *la);

#endif /* IRON_LAYOUT_ANALYSIS_H */
