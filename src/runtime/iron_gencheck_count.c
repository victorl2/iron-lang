/* iron_gencheck_count.c — Phase 30 Plan 30-01 (OPT-08 input): opt-in
 * generation-check counter.
 *
 * Behind IRON_GENCHECK_COUNT (OFF by default), three `_Atomic uint64_t`
 * counters tally the heap / stack / arena generation-check guards executed at
 * runtime (the three static-inline iron_check_*_pointer_gen helpers in
 * iron_runtime.h bump them, relaxed). They feed the deferred OPT-08 published
 * elision-rate report (Phase 36; docs/dev/POINTER-CHECK-ELISION.md).
 *
 * The macro must NOT alter the hot path in normal builds — when undefined the
 * IRON_GENCHECK_COUNT_BUMP_* macros expand to nothing and the accessors below
 * report zeros, so production deref performance and the deterministic
 * phase-invariant test counts are unaffected. This is the structural mirror of
 * iron_rc.c's IRON_RC_COUNT block (Phase 29 OPT-08).
 *
 * Relaxed ordering is correct for a pure observation counter: it tracks a
 * monotone op tally with no happens-before obligation toward the deref result.
 */

#include "runtime/iron_runtime.h"

#include <stdint.h>

#ifdef IRON_GENCHECK_COUNT
iron_atomic_u64 iron_gencheck_heap_count;
iron_atomic_u64 iron_gencheck_stack_count;
iron_atomic_u64 iron_gencheck_arena_count;
#endif

/* iron_gencheck_counts — read the opt-in heap/stack/arena check tallies.
 *
 * Always defined (stable symbol). When IRON_GENCHECK_COUNT is undefined all
 * out-params receive 0. NULL out-params are tolerated (partial read). */
void iron_gencheck_counts(uint64_t *heap, uint64_t *stack, uint64_t *arena) {
#ifdef IRON_GENCHECK_COUNT
    if (heap)  *heap  = IRON_ATOMIC_U64_LOAD_ACQUIRE(iron_gencheck_heap_count);
    if (stack) *stack = IRON_ATOMIC_U64_LOAD_ACQUIRE(iron_gencheck_stack_count);
    if (arena) *arena = IRON_ATOMIC_U64_LOAD_ACQUIRE(iron_gencheck_arena_count);
#else
    if (heap)  *heap  = 0;
    if (stack) *stack = 0;
    if (arena) *arena = 0;
#endif
}

/* iron_gencheck_counts_reset — zero the tallies. No-op when the macro is off. */
void iron_gencheck_counts_reset(void) {
#ifdef IRON_GENCHECK_COUNT
    IRON_ATOMIC_U64_INIT(iron_gencheck_heap_count, 0);
    IRON_ATOMIC_U64_INIT(iron_gencheck_stack_count, 0);
    IRON_ATOMIC_U64_INIT(iron_gencheck_arena_count, 0);
#endif
}
