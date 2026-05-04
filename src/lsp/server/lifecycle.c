/* Phase 2 Plan 03 Task 01 -- LSP lifecycle FSM implementation.
 *
 * Analog: src/analyzer/analyzer.c:35-115 (pipeline-stage dispatcher with
 * state-dependent early-return). The LSP FSM is structurally simpler --
 * each method either transitions forward, is a no-op, or is rejected. */
#include "lsp/server/lifecycle.h"

#include <string.h>

/* Internal helper: string equality for method names. */
static bool eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

bool ilsp_lifecycle_allow_request(IronLsp_LifecycleState s, const char *method) {
    if (!method) return false;

    /* `exit` is always allowed -- LSP 3.17 §exit is the sole way to
     * terminate cleanly. Even from UNINIT we honor it (exit-without-init
     * is a protocol error but we still exit with code 1). */
    if (eq(method, "exit")) return true;

    switch (s) {
        case ILSP_LIFECYCLE_UNINIT:
            /* Only `initialize` gets us out of the uninit state.
             * Everything else (including well-formed textDocument methods)
             * is rejected with -32002 ServerNotInitialized. */
            return eq(method, "initialize");

        case ILSP_LIFECYCLE_INITIALIZING:
            /* The client has sent `initialize` and received a response, but
             * has not yet confirmed with `initialized`. The spec forbids
             * any other request during this narrow window -- only
             * `initialized` (notification) is allowed to advance. */
            return eq(method, "initialized");

        case ILSP_LIFECYCLE_RUNNING:
            /* All normal traffic is allowed EXCEPT a duplicate initialize
             * (-32600 InvalidRequest per spec). `initialized` in RUNNING
             * is also a duplicate and treated as InvalidRequest. */
            if (eq(method, "initialize"))  return false;
            if (eq(method, "initialized")) return false;
            return true;

        case ILSP_LIFECYCLE_SHUTDOWN:
            /* After shutdown the only legal next message is `exit`. Any
             * other request must be rejected with -32600 per LSP 3.17. */
            return false;

        case ILSP_LIFECYCLE_EXIT_QUEUED:
            /* Terminal; the process is about to exit. Nothing further is
             * legal; any inbound message is dropped. */
            return false;
    }
    return false;
}

IronLsp_LifecycleState ilsp_lifecycle_next(IronLsp_LifecycleState s,
                                           const char *method) {
    if (!method) return s;

    if (s == ILSP_LIFECYCLE_UNINIT && eq(method, "initialize")) {
        return ILSP_LIFECYCLE_INITIALIZING;
    }
    if (s == ILSP_LIFECYCLE_INITIALIZING && eq(method, "initialized")) {
        return ILSP_LIFECYCLE_RUNNING;
    }
    if (s == ILSP_LIFECYCLE_RUNNING && eq(method, "shutdown")) {
        return ILSP_LIFECYCLE_SHUTDOWN;
    }
    if (eq(method, "exit")) {
        return ILSP_LIFECYCLE_EXIT_QUEUED;
    }
    return s;
}

const char *ilsp_lifecycle_state_name(IronLsp_LifecycleState s) {
    switch (s) {
        case ILSP_LIFECYCLE_UNINIT:       return "UNINIT";
        case ILSP_LIFECYCLE_INITIALIZING: return "INITIALIZING";
        case ILSP_LIFECYCLE_RUNNING:      return "RUNNING";
        case ILSP_LIFECYCLE_SHUTDOWN:     return "SHUTDOWN";
        case ILSP_LIFECYCLE_EXIT_QUEUED:  return "EXIT_QUEUED";
    }
    return "UNKNOWN";
}
