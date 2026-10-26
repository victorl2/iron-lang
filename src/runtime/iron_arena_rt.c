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

/* Arena epochs are unique across all arenas, so a save point taken in
 * another arena's first segment never passes for one of this arena. */
static iron_atomic_u64 s_arena_epoch = 1;

/* Open a fresh segment: a new generation for the allocations from here on. */
static void arena_seg_open(Iron_Arena_RT *a) {
    if (a->nsegs == a->segcap) {
        uint64_t cap = a->segcap ? a->segcap * 2 : 8;
        uint64_t *n = (uint64_t *)realloc(a->segs, (size_t)cap * sizeof(uint64_t));
        if (!n) iron_oom_abort("iron_arena_rt: segments");
        a->segs = n;
        a->segcap = cap;
    }
    uint64_t g = iron_gen_new(true);
    a->segs[a->nsegs++] = g;
    IRON_ATOMIC_U64_STORE_RELEASE(a->gen, g);
    IRON_ATOMIC_U64_STORE_RELEASE(a->used, 0);
}

/* End segments [from, nsegs): every pointer into them goes stale. */
static void arena_seg_end_from(Iron_Arena_RT *a, uint64_t from) {
    while (a->nsegs > from) (void)iron_gen_end(a->segs[--a->nsegs]);
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
    a->segs       = NULL;
    a->nsegs      = 0;
    a->segcap     = 0;
    a->epoch      = IRON_ATOMIC_U64_FETCH_ADD_RELAXED(s_arena_epoch, 1);
    IRON_ATOMIC_U64_INIT(a->atomic_offset, 0);
    IRON_ATOMIC_U64_INIT(a->gen, 0);
    IRON_ATOMIC_U64_INIT(a->used, 0);
    IRON_ATOMIC_U64_INIT(a->refs, 1);
    arena_seg_open(a);
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
    hdr->arena_gen = &a->gen;   /* the arena's current segment generation */
    hdr->size      = size;

    /* The allocation's generation is its segment's (#432): it stays valid
     * until a reset, a restore to a save point taken before it, or the
     * arena's end ends that segment. Nothing in the arena is read by a
     * check. */
    Iron_FatPtr fp;
    fp.addr = (char *)hdr + sizeof *hdr;                  /* 16B-aligned payload */
    fp.gen  = IRON_ATOMIC_U64_LOAD_ACQUIRE(a->gen);
    if (!IRON_ATOMIC_U64_LOAD_ACQUIRE(a->used)) IRON_ATOMIC_U64_STORE_RELEASE(a->used, 1);
    return fp;
}

void iron_arena_rt_reset(Iron_Arena_RT *a) {                  /* ARENA-06 */
    if (a->threadsafe) {
        IRON_ATOMIC_U64_STORE_RELEASE(a->atomic_offset, 0);
    } else {
        a->offset = 0;
    }
    /* Every segment ends: every outstanding pointer goes stale, and so does
     * every save point (a new epoch). */
    arena_seg_end_from(a, 0);
    a->epoch = IRON_ATOMIC_U64_FETCH_ADD_RELAXED(s_arena_epoch, 1);
    arena_seg_open(a);
}

Iron_ArenaSave iron_arena_rt_save(Iron_Arena_RT *a) {         /* ARENA-01 */
    Iron_ArenaSave s;
    s.offset = a->threadsafe ? IRON_ATOMIC_U64_LOAD_ACQUIRE(a->atomic_offset)
                             : a->offset;
    /* Allocations after the save point must not share a segment with the
     * ones before it: open a new segment unless the current one is empty. */
    if (IRON_ATOMIC_U64_LOAD_ACQUIRE(a->used)) arena_seg_open(a);
    s.seg = a->nsegs - 1;
    s.gen_snapshot = s.seg > 0 ? a->segs[s.seg - 1] : a->epoch;
    return s;
}

void iron_arena_rt_restore(Iron_Arena_RT *a, Iron_ArenaSave s) {  /* ARENA-07 */
    /* A save point is valid while everything allocated before it still is:
     * no reset since, and no restore to an earlier save point. */
    if (s.seg >= a->nsegs ||
        (s.seg > 0 ? a->segs[s.seg - 1] : a->epoch) != s.gen_snapshot) {
        iron_panic_arena_stale_save(a->name);
    }
    if (a->threadsafe) {
        IRON_ATOMIC_U64_STORE_RELEASE(a->atomic_offset, s.offset);
    } else {
        a->offset = s.offset;
    }
    /* Nothing allocated since the save point: nothing to end. */
    if (s.seg == a->nsegs - 1 && !IRON_ATOMIC_U64_LOAD_ACQUIRE(a->used)) return;
    arena_seg_end_from(a, s.seg);
    arena_seg_open(a);
}

uint64_t iron_arena_rt_used(Iron_Arena_RT *a) {               /* ARENA-01 */
    return a->threadsafe ? IRON_ATOMIC_U64_LOAD_ACQUIRE(a->atomic_offset)
                         : a->offset;
}

uint64_t iron_arena_rt_capacity(Iron_Arena_RT *a) {           /* ARENA-01 */
    return a->capacity;
}

/* A spawned task that uses the arena keeps it alive until it finishes; the
 * owner's destroy then only drops its reference. */
void iron_arena_rt_retain(Iron_Arena_RT *a) {
    if (a) (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(a->refs, 1);
}

void iron_arena_rt_destroy(Iron_Arena_RT *a) {
    if (!a) return;
    IRON_HB_BEFORE(&a->refs);
    if (IRON_ATOMIC_U64_FETCH_SUB_RELEASE(a->refs, 1) != 1) return;
    IRON_ATOMIC_FENCE_ACQUIRE();
    IRON_HB_AFTER(&a->refs);
    (void)IRON_ATOMIC_U64_LOAD_ACQUIRE(a->refs);
    /* The arena's end: pointers into it go stale before its memory goes. */
    arena_seg_end_from(a, 0);
    free(a->segs);
    free(a->base);
    free(a);
}

/* ── Thread-local active-arena stack (ARENA-11) ────────────────────────────
 * `in a { }` pushes on entry and pops on exit. The tops below the current
 * one are kept in a per-thread array, not in the arenas: the stack used to
 * be linked through a `prev` field of each arena, so entering an arena
 * already on the stack (`in a { in a { } }`, or a function doing `in a`
 * called inside `in a`) made it point to itself; the pops then left the
 * wrong arena on top, or one already destroyed. The first entries live in
 * a fixed array; deeper nesting (recursion through `in` blocks) spills to
 * the heap, freed once the nesting unwinds. Push and pop always pair, a
 * NULL arena included. */
_Thread_local Iron_Arena_RT *iron_arena_rt_tls_top = NULL;

#define ARENA_STACK_FIXED 64
static _Thread_local Iron_Arena_RT *arena_saved_fixed[ARENA_STACK_FIXED];
static _Thread_local Iron_Arena_RT **arena_saved_more = NULL;
static _Thread_local int arena_saved_more_cap = 0;
static _Thread_local int arena_saved_depth = 0;

void iron_arena_rt_push(Iron_Arena_RT *a) {
    int d = arena_saved_depth;
    if (d < ARENA_STACK_FIXED) {
        arena_saved_fixed[d] = iron_arena_rt_tls_top;
    } else {
        int k = d - ARENA_STACK_FIXED;
        if (k >= arena_saved_more_cap) {
            int cap = arena_saved_more_cap ? arena_saved_more_cap * 2 : 64;
            Iron_Arena_RT **more = (Iron_Arena_RT **)realloc(
                arena_saved_more, (size_t)cap * sizeof(Iron_Arena_RT *));
            if (!more) iron_oom_abort("iron_arena_rt.c:iron_arena_rt_push");
            arena_saved_more = more;
            arena_saved_more_cap = cap;
        }
        arena_saved_more[k] = iron_arena_rt_tls_top;
    }
    arena_saved_depth = d + 1;
    iron_arena_rt_tls_top = a;
}

void iron_arena_rt_pop(void) {
    if (arena_saved_depth == 0) {
        iron_arena_rt_tls_top = NULL;
        return;
    }
    int d = --arena_saved_depth;
    if (d < ARENA_STACK_FIXED) {
        iron_arena_rt_tls_top = arena_saved_fixed[d];
        if (arena_saved_more) {
            free(arena_saved_more);
            arena_saved_more = NULL;
            arena_saved_more_cap = 0;
        }
    } else {
        iron_arena_rt_tls_top = arena_saved_more[d - ARENA_STACK_FIXED];
    }
}

Iron_Arena_RT *iron_arena_rt_current(void) {
    return iron_arena_rt_tls_top;
}
