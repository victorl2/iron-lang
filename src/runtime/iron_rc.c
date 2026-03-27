#include "iron_runtime.h"

#include <stdlib.h>
#include <string.h>

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
