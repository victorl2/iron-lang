/* Phase 2 Plan 03 Task 03 -- per-request cancellation registry.
 *
 * stb_ds string-keyed hashmap (analog: src/analyzer/scope.c:8-72). Values
 * are heap-allocated _Atomic bool pointers. All three entry points hold
 * the registry's internal mutex for the duration of the map mutation;
 * the flag pointer itself is safe to read from any thread without the
 * lock because atomic_load_explicit(..., memory_order_relaxed) has no
 * data-race issue even if the value is concurrently flipped.
 *
 * Lock discipline: main dispatcher thread calls register / unregister.
 * The $/cancelRequest handler calls signal (also on the dispatcher
 * thread in Plans 03-05, but the lock is still taken so future Plans
 * may safely route cancellation through an auxiliary thread without
 * rework). */
#include "lsp/server/cancel.h"

#include "vendor/stb_ds.h"
#include "runtime/iron_runtime.h"  /* IRON_MUTEX_* */

#include <stdlib.h>
#include <string.h>

/* stb_ds string map: sh_new_strdup keys, _Atomic bool * values. */
typedef struct {
    char         *key;
    _Atomic bool *value;
} CancelEntry;

struct IronLsp_CancelRegistry {
    CancelEntry   *map;    /* stb_ds shmap */
    iron_mutex_t   lock;
};

IronLsp_CancelRegistry *ilsp_cancel_registry_create(void) {
    IronLsp_CancelRegistry *r = (IronLsp_CancelRegistry *)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->map = NULL;
    sh_new_strdup(r->map);
    IRON_MUTEX_INIT(r->lock);
    return r;
}

void ilsp_cancel_registry_destroy(IronLsp_CancelRegistry *r) {
    if (!r) return;
    /* Release any remaining flag allocations. */
    for (ptrdiff_t i = 0; i < shlen(r->map); i++) {
        free(r->map[i].value);
    }
    shfree(r->map);
    IRON_MUTEX_DESTROY(r->lock);
    free(r);
}

_Atomic bool *ilsp_cancel_register(IronLsp_CancelRegistry *r, const char *id_key) {
    if (!r || !id_key) return NULL;

    /* Heap-allocate the atomic outside the critical section. */
    _Atomic bool *flag = (_Atomic bool *)calloc(1, sizeof(*flag));
    if (!flag) return NULL;
    atomic_store_explicit(flag, false, memory_order_relaxed);

    IRON_MUTEX_LOCK(r->lock);
    /* If an entry already exists (re-register for same id) discard the
     * new flag and return the existing one -- idempotent. */
    ptrdiff_t idx = shgeti(r->map, id_key);
    if (idx >= 0) {
        _Atomic bool *existing = r->map[idx].value;
        IRON_MUTEX_UNLOCK(r->lock);
        free(flag);
        return existing;
    }
    shput(r->map, id_key, flag);
    IRON_MUTEX_UNLOCK(r->lock);
    return flag;
}

bool ilsp_cancel_signal(IronLsp_CancelRegistry *r, const char *id_key) {
    if (!r || !id_key) return false;

    IRON_MUTEX_LOCK(r->lock);
    ptrdiff_t idx = shgeti(r->map, id_key);
    _Atomic bool *flag = (idx >= 0) ? r->map[idx].value : NULL;
    IRON_MUTEX_UNLOCK(r->lock);

    if (!flag) return false;
    atomic_store_explicit(flag, true, memory_order_relaxed);
    return true;
}

void ilsp_cancel_unregister(IronLsp_CancelRegistry *r, const char *id_key) {
    if (!r || !id_key) return;

    IRON_MUTEX_LOCK(r->lock);
    ptrdiff_t idx = shgeti(r->map, id_key);
    _Atomic bool *flag = NULL;
    if (idx >= 0) {
        flag = r->map[idx].value;
        shdel(r->map, id_key);
    }
    IRON_MUTEX_UNLOCK(r->lock);

    free(flag);
}
