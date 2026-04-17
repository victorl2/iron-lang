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

#endif /* IRON_LSP_TRANSPORT_TYPES_H */
