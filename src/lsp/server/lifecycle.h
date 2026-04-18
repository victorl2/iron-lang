#ifndef IRON_LSP_SERVER_LIFECYCLE_H
#define IRON_LSP_SERVER_LIFECYCLE_H

/* Phase 2 Plan 03 Task 01 -- LSP lifecycle FSM.
 *
 * LSP 3.17 §General Messages §Lifecycle defines a 5-state machine:
 *   UNINIT -> INITIALIZING -> RUNNING -> SHUTDOWN -> EXIT_QUEUED
 *
 * Entry transitions:
 *   UNINIT        + initialize  -> INITIALIZING
 *   INITIALIZING  + initialized -> RUNNING
 *   RUNNING       + shutdown    -> SHUTDOWN
 *   SHUTDOWN      + exit        -> EXIT_QUEUED
 *
 * Gates (ilsp_lifecycle_allow_request):
 *   UNINIT        : allow only `initialize` and `exit`
 *   INITIALIZING  : allow only `initialized` and `exit`
 *   RUNNING       : allow every method EXCEPT duplicate `initialize`
 *   SHUTDOWN      : allow only `exit`
 *   EXIT_QUEUED   : allow nothing (terminal; process is exiting)
 *
 * This is a pure-function API; no mutation. Callers (dispatcher) own the
 * state variable and apply transitions on their side. */

#include <stdbool.h>

typedef enum IronLsp_LifecycleState {
    ILSP_LIFECYCLE_UNINIT       = 0,
    ILSP_LIFECYCLE_INITIALIZING,   /* initialize received; initialized not yet */
    ILSP_LIFECYCLE_RUNNING,
    ILSP_LIFECYCLE_SHUTDOWN,
    ILSP_LIFECYCLE_EXIT_QUEUED
} IronLsp_LifecycleState;

/* Pure state-guard check. Returns true if the method is allowed in the
 * given state. Does not mutate anything. */
bool ilsp_lifecycle_allow_request(IronLsp_LifecycleState s, const char *method);

/* Compute the successor state after a successful handler invocation.
 * If the method does not drive a transition (e.g. `$/cancelRequest` in
 * RUNNING), returns the same state. */
IronLsp_LifecycleState ilsp_lifecycle_next(IronLsp_LifecycleState s,
                                           const char *method);

/* Human-readable state name for logging / diagnostics. */
const char *ilsp_lifecycle_state_name(IronLsp_LifecycleState s);

#endif /* IRON_LSP_SERVER_LIFECYCLE_H */
