/* iron_panic.h — Phase 19 stale-pointer panic API (runtime-side).
 *
 * The user-facing declaration of iron_panic_stale_pointer lives in
 * src/diagnostics/diagnostics.h next to iron_oom_abort (single coherent
 * "how we die" surface). This header additionally exposes
 * iron_panic_init_from_env, called from iron_runtime_init in
 * src/runtime/iron_string.c to cache the IRON_PANIC_FORMAT environment
 * variable BEFORE any allocation can panic (Pitfall 6: getenv-once-at-
 * init).
 *
 * Definition: src/runtime/iron_panic.c
 * Layout / ABI lock: docs/dev/POINTER-LAYOUT.md (Plan 19-03 closeout).
 */
#ifndef IRON_PANIC_H
#define IRON_PANIC_H

#include "diagnostics/diagnostics.h"  /* iron_panic_stale_pointer */

/* Read IRON_PANIC_FORMAT env once and cache the result.
 * Idempotent across repeated iron_runtime_init calls (test-harness pattern).
 * Must be called BEFORE any allocation that could panic. */
void iron_panic_init_from_env(void);

#endif /* IRON_PANIC_H */
