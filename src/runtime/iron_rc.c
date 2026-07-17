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
 *       iron_weak_rc_release(user_ptr);   // drop the collective weak;
 *                                         // weak 1→0 edge frees the block
 *   }
 *
 * weak_count starts at 1: the strong cohort collectively owns one weak ref
 * (Rust Arc scheme). The single weak counter linearizes the block free —
 * see iron_rc_alloc / iron_weak_rc_release for the race this prevents.
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

/* ── Phase 29 OPT-08 input: opt-in rc-op counter ─────────────────────────────
 *
 * Behind IRON_RC_COUNT (OFF by default), a pair of `_Atomic uint64_t` counters
 * increment (relaxed) inside iron_rc_retain / iron_rc_release. They feed the
 * deferred OPT-08 measurement that informs the deferred OQ-07 `arc`-policy
 * decision (docs/dev/RC-ELISION.md). The macro must NOT alter the hot path in
 * normal builds — when undefined, IRON_RC_COUNT_BUMP_* expand to nothing and
 * the accessor reports zeros, so production refcount performance and the
 * deterministic phase-invariant test counts are unaffected.
 *
 * Relaxed ordering is correct for a pure observation counter: it tracks a
 * monotone op tally with no happens-before obligation toward the refcount
 * fields, mirroring the iron_rc_retain relaxed-inc discipline. */
#ifdef IRON_RC_COUNT
static _Atomic uint64_t s_rc_retain_count;
static _Atomic uint64_t s_rc_release_count;
#  define IRON_RC_COUNT_BUMP_RETAIN()  \
       (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(s_rc_retain_count, 1)
#  define IRON_RC_COUNT_BUMP_RELEASE() \
       (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(s_rc_release_count, 1)
#else
#  define IRON_RC_COUNT_BUMP_RETAIN()  ((void)0)
#  define IRON_RC_COUNT_BUMP_RELEASE() ((void)0)
#endif

/* iron_rc_op_counts — read the opt-in retain/release tallies.
 *
 * Always defined (stable symbol). When IRON_RC_COUNT is undefined both
 * out-params receive 0. NULL out-params are tolerated (partial read). */
void iron_rc_op_counts(uint64_t *retains, uint64_t *releases) {
#ifdef IRON_RC_COUNT
    if (retains)  *retains  = IRON_ATOMIC_U64_LOAD_ACQUIRE(s_rc_retain_count);
    if (releases) *releases = IRON_ATOMIC_U64_LOAD_ACQUIRE(s_rc_release_count);
#else
    if (retains)  *retains  = 0;
    if (releases) *releases = 0;
#endif
}

/* iron_rc_op_counts_reset — zero the tallies. No-op when the macro is off. */
void iron_rc_op_counts_reset(void) {
#ifdef IRON_RC_COUNT
    IRON_ATOMIC_U64_INIT(s_rc_retain_count, 0);
    IRON_ATOMIC_U64_INIT(s_rc_release_count, 0);
#endif
}

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
    /* weak_count starts at 1: the strong cohort collectively owns ONE weak
     * reference (Rust Arc's scheme — library/alloc/src/sync.rs). The final
     * strong release runs drop_fn and then releases this collective weak;
     * whoever takes weak_count to 0 frees the block. A single counter thus
     * linearizes the free. The previous scheme (weak starts at 0; strong
     * side checks weak==0 after dec, weak side checks strong==0 after dec)
     * was a check-then-act race across two atomics: a racing last-strong /
     * last-weak release pair could both observe the other counter at 0 and
     * double-free (or free mid-drop_fn). Phase 27 Pitfall 4 still applies:
     * the field MUST be initialized here or fetch_add observes garbage. */
    IRON_ATOMIC_U64_INIT(rch->weak_count, 1);

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
    IRON_RC_COUNT_BUMP_RETAIN();  /* Phase 29 OPT-08: no-op unless IRON_RC_COUNT */
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
    IRON_RC_COUNT_BUMP_RELEASE();  /* Phase 29 OPT-08: no-op unless IRON_RC_COUNT */

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

        /* Release the strong cohort's collective weak (initialized to 1 in
         * iron_rc_alloc — Rust Arc scheme). Whoever takes weak_count to 0
         * frees the block, so a SINGLE counter linearizes the free. If real
         * weak refs remain, the header persists and upgrade() observes
         * refcount==0 via the Rust-Arc-canonical CAS loop (returns NULL);
         * the last weak release then frees. The previous protocol
         * (dec refcount then load weak_count here, dec weak_count then load
         * refcount in iron_weak_rc_release) was a check-then-act race across
         * two atomics: a racing last-strong/last-weak pair could both read
         * the other counter as 0 and double-free, or the weak side could
         * free the block while drop_fn above was still executing. */
        iron_weak_rc_release(user_ptr);
    }
}

/* ===== Phase 27 weak rc API ============================================= */

/* ── iron_weak_rc_retain — relaxed-inc weak_count bump ──────────────────────
 *
 * Atomic discipline (Phase 27 GA1):
 *   weak_count fetch_add: RELAXED. Iron does not surface get_mut-style
 *   exclusive-access APIs so Mara Bos's Acquire-on-weak-inc pairing is
 *   unnecessary. The block-free guard (acquire-load on refcount in
 *   iron_weak_rc_release) carries the cross-counter synchronization edge.
 *
 * Pitfall 4 mitigation: iron_rc_alloc initialises weak_count (to 1 — the
 * strong cohort's collective weak) so the first call after alloc never
 * observes garbage.
 *
 * Debug saturation guard: UINT64_MAX-1 retain count fires iron_oom_abort;
 * matches the precedent in iron_rc_retain (physically unreachable but
 * defines behavior over wrap).
 */
void iron_weak_rc_retain(void *user_ptr) {
    if (!user_ptr) return;
    Iron_RcHeader *rch = iron_rc_header_of(user_ptr);
#ifndef NDEBUG
    uint64_t cur = IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count);
    if (cur >= UINT64_MAX - 1) {
        iron_oom_abort("iron_weak_rc_retain: weak_count saturation (UINT64_MAX-1)");
    }
#endif
    (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(rch->weak_count, 1);
}

/* ── iron_weak_rc_release — release-dec weak_count; frees on the 1→0 edge ───
 *
 * Atomic discipline (Rust Weak::drop, library/alloc/src/sync.rs):
 *   weak_count fetch_sub: RELEASE — the decrementing holder's prior header
 *                         accesses happen-before the eventual free.
 *   on prev == 1:         ACQUIRE fence — pairs with every earlier
 *                         release-dec on weak_count (including the final
 *                         strong holder's collective-weak release, which is
 *                         sequenced after drop_fn), so all other holders'
 *                         accesses — and the destructor — happen-before the
 *                         free below.
 *
 * Block free condition: weak_count transitioned 1→0. Because the strong
 * cohort owns a collective weak (iron_rc_alloc inits weak_count=1) that is
 * only released AFTER the final strong release runs drop_fn, weak_count
 * reaching 0 implies refcount == 0 and payload destruction has completed.
 * No cross-counter load is needed — the old scheme's dec-then-load-other
 * protocol was a check-then-act race (see iron_rc_release).
 *
 * Debug underflow guard: prev == 0 indicates a double-release programmer
 * bug; fires iron_oom_abort deterministically (matches iron_rc_release
 * underflow handling).
 */
void iron_weak_rc_release(void *user_ptr) {
    if (!user_ptr) return;
    Iron_RcHeader *rch = iron_rc_header_of(user_ptr);
    uint64_t prev = IRON_ATOMIC_U64_FETCH_SUB_RELEASE(rch->weak_count, 1);
#ifndef NDEBUG
    if (prev == 0) {
        iron_oom_abort("iron_weak_rc_release: weak_count underflow "
                       "(programmer bug — double release?)");
    }
#endif
    if (prev == 1) {
        /* Last weak (including the strong cohort's collective weak) gone —
         * strong is necessarily 0 and drop_fn has run. This thread frees. */
        IRON_ATOMIC_FENCE_ACQUIRE();
        free((char *)user_ptr - IRON_RC_HEADER_TOTAL_SIZE);
    }
}

/* ── iron_rc_downgrade — strong rc → weak rc by bumping weak_count ──────────
 *
 * Atomic discipline (Phase 27 CONTEXT.md GA1):
 *   weak_count fetch_add: RELAXED. Iron does not surface get_mut so
 *   Mara Bos's Acquire/Release weak_count pairing is not needed — see
 *   docs/dev/RC-LAYOUT.md §8 for the divergence rationale.
 *
 * Returns the SAME user pointer the strong rc had. `weak rc T` is an alias
 * into the same allocation block, distinguished only by the static type
 * IRON_TYPE_WEAK_RC at the compiler level (lands in Plan 27-02).
 *
 * NULL discipline: NULL input → NULL output (Phase 19/26 mirror).
 *
 * Debug saturation guard: UINT64_MAX-1 weak_count fires iron_oom_abort
 * deterministically; mirrors iron_rc_retain's overflow band.
 */
void *iron_rc_downgrade(void *strong_user_ptr) {
    if (!strong_user_ptr) return NULL;
    Iron_RcHeader *rch = iron_rc_header_of(strong_user_ptr);
#ifndef NDEBUG
    uint64_t cur = IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->weak_count);
    if (cur >= UINT64_MAX - 1) {
        iron_oom_abort("iron_rc_downgrade: weak_count saturation (UINT64_MAX-1)");
    }
#endif
    (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(rch->weak_count, 1);
    return strong_user_ptr;
}

/* ── iron_rc_upgrade — Rust-Arc-canonical CAS loop reserving a strong ref ───
 *
 * Atomic discipline (Mara Bos "Building Our Own Arc" Ch. 6 +
 *                    Rust library/alloc/src/sync.rs Weak::upgrade):
 *   load:  ACQUIRE on refcount — synchronize-with prior release-dec from
 *          the holder that may have triggered destruction. This is the
 *          critical sync edge that lets Thread B (upgrader) observe Thread
 *          A's (strong-final-dropper) commitment to refcount=0.
 *   CAS:   RELAXED/RELAXED on both success and failure — the acquire-load
 *          above already established happens-before; the CAS itself does
 *          not need to be a synchronization point.
 *
 * Returns user_ptr (the strong rc T payload pointer) on successful CAS;
 * NULL when the loop observes refcount == 0 (covers mid-destructor race —
 * payload has been or is being destroyed; upgrade must return NULL).
 *
 * The Iron type system surfaces the NULL as T? (nullable strong rc) per
 * POL-09. Lands at parser/typecheck in Plan 27-02.
 *
 * Pitfall 1 (RESEARCH.md §7): the acquire-load + relaxed/relaxed CAS pattern
 * guarantees B never reserves a strong ref to a soon-to-be-destructed
 * payload. CAS-success implies B observed refcount > 0 strictly AFTER A's
 * release-dec (otherwise the acquire-load would have observed 0).
 *
 * Pitfall 10: u64 CAS macro is defined adjacent to IRON_ATOMIC_CAS_WEAK in
 * iron_runtime.h with POSIX (relaxed/relaxed) and Win32 (seq-cst via
 * InterlockedCompareExchange64) branches.
 */
void *iron_rc_upgrade(void *weak_user_ptr) {
    if (!weak_user_ptr) return NULL;   /* weak rc null → upgrade always null */
    Iron_RcHeader *rch = iron_rc_header_of(weak_user_ptr);
    uint64_t s = IRON_ATOMIC_U64_LOAD_ACQUIRE(rch->refcount);
    while (s != 0) {
        if (IRON_ATOMIC_U64_CAS_WEAK_RELAXED(rch->refcount, &s, s + 1)) {
            return weak_user_ptr;  /* race-won; new strong ref reserved */
        }
        /* CAS failed; the macro updated `s` to the observed value. Loop. */
    }
    return NULL;  /* refcount observed at 0 — payload destroyed (or in-flight). */
}
