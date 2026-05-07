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

/* Process-global allocation-id counter; declaration in iron_runtime.h. */
iron_atomic_u64 iron_alloc_id_counter;

/* ── iron_panic_stale_pointer — temporary minimal body ──────────────────────
 * Plan 19-01 needs the symbol present so iron_runtime.a links into every
 * unit test and downstream binary. Plan 19-02 ships the canonical body in
 * src/runtime/iron_panic.c with text/JSON output channels, IRON_PANIC_FORMAT
 * env handling, and distinct heap/stack message variants — and removes this
 * stub. The body below mirrors iron_oom_abort's stderr+abort discipline so
 * that any accidental panic during Plan 19-01 unit tests still terminates
 * the process visibly with a stale-pointer-tagged diagnostic instead of
 * crashing in undefined territory.
 *
 * Tests that exercise the panic path are TEST_IGNORE'd until 19-02 lands. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_stale_pointer(const char *deref_file,
                              int deref_line,
                              const IronAllocHdr *hdr) {
    fprintf(stderr,
            "iron: stale pointer dereference\n"
            "  deref site: %s:%d\n",
            deref_file ? deref_file : "<unknown>",
            deref_line);
    if (hdr) {
        fprintf(stderr,
                "  allocation: size=%llu\n",
                (unsigned long long)hdr->size);
    }
    fflush(stderr);
    abort();
}

/* Compile-time lock on lock-free 64-bit atomics — fail fast on
 * misconfigured 32-bit ARM builds that would otherwise emit __atomic_*
 * library calls and require -latomic. */
#ifndef _WIN32
  _Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
                 "iron_runtime requires lock-free 64-bit atomics. "
                 "On 32-bit ARM, link with -latomic; see Pitfall 8 in "
                 ".planning/phases/19-generational-pointer-infrastructure/19-RESEARCH.md");
#endif

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
#else
    (void)site_file;
    (void)site_line;
#endif

    void *user = (void *)((uint8_t *)block + sizeof(IronAllocHdr));
    return (Iron_FatPtr){user, IRON_ATOMIC_U64_LOAD_ACQUIRE(hdr->gen)};
}

void iron_heap_free(Iron_FatPtr fp) {
    if (!fp.addr) return;
    IronAllocHdr *hdr = ((IronAllocHdr *)fp.addr) - 1;

    /* Double-free / stale-fp validation: caller's gen MUST match current.
     * On mismatch, this is a stale or already-freed pointer — panic via
     * iron_panic_stale_pointer (lands in Plan 19-02; this call resolves
     * at link time when 19-02's atomic commit lands). */
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
