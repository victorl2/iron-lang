/* iron_rc.c — Phase 26 POL-06: rc policy runtime substrate.
 *
 * Plan 26-01 Task 2 GREEN: replaces the Task 1 Wave 0 RED stubs with the
 * Iron_RcHeader-based implementation following Phase 19's atomic-ordering
 * discipline.
 *
 * Block layout: [Iron_RcHeader][IronAllocHdr][user payload].
 * User pointer = payload start (preserves Phase 19 ABI invariant — every
 * `*T` pointing into rc-allocated memory walks back via the same
 * `((IronAllocHdr *)addr) - 1` recipe used by Phase 19's deref check).
 *
 * Atomic discipline (Rust Arc canonical, cross-verified against
 * RustBelt-Relaxed paper + mara.nl "Building Our Own Arc"):
 *   retain:  IRON_ATOMIC_U64_FETCH_ADD_RELAXED(refcount, 1)
 *   release: prev = IRON_ATOMIC_U64_FETCH_SUB_RELEASE(refcount, 1)
 *   if (prev == 1) {
 *       IRON_ATOMIC_FENCE_ACQUIRE();
 *       if (drop_fn) drop_fn(user_ptr);
 *       free(block);
 *   }
 *
 * Pitfall 1 (RESEARCH §271-281): UINT64_MAX-1 saturation on retain;
 *   underflow ICE on release in debug.
 * Pitfall 2 (RESEARCH §283-294): acquire fence MUST precede destructor so the
 *   user-side `drop {}` body observes writes from other holders.
 * Pitfall 4 (RESEARCH §309-319): no retain on already-released — the type
 *   system enforces this in Phase 26; Phase 27 weak-rc adds the strong→weak
 *   hard case via a separate weak_count field.
 *
 * Underflow detection (debug): asserts `prev > 0`; fires iron_oom_abort with
 *   a diagnostic message (iron_ice is compiler-only and not linkable from
 *   iron_runtime — see Plan 26-01 SUMMARY.md deviation note). Release builds
 *   silently wrap — the resulting leak is the defined cost over an undefined
 *   use-after-free.
 *
 * Overflow detection (debug): saturates at UINT64_MAX-1; fires iron_oom_abort
 *   deterministically. UINT64_MAX retains is physically unreachable
 *   (~5 billion years at 100M ops/sec) so the abort defines behavior over wrap.
 *
 * Lock-document: docs/dev/RC-LAYOUT.md (Phase 26 Plan 26-01 closeout).
 */

#include "runtime/iron_runtime.h"
#include "diagnostics/diagnostics.h"  /* iron_oom_abort — runtime canonical abort.
                                       *
                                       * NB: iron_ice lives in iron_compiler
                                       * (diagnostics.c) and is NOT linked into
                                       * iron_runtime — so the debug underflow
                                       * path below uses iron_oom_abort, matching
                                       * the Phase 19 heap_track.c:107
                                       * gen-overflow precedent. Documented as
                                       * a Plan 26-01 deviation in SUMMARY.md. */

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

/* Block size: [Iron_RcHeader][IronAllocHdr][payload].
 *
 * The two-header prefix is recovered in O(1) by walking the user pointer back
 * by exactly this many bytes. The order (rc header FIRST, alloc header
 * SECOND, payload THIRD) is locked by RC-LAYOUT.md §1 and is required for
 * Phase 19's `((IronAllocHdr *)addr) - 1` deref-check arithmetic to survive
 * unchanged when applied to `*T` pointers derived from rc-allocated memory.
 */
#define IRON_RC_HEADER_TOTAL_SIZE (sizeof(Iron_RcHeader) + sizeof(IronAllocHdr))

/* ── iron_rc_header_of — O(1) recovery of the rc header ─────────────────────
 *
 * Walks the user pointer back by IRON_RC_HEADER_TOTAL_SIZE bytes. Returns
 * NULL for NULL input (Phase 19 NULL-discipline mirror).
 *
 * This is the inverse of the iron_rc_alloc pointer arithmetic:
 *   alloc:     block + Iron_RcHeader + IronAllocHdr -> user pointer
 *   recovery:  user pointer - IronAllocHdr - Iron_RcHeader -> block
 *
 * Exposed in the public API specifically so the test suite can probe the
 * refcount field directly (test_rc_layout, test_rc_atomic_ordering,
 * test_runtime_rc_concurrent). Production callers should NOT touch the
 * header directly — use iron_rc_retain / iron_rc_release.
 */
Iron_RcHeader *iron_rc_header_of(void *user_ptr) {
    if (!user_ptr) return NULL;
    return (Iron_RcHeader *)((char *)user_ptr - IRON_RC_HEADER_TOTAL_SIZE);
}

/* ── iron_rc_alloc — allocate refcounted block ──────────────────────────────
 *
 * Layout:
 *   block[0..16)        Iron_RcHeader  (refcount = 1, drop_fn = user)
 *   block[16..32)       IronAllocHdr   (gen = 1, size = `size`)
 *   block[32..32+size)  user payload
 *
 * Returns the user pointer (payload start). On allocation failure aborts
 * deterministically via iron_oom_abort (matches Phase 19's heap path).
 *
 * The Phase 19 IronAllocHdr fields gen + size are initialized so that any
 * `*T` pointer derived from this block walks through the standard deref
 * check unchanged (RC-LAYOUT.md §1 — Phase 19 ABI invariant preserved).
 */
void *iron_rc_alloc(size_t size, void (*drop_fn)(void *)) {
    void *block = malloc(IRON_RC_HEADER_TOTAL_SIZE + size);
    if (!block) iron_oom_abort("iron_rc_alloc");

    Iron_RcHeader *rch = (Iron_RcHeader *)block;
    IRON_ATOMIC_U64_INIT(rch->refcount, 1);
    rch->drop_fn = drop_fn;

    IronAllocHdr *ahd =
        (IronAllocHdr *)((char *)block + sizeof(Iron_RcHeader));
    IRON_ATOMIC_U64_INIT(ahd->gen, 1);
    ahd->size = (uint64_t)size;

    return (char *)block + IRON_RC_HEADER_TOTAL_SIZE;
}

/* ── iron_rc_retain — relaxed-inc refcount bump ─────────────────────────────
 *
 * Single relaxed atomic fetch_add on the refcount. Per Rust Arc convention,
 * copies don't observe ordering of unrelated writes — the only invariant the
 * retain operation must preserve is monotonicity, which relaxed semantics
 * provide.
 *
 * Pitfall 1 (debug guard): saturate at UINT64_MAX-1. If the current refcount
 * is already in the saturation band, the next retain would wrap to 0 and
 * trigger an immediate destructor invocation on a still-live value. We abort
 * deterministically instead.
 *
 * The acquire-load used for the overflow check is permissible in debug:
 * UINT64_MAX retains is physically unreachable, so the cost is irrelevant.
 * In release builds the check is compiled out — `iron_rc_retain` is a single
 * relaxed-inc instruction.
 */
void iron_rc_retain(void *user_ptr) {
    if (!user_ptr) return;  /* defensive — Phase 19 NULL-discipline mirror */
    Iron_RcHeader *rch = iron_rc_header_of(user_ptr);
#ifndef NDEBUG
    uint64_t cur = IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount);
    if (cur >= UINT64_MAX - 1) {
        iron_oom_abort("iron_rc_retain: refcount overflow (UINT64_MAX-1)");
    }
#endif
    (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(rch->refcount, 1);
}

/* ── iron_rc_release — release-dec with acquire-fence on final drop ─────────
 *
 * Per Rust Arc canonical pattern:
 *   1. fetch_sub with RELEASE semantics — caller's prior writes to fields
 *      synchronize-with the destructor that may eventually read them.
 *   2. If fetch_sub returned 1, this thread observed the linearization
 *      point (refcount transitioned to 0). Emit an acquire fence before
 *      invoking the destructor so writes from other holders (which
 *      synchronized-with the release in step 1) are visible.
 *   3. Invoke drop_fn(user_ptr) if non-NULL — primitive-payload rc
 *      allocations skip the trampoline cleanly.
 *   4. free the entire block (Iron_RcHeader + IronAllocHdr + payload).
 *
 * Pitfall 1 (debug underflow guard): prev == 0 means caller released on
 * an already-zero refcount — a programmer bug. Fires iron_ice with the
 * allocation-site message. In release builds the wrap is silent and the
 * resulting allocation is leaked (defined behavior over use-after-free).
 *
 * Reference: https://mara.nl/atomics/building-arc.html ;
 *            https://github.com/rust-lang/rust/issues/62230
 */
void iron_rc_release(void *user_ptr) {
    if (!user_ptr) return;
    Iron_RcHeader *rch = iron_rc_header_of(user_ptr);

    uint64_t prev = IRON_ATOMIC_U64_FETCH_SUB_RELEASE(rch->refcount, 1);

#ifndef NDEBUG
    if (prev == 0) {
        /* Programmer-bug semantic — caller released on already-zero refcount.
         * Would-be iron_ice, but iron_runtime cannot link the compiler-side
         * diagnostics module, so we route through iron_oom_abort following
         * Phase 19's heap_track.c:107 precedent. The abort message preserves
         * the diagnostic intent. */
        iron_oom_abort("iron_rc_release: refcount underflow (already at 0)");
    }
#endif

    if (prev == 1) {
        /* Last reference — acquire fence synchronizes with all prior
         * releases from other threads so the destructor observes their
         * writes to user fields. */
        IRON_ATOMIC_FENCE_ACQUIRE();

        if (rch->drop_fn) {
            rch->drop_fn(user_ptr);
        }

        /* Free the entire block (header + IronAllocHdr + payload). */
        free((char *)user_ptr - IRON_RC_HEADER_TOTAL_SIZE);
    }
}
