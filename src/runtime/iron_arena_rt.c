/* iron_arena_rt.c — Phase 28 STDLIB-06: arena runtime substrate.
 *
 * Plan 28-02 GREEN: implements the malloc-backed bump allocator declared in
 * iron_arena_rt.h, flipping the Plan 28-01 Wave 0 RED unit tests
 * (test_arena_rt, test_arena_gen_invalidation, test_arena_layout) to GREEN.
 *
 * Mirrors iron_rc.c's malloc + iron_oom_abort allocation shape and the
 * Phase 19/20 atomic-ordering discipline (IRON_ATOMIC_U64_* macros).
 *
 * Generation model (28-CONTEXT.md GA1 — monotonic per-allocation snapshot):
 *   - `gen` starts at 1 (Pitfall 3: gen=0 is the Phase-19 freed sentinel).
 *   - alloc() FETCH_ADDs gen by 1 and snapshots the PRE-increment value into
 *     the returned Iron_FatPtr.gen, so snapshots increase monotonically.
 *   - reset() lowers the live counter to the floor (1) — O(1) invalidation of
 *     every outstanding pointer (all snapshots >= 1). restore() lowers it to
 *     the save's gen_snapshot — invalidating exactly the post-save allocations
 *     while pre-save pointers survive.
 *   - Each alloc writes a back-ref to `gen` into its prefix header; the
 *     returned Iron_FatPtr.gen is the snapshot. iron_check_arena_pointer_gen
 *     (iron_runtime.h) panics iff snapshot >= *hdr->arena_gen (i.e. valid iff
 *     snapshot < live gen) on deref.
 *
 * Bump path:
 *   - non-threadsafe: plain `offset += need`.
 *   - threadsafe (ARENA-02): IRON_ATOMIC_U64_FETCH_ADD_RELAXED on the
 *     DEDICATED `atomic_offset` field (NOT `gen` — gen is the invalidation
 *     counter). used() reads whichever field the arena's mode uses.
 *
 * OOM (ARENA-10): iron_panic_arena_oom(name, requested_size, capacity) —
 *   noreturn; the bump-pointer contract never returns null.
 *
 * ABI lock: docs/dev/ARENA-LAYOUT.md.
 */

#include "runtime/iron_arena_rt.h"
#include "runtime/iron_panic.h"        /* iron_panic_arena_stale / iron_panic_arena_oom */
#include "diagnostics/diagnostics.h"   /* iron_oom_abort */

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

/* 16B-align: round `n` up to the next multiple of 16. Keeps every payload
 * 16B-aligned given the header is exactly 16B and malloc returns
 * max_align_t-aligned base (so base+offset stays 16B-aligned). */
static inline uint64_t iron_arena_rt_align16(uint64_t n) {
    return (n + 15u) & ~(uint64_t)15u;
}

Iron_Arena_RT *iron_arena_rt_new(uint64_t capacity, bool threadsafe,
                              const char *name) {
    Iron_Arena_RT *a = (Iron_Arena_RT *)malloc(sizeof *a);
    if (!a) iron_oom_abort("iron_arena_rt_new");
    a->base = (char *)malloc(capacity ? capacity : 1);
    if (!a->base) iron_panic_arena_oom(name, capacity, 0);  /* ARENA-10 */
    a->offset     = 0;
    a->capacity   = capacity;
    a->threadsafe = threadsafe;
    a->name       = name;
    a->prev       = NULL;
    IRON_ATOMIC_U64_INIT(a->atomic_offset, 0);
    IRON_ATOMIC_U64_INIT(a->gen, 1);  /* Pitfall 3: gen starts at 1 */
    return a;
}

Iron_FatPtr iron_arena_rt_alloc(Iron_Arena_RT *a, uint64_t size) {
    /* header + payload, payload 16B-aligned. Overflow-harden the size math
     * first: a size near UINT64_MAX would wrap `sizeof(hdr) + size` (and the
     * align16 round-up), producing a tiny `need` that slips past the capacity
     * check and writes the header/payload out of bounds. Reject before any
     * arithmetic can wrap; the capacity comparison below is phrased to be
     * wrap-proof as well. */
    if (size > UINT64_MAX - sizeof(IronArenaAllocHdr) - 15u) {
        iron_panic_arena_oom(a->name, size, a->capacity);  /* ARENA-10, noreturn */
    }
    uint64_t need = iron_arena_rt_align16(sizeof(IronArenaAllocHdr) + size);

    uint64_t off;
    if (a->threadsafe) {
        /* ARENA-02: atomic bump on the DEDICATED offset field. */
        off = IRON_ATOMIC_U64_FETCH_ADD_RELAXED(a->atomic_offset, need);
    } else {
        off = a->offset;
        a->offset += need;
    }

    if (need > a->capacity || off > a->capacity - need) {
        iron_panic_arena_oom(a->name, size, a->capacity);  /* ARENA-10, noreturn */
    }

    IronArenaAllocHdr *hdr = (IronArenaAllocHdr *)(a->base + off);
    hdr->arena_gen = &a->gen;   /* back-ref to live counter (GA1) */
    hdr->size      = size;

    /* Per-allocation generation tag (GA1, monotonic). FETCH_ADD returns the
     * PRE-increment value: the allocation's snapshot is that old value, and the
     * arena's live counter advances by one. The deref check (iron_runtime.h)
     * holds the invariant "valid iff snapshot < live gen" — so a pointer is
     * valid the instant it is handed out (old < old+1) and stays valid until a
     * reset()/restore() lowers the live counter to or below its snapshot.
     *
     * Why per-alloc (not per-reset-only) increment: it is the only scheme that
     * lets restore() selectively invalidate exactly the post-save allocations
     * while pre-save pointers survive — both share the same arena-LEVEL counter,
     * so they must be distinguished by their MONOTONIC snapshot ordering, not by
     * a single bump (which cannot tell pre- from post-save apart). reset() lowers
     * the counter to the floor (1) to invalidate every outstanding pointer in
     * O(1); restore() lowers it to the saved snapshot. gen never reaches 0 (the
     * Phase-19 reserved freed sentinel, iron_runtime.h:153). */
    Iron_FatPtr fp;
    fp.addr = (char *)hdr + sizeof *hdr;                  /* 16B-aligned payload */
    fp.gen  = IRON_ATOMIC_U64_FETCH_ADD_RELAXED(a->gen, 1); /* snapshot = old gen */
    return fp;
}

void iron_arena_rt_reset(Iron_Arena_RT *a) {                  /* ARENA-06 */
    if (a->threadsafe) {
        IRON_ATOMIC_U64_STORE_RELEASE(a->atomic_offset, 0);
    } else {
        a->offset = 0;
    }
    /* Lower the live counter to the floor (1). Every outstanding snapshot is
     * >= 1, so all become stale (snapshot >= cur) in O(1). Subsequent allocs
     * resume handing out 1, 2, 3, ... again. (STORE, not INIT: atomic_init
     * on a live atomic racing concurrent fetch_adds is C11 UB.) */
    IRON_ATOMIC_U64_STORE_RELEASE(a->gen, 1);
}

Iron_ArenaSave iron_arena_rt_save(Iron_Arena_RT *a) {         /* ARENA-01 */
    Iron_ArenaSave s;
    s.offset = a->threadsafe ? IRON_ATOMIC_U64_LOAD_ACQUIRE(a->atomic_offset)
                             : a->offset;
    /* The live counter == the snapshot the NEXT allocation will receive, i.e.
     * one past the last handed-out snapshot. restore() lowers gen back to this
     * value: allocations made after save() carry snapshots >= gen_snapshot and
     * go stale; allocations made before save() carry snapshots < gen_snapshot
     * and survive. */
    s.gen_snapshot = IRON_ATOMIC_U64_LOAD_ACQUIRE(a->gen);
    return s;
}

void iron_arena_rt_restore(Iron_Arena_RT *a, Iron_ArenaSave s) {  /* ARENA-07 */
    if (a->threadsafe) {
        IRON_ATOMIC_U64_STORE_RELEASE(a->atomic_offset, s.offset);
    } else {
        a->offset = s.offset;
    }
    /* Lower the live counter to the saved snapshot: invalidates exactly the
     * post-save allocations (snapshot >= gen_snapshot) while pre-save pointers
     * (snapshot < gen_snapshot) stay valid. O(1). (STORE, not INIT — see
     * iron_arena_rt_reset.) */
    IRON_ATOMIC_U64_STORE_RELEASE(a->gen, s.gen_snapshot);
}

uint64_t iron_arena_rt_used(Iron_Arena_RT *a) {               /* ARENA-01 */
    return a->threadsafe ? IRON_ATOMIC_U64_LOAD_ACQUIRE(a->atomic_offset)
                         : a->offset;
}

uint64_t iron_arena_rt_capacity(Iron_Arena_RT *a) {           /* ARENA-01 */
    return a->capacity;
}

void iron_arena_rt_destroy(Iron_Arena_RT *a) {
    if (!a) return;
    free(a->base);
    free(a);
}

/* ── Thread-local active-arena stack (ARENA-11) ────────────────────────────
 * Intrusive singly-linked stack, mirroring iron_init_cleanup_top
 * (iron_panic.c:42). push sets a->prev = current top; pop restores prev. */
_Thread_local Iron_Arena_RT *iron_arena_rt_tls_top = NULL;

void iron_arena_rt_push(Iron_Arena_RT *a) {
    if (!a) return;
    a->prev = iron_arena_rt_tls_top;
    iron_arena_rt_tls_top = a;
}

void iron_arena_rt_pop(void) {
    if (!iron_arena_rt_tls_top) return;
    iron_arena_rt_tls_top = iron_arena_rt_tls_top->prev;
}

Iron_Arena_RT *iron_arena_rt_current(void) {
    return iron_arena_rt_tls_top;
}
