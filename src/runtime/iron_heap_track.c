/* iron_heap_track.c — Phase 19 generational pointer tracker implementation.
 *
 * Header-prepended allocation: each iron_heap_alloc returns a pointer to
 * user payload; the IronAllocHdr immediately precedes that pointer.
 * Recovery is O(1) pointer arithmetic (no global table, no lock contention).
 *
 * Atomicity: generation increments use memory_order_relaxed (monotonic
 * counter; no surrounding writes to publish). Deref-check loads use
 * memory_order_acquire (synchronizes with the freeing thread's relaxed-
 * fetch-add on the same atomic). Pattern: Linux kernel refcount.h.
 *
 * Lifecycle: iron_alloc_id_counter is initialized to 0 in iron_runtime_init
 * (src/runtime/iron_string.c). Phase 31 debug allocator extends this with
 * leak detection / poison-on-free / double-free poisoning.
 *
 * Layout lock: see docs/dev/POINTER-LAYOUT.md (Plan 19-03 closeout).
 * Single-TU implementation discipline: this is the ONLY TU defining the
 * iron_heap_alloc / iron_heap_free symbols.
 */

#include "runtime/iron_runtime.h"
#include "runtime/iron_heap_track.h"
#include "diagnostics/diagnostics.h"  /* iron_oom_abort */

#include <stdint.h>
#include <stdio.h>   /* fprintf, fflush */
#include <stdlib.h>  /* malloc, free, abort */
#include <string.h>  /* memset (DBG-01 poison) */
#include <stdbool.h>

/* Process-global allocation-id counter; declaration in iron_runtime.h. */
iron_atomic_u64 iron_alloc_id_counter;

/* Phase 20 PTR-10: per-thread stack-frame generation counter.
 *
 * Initial value 1 keeps gen=0 reserved as the freed-sentinel value per
 * Phase 19 ABI lock; each thread starts with iron_stack_gen=1 (TLS
 * default-init is 0, but the explicit `= 1` covers every thread because
 * the C language initializes TLS variables from the explicit initializer
 * at thread start, not from the parent thread's value). Bumped per
 * takes_local_addr-marked function entry and once again per return path
 * (per-call semantics; OQ-E lock per CONTEXT.md). Codegen: emit_c.c
 * injects `iron_stack_gen += 1;` in the function prologue and before
 * every IRON_LIR_RETURN when fn->takes_local_addr.
 *
 * Declaration in iron_runtime.h. */
_Thread_local uint64_t iron_stack_gen = 1;

/* iron_panic_stale_pointer body lives in src/runtime/iron_panic.c (Plan
 * 19-02). It is forward-declared in src/runtime/iron_runtime.h with
 * __attribute__((noreturn)); the canonical declaration is in
 * src/diagnostics/diagnostics.h next to iron_oom_abort. The link-time
 * resolve closes the call site in iron_heap_free below and the static-
 * inline iron_check_pointer_gen in iron_runtime.h. */

/* Compile-time lock on lock-free 64-bit atomics — fail fast on
 * misconfigured 32-bit ARM builds that would otherwise emit __atomic_*
 * library calls and require -latomic. */
#ifndef _WIN32
  _Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
                 "iron_runtime requires lock-free 64-bit atomics. "
                 "On 32-bit ARM, link with -latomic; see Pitfall 8 in "
                 ".planning/phases/19-generational-pointer-infrastructure/19-RESEARCH.md");
#endif

/* ── Phase 31 GA1 (Plan 31-01) — debug-allocator state (DBG-03 registry) ────
 *
 * Intrusive doubly-linked registry threaded through the 64B debug header.
 * O(1) link on alloc / unlink on free, serialized by a single mutex. The
 * atexit dump (iron_leak_dump) walks the list and reports still-live
 * allocations to STDERR with their alloc-site provenance.
 *
 * Scope note (Pitfall 3, intended false-negative): only iron_heap_alloc'd
 * blocks are registered. rc (iron_rc.c) and arena (iron_arena_rt.c)
 * allocate their own blocks WITHOUT routing through iron_heap_alloc, so they
 * are intentionally absent from this registry — documented, not a bug. The
 * dump header line states this. */
#ifdef IRON_DEBUG_ALLOCATOR
static IronAllocHdr *s_reg_head = NULL;
static iron_mutex_t  s_reg_lock;
static bool          s_reg_inited = false;

/* Idempotent registry-lock initializer. Called from iron_runtime_init under
 * IRON_DEBUG_ALLOCATOR (src/runtime/iron_string.c). Safe across repeated
 * init in a unit-test harness — the once-flag guards re-init. */
void iron_debug_alloc_init(void) {
    if (!s_reg_inited) {
        IRON_MUTEX_INIT(s_reg_lock);
        s_reg_inited = true;
    }
}

/* DBG-03: link hdr at the registry head (under lock). */
static void iron_debug_registry_link(IronAllocHdr *hdr) {
    if (!s_reg_inited) iron_debug_alloc_init();
    IRON_MUTEX_LOCK(s_reg_lock);
    hdr->reg_next = s_reg_head;
    hdr->reg_prev = NULL;
    if (s_reg_head) s_reg_head->reg_prev = hdr;
    s_reg_head = hdr;
    IRON_MUTEX_UNLOCK(s_reg_lock);
}

/* DBG-03: unlink hdr from the registry (under lock). */
static void iron_debug_registry_unlink(IronAllocHdr *hdr) {
    if (!s_reg_inited) return;
    IRON_MUTEX_LOCK(s_reg_lock);
    if (hdr->reg_prev) {
        hdr->reg_prev->reg_next = hdr->reg_next;
    } else if (s_reg_head == hdr) {
        s_reg_head = hdr->reg_next;
    }
    if (hdr->reg_next) hdr->reg_next->reg_prev = hdr->reg_prev;
    hdr->reg_next = NULL;
    hdr->reg_prev = NULL;
    IRON_MUTEX_UNLOCK(s_reg_lock);
}

/* DBG-03: atexit leak dump. Walks the registry under lock and reports each
 * still-live allocation to STDERR (NOT stdout — Pitfall 7: stdout must be
 * byte-identical debug vs release; only stderr diagnostics differ). Names
 * the alloc-site file:line + size + alloc_id. If the registry is empty,
 * prints NOTHING (clean exit). Tolerates an uninitialized lock. */
void iron_leak_dump(void) {
    if (!s_reg_inited) return;
    IRON_MUTEX_LOCK(s_reg_lock);
    if (s_reg_head == NULL) {
        IRON_MUTEX_UNLOCK(s_reg_lock);
        return;  /* clean exit — no leaks */
    }
    /* Count first so the header line can report the total. */
    uint64_t count = 0;
    for (IronAllocHdr *h = s_reg_head; h; h = h->reg_next) count++;
    fprintf(stderr,
            "iron: %llu heap allocation(s) leaked at exit "
            "(heap allocations only; rc/arena not tracked)\n",
            (unsigned long long)count);
    for (IronAllocHdr *h = s_reg_head; h; h = h->reg_next) {
        const char *af = h->alloc_site_file ? h->alloc_site_file : "<unknown>";
        fprintf(stderr, "  leaked: %s:%u  (id=%u size=%llu)\n",
                af, (unsigned)h->alloc_site_line,
                (unsigned)h->alloc_id,
                (unsigned long long)h->size);
    }
    IRON_MUTEX_UNLOCK(s_reg_lock);
    fflush(stderr);
}
#endif /* IRON_DEBUG_ALLOCATOR */

Iron_FatPtr iron_heap_alloc(const char *site_file, int site_line, size_t size) {
    /* Allocate header + payload contiguously. */
    void *block = malloc(sizeof(IronAllocHdr) + size);
    if (!block) {
        iron_oom_abort("iron_heap_alloc");
    }
    IronAllocHdr *hdr = (IronAllocHdr *)block;

    /* gen=0 reserved for null-sentinel; first valid gen is 1. */
    IRON_ATOMIC_U64_INIT(hdr->gen, 1);
    hdr->size = (uint64_t)size;

#ifdef IRON_DEBUG_ALLOCATOR
    hdr->alloc_site_file = site_file;  /* string-literal pointer; no strdup */
    hdr->alloc_site_line = (uint32_t)site_line;
    hdr->alloc_id = (uint32_t)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(iron_alloc_id_counter, 1);
    /* DBG-04: free-site starts NULL (no free yet). */
    hdr->free_site_file = NULL;
    hdr->free_site_line = 0;
    hdr->_pad = 0;
    /* DBG-03: link into the leak registry (O(1) head insert under lock). */
    iron_debug_registry_link(hdr);
#else
    (void)site_file;
    (void)site_line;
#endif

    void *user = (void *)((uint8_t *)block + sizeof(IronAllocHdr));
    return (Iron_FatPtr){user, IRON_ATOMIC_U64_LOAD_ACQUIRE(hdr->gen)};
}

#ifdef IRON_DEBUG_ALLOCATOR

/* Phase 31 DBG-01/03/04: debug-gated free with explicit free-site.
 *
 * Codegen (src/lir/emit_c.c) emits iron_heap_free_dbg(fp, __FILE__, __LINE__)
 * under IRON_DEBUG_ALLOCATOR so a double-free can report the SECOND/current
 * free-site too. Layered on the Phase 19 gen-mismatch path:
 *   - gen mismatch  → header still holds the FIRST free-site → both-sites
 *                     double-free panic (DBG-04)
 *   - gen match     → record this free-site, unlink from the registry,
 *                     poison the payload 0xDD (DBG-01), bump gen, free. */
void iron_heap_free_dbg(Iron_FatPtr fp, const char *free_file, int free_line) {
    if (!fp.addr) return;
    IronAllocHdr *hdr = ((IronAllocHdr *)fp.addr) - 1;

    uint64_t cur = IRON_ATOMIC_U64_LOAD_ACQUIRE(hdr->gen);
    if (cur != fp.gen) {
        /* DBG-04: the header still holds the FIRST free-site (recorded on the
         * first free below, before the gen bump). Report BOTH sites. */
        iron_panic_double_free(hdr->free_site_file, (int)hdr->free_site_line,
                               free_file, free_line, hdr);
        /* noreturn */
    }

    /* Saturating overflow guard (matches release path). */
    if (cur >= UINT64_MAX - 1) {
        iron_oom_abort("iron_heap_free: generation counter overflow");
    }

    /* DBG-04: record THIS free-site BEFORE bumping gen, so a subsequent
     * double-free reads it as the "first free-site". */
    hdr->free_site_file = free_file;
    hdr->free_site_line = (uint32_t)free_line;

    /* DBG-03: unlink from the registry before poisoning/freeing. */
    iron_debug_registry_unlink(hdr);

    /* DBG-01: poison the user payload so any UAF read hits 0xDD garbage. */
    memset(fp.addr, 0xDD, (size_t)hdr->size);

    /* Bump generation BEFORE freeing (racing acquire-load sees new value). */
    (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(hdr->gen, 1);
    free(hdr);  /* free entire block (header + payload) */
}

/* Debug build: iron_heap_free forwards to the dbg path with no explicit
 * free-site (NULL/0). Codegen normally calls iron_heap_free_dbg directly so
 * this NULL-site fallback is only hit by hand-written runtime callers. */
void iron_heap_free(Iron_FatPtr fp) {
    iron_heap_free_dbg(fp, NULL, 0);
}

#else /* !IRON_DEBUG_ALLOCATOR — release path (unchanged from Phase 19) */

void iron_heap_free(Iron_FatPtr fp) {
    if (!fp.addr) return;
    IronAllocHdr *hdr = ((IronAllocHdr *)fp.addr) - 1;

    /* Double-free / stale-fp validation: caller's gen MUST match current.
     * On mismatch, this is a stale or already-freed pointer — panic via
     * iron_panic_stale_pointer (generic gen-mismatch; release keeps this). */
    uint64_t cur = IRON_ATOMIC_U64_LOAD_ACQUIRE(hdr->gen);
    if (cur != fp.gen) {
        iron_panic_stale_pointer("<iron_heap_free>", 0, hdr);
    }

    /* Saturating overflow guard: bumping at UINT64_MAX-1 -> abort
     * deterministically. UINT64_MAX free()s is physically unreachable
     * (would take ~5 billion years at 100M free/sec) but defines
     * behavior over wraparound (wrap risks false-positive validation). */
    if (cur >= UINT64_MAX - 1) {
        iron_oom_abort("iron_heap_free: generation counter overflow");
    }

    /* Bump generation BEFORE freeing memory so any racing acquire-load
     * on a stale fp.gen sees the new value (mismatches -> triggers panic
     * on next deref). */
    (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(hdr->gen, 1);
    free(hdr);  /* free entire block (header + payload) */
}

/* Release-build iron_heap_free_dbg: thin wrapper. Codegen always emits
 * iron_heap_free_dbg(fp, __FILE__, __LINE__) so the generated C call site is
 * identical across build modes; in release the free-site is ignored and the
 * plain generation-checked free runs (no registry, no poison, generic
 * stale-pointer panic on double-free). */
void iron_heap_free_dbg(Iron_FatPtr fp, const char *free_file, int free_line) {
    (void)free_file;
    (void)free_line;
    iron_heap_free(fp);
}

#endif /* IRON_DEBUG_ALLOCATOR */
