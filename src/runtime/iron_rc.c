/* iron_rc.c — Phase 26 POL-06: rc policy runtime substrate.
 *
 * Plan 26-01 Task 1 lands the new API surface only (this file as a TEMPORARY
 * Wave 0 RED stub). Plan 26-01 Task 2 rewrites these bodies around
 * Iron_RcHeader + Phase 19 atomic discipline.
 *
 * The pre-v4 control-block-plus-value design (Iron_Rc, Iron_RcControl,
 * Iron_Weak, iron_rc_create / iron_rc_downgrade / iron_weak_upgrade) has been
 * removed entirely. Verification (Plan 26-01 substrate audit):
 *   - zero codegen call sites (grep iron_rc_retain\|iron_rc_release src/lir/
 *     src/hir/ src/cli/ returns zero hits in this tree at Phase 26 start)
 *   - the legacy API predated the Phase 19 atomic-ordering convention
 *     (relaxed-inc / acquire-load) and used iron_atomic_int instead of
 *     iron_atomic_u64 — incompatible memory-ordering policy
 *
 * Phase 26 atomic discipline (Rust Arc canonical, cross-verified against
 * RustBelt-Relaxed paper + mara.nl "Building Our Own Arc"):
 *   retain:  IRON_ATOMIC_U64_FETCH_ADD_RELAXED(refcount, 1)
 *   release: prev = IRON_ATOMIC_U64_FETCH_SUB_RELEASE(refcount, 1)
 *   if (prev == 1) {
 *       IRON_ATOMIC_FENCE_ACQUIRE();
 *       if (drop_fn) drop_fn(user_ptr);
 *       free(block);
 *   }
 *
 * Block layout: [Iron_RcHeader][IronAllocHdr][user payload].
 * User pointer = payload start (preserves Phase 19 ABI invariant).
 * Recovery: iron_rc_header_of(user) walks back
 *   sizeof(IronAllocHdr) + sizeof(Iron_RcHeader).
 *
 * Lock-document: docs/dev/RC-LAYOUT.md (Phase 26 Plan 26-01 closeout).
 */

#include "runtime/iron_runtime.h"

#include <stdlib.h>
#include <stddef.h>

/* ── Plan 26-01 Task 1 Wave 0 RED stubs ──────────────────────────────────────
 * These bodies are intentionally non-functional — they exist so that the
 * iron_runtime static library still links cleanly between Task 1 and Task 2.
 * Plan 26-01 Task 2 replaces every body below with the Iron_RcHeader-based
 * implementation. Until then, the Wave 0 unit tests (test_rc_layout,
 * test_rc_atomic_ordering, test_runtime_rc_concurrent) run-fail
 * deterministically — that IS the Wave 0 RED signal.
 *
 * Why stubs and not link-fail: leaving iron_rc.c with the old API would
 * break every other consumer of iron_runtime (every test, every binary).
 * The Wave 0 RED scope is the rc-specific tests, not the entire suite. */

void *iron_rc_alloc(size_t size, void (*drop_fn)(void *)) {
    (void)size;
    (void)drop_fn;
    return NULL;  /* RED: Task 2 implements the real allocation */
}

void iron_rc_retain(void *user_ptr) {
    (void)user_ptr;  /* RED: Task 2 implements the relaxed-inc */
}

void iron_rc_release(void *user_ptr) {
    (void)user_ptr;  /* RED: Task 2 implements release-dec + acquire-fence */
}

Iron_RcHeader *iron_rc_header_of(void *user_ptr) {
    (void)user_ptr;
    return NULL;  /* RED: Task 2 implements the pointer arithmetic */
}
