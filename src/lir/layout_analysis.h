/* layout_analysis.h -- Phase 48: Field access analysis for layout optimization.
 *
 * Scans LIR GET_FIELD/SET_FIELD instructions across all functions to build
 * per-collection used-field sets.  The query function iron_layout_is_field_used()
 * lets the C emitter skip unused fields in split collection storage structs.
 *
 * Phase 48 Plan 02: SoA/AoS layout selection based on field access ratios
 * and common field factoring across interface implementors.
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

/* ── SoA/AoS layout selection ────────────────────────────────────────────── */

#define IRON_SOA_THRESHOLD 0.5  /* SoA when accessed_fields / total_fields < this */

typedef enum {
    IRON_LAYOUT_AOS,  /* Array of Structs (default) */
    IRON_LAYOUT_SOA   /* Structure of Arrays */
} IronLayoutKind;

/* Common field entry: field shared across all alive implementors */
typedef struct {
    const char *name;
    const char *c_type;   /* C type string for emission */
    int position;         /* field index in original struct */
} CommonField;

/* ── Layout Analysis Context ─────────────────────────────────────────────── */

typedef struct {
    /* stb_ds string-keyed hash map: key = "collectionVid:fieldName" -> used */
    struct { char *key; bool value; } *field_used;

    /* Phase 48-02: SoA/AoS layout decisions per (iface_mangled, type_name) */
    struct { char *key; IronLayoutKind value; } *layout_decisions;

    /* Phase 48-02: Common fields per interface name -> stb_ds array of CommonField */
    struct { char *key; CommonField *value; } *common_fields;

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

/* Analyze per-loop access patterns and determine SoA vs AoS per (interface, type).
 * Must be called AFTER iron_layout_analyze() has populated field_used data.
 * Populates la->layout_decisions and la->common_fields.
 *
 * Strategy: For each split-eligible for-loop over a split collection, count how
 * many fields of each implementor type are accessed in the loop body.  If the
 * access ratio (accessed / total) is below IRON_SOA_THRESHOLD, select SoA.
 * The lowest access ratio across all loops drives the decision (most SoA-friendly).
 */
void iron_layout_select(LayoutAnalysis *la,
                        IronLIR_Module *module,
                        Iron_SplitCollectionId *split_collection_ids,
                        Iron_IfaceRegistry *iface_reg);

/* Get layout kind for a specific (interface, type).
 * Returns IRON_LAYOUT_AOS if no data or no analysis was performed. */
IronLayoutKind iron_layout_get_kind(LayoutAnalysis *la,
                                     const char *iface_mangled,
                                     const char *type_name);

/* Get common fields for an interface. Returns stb_ds array (NULL if none).
 * Caller must NOT free the returned array. */
CommonField *iron_layout_get_common_fields(LayoutAnalysis *la,
                                            const char *iface_name);

/* Free internal hash map resources. */
void iron_layout_free(LayoutAnalysis *la);

#endif /* IRON_LAYOUT_ANALYSIS_H */
