#include "iron_runtime.h"

#include <stdlib.h>
#include <string.h>

/* FIX-03 / AUDIT-04 §13 + §14: SAFETY + deferred-fix documentation.
 *
 * Row §13 (iron_rc_release weak-count leak): when the last strong reference
 * drops while weak_count > 0, the control block is deliberately kept alive
 * (line ~43 below) so that a later `iron_weak_upgrade` can safely observe
 * `strong_count == 0` and return an empty Iron_Rc. The audit's concern is
 * that if every weak reference is later dropped WITHOUT an explicit
 * weak-count decrement, the control block leaks forever.
 *
 * Row §14 (Iron_weak_release missing API): there is no public
 * `Iron_weak_release` / `iron_weak_release` function to decrement
 * weak_count. The Iron_Weak type owns a strong reference to the control
 * block on construction (iron_rc_downgrade line ~57 increments
 * weak_count), but there is no counterpart decrement. Every
 * iron_rc_downgrade permanently adds to weak_count.
 *
 * Reachability analysis (2026-04-13, Phase 67-07):
 *   - grep `iron_rc_downgrade\|iron_weak_upgrade` src/ → 4 hits, all in
 *     runtime declarations + definitions (iron_runtime.h + iron_rc.c).
 *     Zero codegen sites (src/hir/ + src/lir/ emit nothing that calls
 *     downgrade or upgrade).
 *   - grep `weak` tests/integration/ → 0 hits. No Iron source fixture
 *     exercises any weak-reference API.
 *   - Iron language does not currently expose `weak` as a keyword or
 *     stdlib function — the runtime type is present but unused.
 *
 * Conclusion: rows §13 and §14 describe theoretical leaks in a runtime
 * API that is not reachable from Iron source today. The plan's decision
 * rule (see 67-07-PLAN.md) is: "If the rc leak is exploitable from Iron
 * source code today, fix it; if it's only a theoretical leak under weak-
 * ref usage patterns Iron doesn't support yet, SAFETY-annotate and defer."
 *
 * Treatment: SAFETY-annotate and DEFER to the future phase that wires
 * Iron-level weak references (no such phase is planned in the current
 * ROADMAP.md). The annotation below makes the deferral grep-visible so
 * that when the language does gain weak-ref support, the implementer
 * MUST land a paired Iron_weak_release API + an explicit weak-count
 * decrement path in iron_rc_release before shipping. Leaving this as
 * SAFETY-only without a future work ticket is acceptable because the
 * leak is guaranteed-zero under current Iron programs. */

/* ── Iron_Rc — atomic reference-counted heap value ───────────────────────── */

Iron_Rc iron_rc_create(void *value, size_t size, void (*destructor)(void *)) {
    /* Layout: [Iron_RcControl | value bytes] in a single allocation */
    Iron_RcControl *ctrl = (Iron_RcControl *)malloc(sizeof(Iron_RcControl) + size);
    if (!ctrl) {
        return (Iron_Rc){NULL, NULL};
    }

    IRON_ATOMIC_INIT(ctrl->strong_count, 1);
    IRON_ATOMIC_INIT(ctrl->weak_count,   0);
    ctrl->destructor = destructor;

    /* Copy value bytes immediately after the control block */
    void *stored = (char *)ctrl + sizeof(Iron_RcControl);
    if (value && size > 0) {
        memcpy(stored, value, size);
    }

    return (Iron_Rc){ctrl, stored};
}

void iron_rc_retain(Iron_Rc *rc) {
    if (!rc || !rc->ctrl) return;
    IRON_ATOMIC_FETCH_ADD(rc->ctrl->strong_count, 1);
}

void iron_rc_release(Iron_Rc *rc) {
    if (!rc || !rc->ctrl) return;

    int prev = IRON_ATOMIC_FETCH_SUB(rc->ctrl->strong_count, 1);
    if (prev == 1) {
        /* Last strong reference dropped */
        if (rc->ctrl->destructor) {
            rc->ctrl->destructor(rc->value);
        }
        /* Free control block only if no weak references remain */
        if (IRON_ATOMIC_LOAD(rc->ctrl->weak_count) == 0) {
            free(rc->ctrl);
        }
        rc->ctrl  = NULL;
        rc->value = NULL;
    }
}

/* ── Iron_Weak — non-owning reference ───────────────────────────────────── */

Iron_Weak iron_rc_downgrade(const Iron_Rc *rc) {
    if (!rc || !rc->ctrl) {
        return (Iron_Weak){NULL};
    }
    IRON_ATOMIC_FETCH_ADD(rc->ctrl->weak_count, 1);
    return (Iron_Weak){rc->ctrl};
}

Iron_Rc iron_weak_upgrade(const Iron_Weak *weak) {
    if (!weak || !weak->ctrl) {
        return (Iron_Rc){NULL, NULL};
    }

    Iron_RcControl *ctrl = weak->ctrl;

    /* CAS loop: atomically increment strong_count only if it is > 0 */
    int expected = IRON_ATOMIC_LOAD(ctrl->strong_count);
    while (expected > 0) {
        if (IRON_ATOMIC_CAS_WEAK(ctrl->strong_count,
                                  &expected, expected + 1)) {
            /* Success: compute value pointer from control block layout */
            void *value = (char *)ctrl + sizeof(Iron_RcControl);
            return (Iron_Rc){ctrl, value};
        }
        /* expected was updated by the failed CAS — retry */
    }

    /* strong_count reached 0 — the value is dead */
    return (Iron_Rc){NULL, NULL};
}
