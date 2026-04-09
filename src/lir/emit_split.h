/* emit_split.h -- Split collection struct generation, push/free functions, and prescan.
 *
 * This module handles all split collection emission:
 *   - Module-level prescan to identify interface-typed ARRAY_LITs
 *   - Arena-tracked allocation helpers (_iron_sl_track, etc.)
 *   - Per-interface split collection struct generation (Iron_SplitList_<Iface>)
 *   - Per-type push functions (Iron_SplitList_<Iface>_push_<Type>)
 *   - Free functions (Iron_SplitList_<Iface>_free)
 *
 * Extracted from emit_c.c and emit_structs.c (Phase 52, Plan 03).
 */

#ifndef IRON_EMIT_SPLIT_H
#define IRON_EMIT_SPLIT_H

#include "lir/emit_helpers.h"

/* Pre-scan all functions for interface-typed ARRAY_LITs to identify
 * split collections. Populates ctx->split_collection_ids, runs
 * layout analysis and value range analysis. */
void emit_prescan_split_collections(EmitCtx *ctx);

/* Emit arena-tracked allocation helpers (_iron_sl_track,
 * _iron_sl_realloc_tracked, _iron_sl_free_all) as static inline
 * functions into ctx->struct_bodies. Called once before the
 * per-interface loop in emit_type_decls. */
void emit_split_arena_helpers(EmitCtx *ctx);

/* Emit split collection structs, push functions, and free functions
 * for a single interface entry. Called from emit_type_decls for each
 * interface with alive implementors.
 *
 * Writes to ctx->struct_bodies (struct + push + free).
 * iface_mangled: mangled interface name (e.g., "Iron_Shape")
 * entry: interface registry entry with implementor info
 */
void emit_split_collection_for_iface(EmitCtx *ctx, const char *iface_mangled,
                                      Iron_IfaceEntry *entry);

#endif /* IRON_EMIT_SPLIT_H */
