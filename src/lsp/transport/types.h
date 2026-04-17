#ifndef IRON_LSP_TRANSPORT_TYPES_H
#define IRON_LSP_TRANSPORT_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Transport-layer type skeletons for iron-lsp. Phase 2 Plan 01 establishes
 * enum tags and tagged-union shapes so later plans (02-02 framing, 02-03
 * dispatch) can reference them from the first test. Fields are filled in
 * as those plans land. */

typedef enum IronLsp_MsgKind {
    ILSP_MSG_INVALID = 0,
    ILSP_MSG_REQUEST,
    ILSP_MSG_RESPONSE,
    ILSP_MSG_NOTIFICATION
} IronLsp_MsgKind;

typedef enum IronLsp_ReqIdKind {
    ILSP_REQID_NULL = 0,
    ILSP_REQID_INT,
    ILSP_REQID_STR
} IronLsp_ReqIdKind;

typedef struct IronLsp_ReqId {
    IronLsp_ReqIdKind kind;
    union {
        int64_t     n;
        const char *s;
    } u;
} IronLsp_ReqId;

struct IronLsp_Message;

/* Writer queue priorities (Phase 2 Plan 02 Task 03).
 * Order matters for the drop policy: when the queue is full, the writer
 * drops the oldest ILSP_PRIO_LOG item first; if none exist, the oldest
 * ILSP_PRIO_NOTIFICATION; responses are only dropped if every slot holds
 * a response (an extreme back-pressure case that returns FULL_DROPPED). */
typedef enum IronLsp_Priority {
    ILSP_PRIO_RESPONSE     = 0,  /* highest -- never dropped while any
                                    lower-priority item can be dropped */
    ILSP_PRIO_NOTIFICATION = 1,  /* middle -- e.g. publishDiagnostics */
    ILSP_PRIO_LOG          = 2   /* lowest -- logMessage, $/progress */
} IronLsp_Priority;

/* Single outbound item owned by the writer queue. body is malloc'd by the
 * caller and free'd by the writer after flushing (or by the drop path). */
typedef struct IronLsp_OutboundItem {
    IronLsp_Priority prio;
    char            *body;
    size_t           len;
} IronLsp_OutboundItem;

#endif /* IRON_LSP_TRANSPORT_TYPES_H */
