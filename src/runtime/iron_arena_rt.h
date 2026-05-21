/* iron_arena_rt.h — Phase 28 STDLIB-06: arena runtime substrate.
 *
 * A fresh malloc-backed bump allocator with an arena-LEVEL shared generation
 * counter for O(1) mass-invalidation on reset()/restore() (28-CONTEXT.md GA1).
 * Distinct from the compiler's internal Iron_Arena (src/util/arena.c) — this is
 * the user-facing `Arena` stdlib type's runtime backing.
 *
 * Generation & pointer-invalidation model (GA1, monotonic per-alloc snapshot):
 *   - One generation counter (`gen`) per Arena instance, starting at 1.
 *   - alloc() FETCH_ADDs `gen` and snapshots the pre-increment value into the
 *     returned Iron_FatPtr.gen, so snapshots increase monotonically.
 *   - reset() lowers `gen` to the floor (1) — every outstanding pointer goes
 *     stale (O(1)). restore() lowers `gen` to the save's gen_snapshot —
 *     invalidating exactly the post-save allocations while pre-save pointers
 *     (snapshot < gen_snapshot) survive.
 *   - Each allocation carries a minimal 16B prefix header (IronArenaAllocHdr)
 *     holding a back-reference to the arena's live `gen` counter + the payload
 *     size.
 *   - iron_check_arena_pointer_gen (the 3rd deref-check sibling, in
 *     iron_runtime.h) recovers the header via pointer arithmetic, loads
 *     *hdr->arena_gen, and panics via iron_panic_arena_stale iff the snapshot
 *     is >= the live counter (valid iff snapshot < live gen).
 *
 * ABI lock: docs/dev/ARENA-LAYOUT.md.
 *   IronArenaAllocHdr — 16B: arena_gen@0, size@8.
 *   Iron_ArenaSave    — { offset, gen_snapshot } (two uint64 on LP64).
 *   Iron_FatPtr stays 16B ABI-frozen (iron_runtime.h:160) — the arena back-ref
 *   lives in the prefix header, NOT in the fat pointer.
 *
 * Definition: src/runtime/iron_arena_rt.c. Wired into the iron_runtime STATIC
 * target alongside iron_rc.c.
 */
#ifndef IRON_ARENA_RT_H
#define IRON_ARENA_RT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>  /* offsetof for the ABI-lock _Static_asserts */

#include "runtime/iron_runtime.h"  /* iron_atomic_u64, Iron_FatPtr, IRON_ATOMIC_U64_* */

/* ── Per-allocation prefix header (GA1, ARENA-LAYOUT ABI lock) ─────────────
 * Minimal 16B header prepended to every arena allocation. Smaller than the
 * full Phase-19 IronAllocHdr — it only needs the deref-routing back-reference
 * and the payload size (for destructor walks + accounting).
 *
 *   offset 0:  arena_gen — back-ref to the owning Arena's live generation
 *              counter. Deref-routing reads this first.
 *   offset 8:  size      — payload size in bytes.
 *
 * The user payload follows the header, 16B-aligned. Recovery from a user
 * pointer is `((IronArenaAllocHdr *)fp.addr) - 1`. */
typedef struct IronArenaAllocHdr {
    iron_atomic_u64 *arena_gen;  /* back-ref to owning Arena's live gen counter */
    uint64_t         size;       /* payload size — destructor-walk + accounting */
} IronArenaAllocHdr;

_Static_assert(sizeof(IronArenaAllocHdr) == 16,
               "ARENA-LAYOUT ABI lock — IronArenaAllocHdr must be 16B "
               "(arena_gen@0 + size@8). See docs/dev/ARENA-LAYOUT.md.");
_Static_assert(offsetof(IronArenaAllocHdr, arena_gen) == 0,
               "arena_gen@0 — deref-routing back-reference is the first field");
_Static_assert(offsetof(IronArenaAllocHdr, size) == 8,
               "size@8 — destructor-walk + arena accounting");

/* ── Arena runtime struct ──────────────────────────────────────────────────
 * `gen` starts at 1 (gen=0 is the Phase-19 reserved freed sentinel,
 * iron_runtime.h:153). new() uses a plain `offset` bump; new_threadsafe()
 * uses the DEDICATED `atomic_offset` field via IRON_ATOMIC_U64_FETCH_ADD_RELAXED
 * (NOT `gen` — gen is the invalidation counter, not the bump pointer). */
typedef struct Iron_Arena_RT {
    char            *base;          /* malloc-backed buffer */
    uint64_t         offset;        /* current bump offset (plain path) */
    iron_atomic_u64  atomic_offset; /* current bump offset (threadsafe path) */
    uint64_t         capacity;      /* ARENA-01 capacity() */
    iron_atomic_u64  gen;           /* shared live generation; bumped on reset/restore */
    bool             threadsafe;    /* ARENA-02 selects atomic vs plain bump */
    const char      *name;          /* arena-specific panic message (ARENA-10) */
    struct Iron_Arena_RT *prev;     /* TLS intrusive active-arena stack link (ARENA-11) */
} Iron_Arena_RT;

/* ── Save point (GA1) ───────────────────────────────────────────────────────
 * save() captures the current bump offset + the generation snapshot.
 * restore() lowers the bump pointer to `offset` AND bumps `gen` (ARENA-07). */
typedef struct {
    uint64_t offset;
    uint64_t gen_snapshot;
} Iron_ArenaSave;

/* ── Public arena API (the Plan 28-01 unit-test contract) ──────────────────── */
Iron_Arena_RT *iron_arena_rt_new(uint64_t capacity, bool threadsafe, const char *name);
Iron_FatPtr    iron_arena_rt_alloc(Iron_Arena_RT *a, uint64_t size);
void           iron_arena_rt_reset(Iron_Arena_RT *a);                 /* ARENA-06 */
Iron_ArenaSave iron_arena_rt_save(Iron_Arena_RT *a);                  /* ARENA-01 */
void           iron_arena_rt_restore(Iron_Arena_RT *a, Iron_ArenaSave s); /* ARENA-07 */
uint64_t       iron_arena_rt_used(Iron_Arena_RT *a);                  /* ARENA-01 */
uint64_t       iron_arena_rt_capacity(Iron_Arena_RT *a);              /* ARENA-01 */
void           iron_arena_rt_destroy(Iron_Arena_RT *a);

/* ── Thread-local active-arena stack (ARENA-11) ────────────────────────────
 * Intrusive singly-linked stack, mirroring iron_init_cleanup_top
 * (iron_runtime.h:334). `in arena {}` pushes on entry, pops on exit; bare
 * `heap` inside resolves the innermost via iron_arena_rt_current(). current()
 * returns NULL when empty (→ general allocator, ARENA-05). */
extern _Thread_local Iron_Arena_RT *iron_arena_rt_tls_top;
void           iron_arena_rt_push(Iron_Arena_RT *a);
void           iron_arena_rt_pop(void);
Iron_Arena_RT *iron_arena_rt_current(void);

#endif /* IRON_ARENA_RT_H */
