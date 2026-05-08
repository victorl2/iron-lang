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

#include "diagnostics/diagnostics.h"  /* iron_panic_stale_pointer + iron_panic_stale_stack_pointer */

/* Read IRON_PANIC_FORMAT env once and cache the result.
 * Idempotent across repeated iron_runtime_init calls (test-harness pattern).
 * Must be called BEFORE any allocation that could panic. */
void iron_panic_init_from_env(void);

/* Phase 20 PTR-10 (OQ-B Option C): stack-pointer panic helper.
 * Re-declared here (canonical declaration in diagnostics.h) so callers
 * inside src/runtime/ — notably the static-inline iron_check_stack_pointer_gen
 * in iron_runtime.h — can resolve the symbol with __attribute__((noreturn))
 * intact even when iron_runtime.h is included in TU's that don't pull
 * diagnostics.h into scope first. Definition in src/runtime/iron_panic.c. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void iron_panic_stale_stack_pointer(const char *deref_file,
                                    int deref_line,
                                    uint64_t captured_frame_gen);

#endif /* IRON_PANIC_H */
