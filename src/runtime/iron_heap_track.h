/* iron_heap_track.h — Phase 19 generational pointer tracker (public API).
 *
 * This header re-exports the heap-tracker surface from iron_runtime.h so
 * Phase 21 codegen and Phase 19 unit tests can include just the tracker
 * without pulling in the full iron_runtime API. The actual declarations
 * live in iron_runtime.h (alongside the IRON_ATOMIC_U64_* macros and
 * Iron_FatPtr typedef they depend on).
 *
 * Definitions: src/runtime/iron_heap_track.c
 * Static-inline iron_check_pointer_gen body: iron_runtime.h
 * Header layout lock: docs/dev/POINTER-LAYOUT.md (Plan 19-03 closeout)
 */
#ifndef IRON_HEAP_TRACK_H
#define IRON_HEAP_TRACK_H

#include "runtime/iron_runtime.h"

/* All declarations are in iron_runtime.h:
 *   - typedef Iron_FatPtr / IronAllocHdr
 *   - Iron_FatPtr iron_heap_alloc(const char *, int, size_t)
 *   - void iron_heap_free(Iron_FatPtr)
 *   - static inline iron_check_pointer_gen(Iron_FatPtr, const char *, int)
 *   - extern iron_atomic_u64 iron_alloc_id_counter
 *
 * This header exists so includers can express dependency on heap-tracking
 * specifically rather than the broad iron_runtime surface.
 */

#endif /* IRON_HEAP_TRACK_H */
