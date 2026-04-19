#ifndef IRON_LSP_FACADE_EDIT_RENAME_APPLY_H
#define IRON_LSP_FACADE_EDIT_RENAME_APPLY_H

/* Phase 4 Plan 04-06 Task 02 (EDIT-11, D-11) -- textDocument/rename
 * facade. Builds a WorkspaceEdit (documentChanges preferred, changes
 * fallback) covering every reference to the target symbol workspace-
 * wide.
 *
 * Outcomes:
 *   SUCCESS                    : WorkspaceEdit built; caller emits JSON
 *   FAIL_COLLISION             : scope / keyword / empty-name conflict
 *   FAIL_STDLIB_IMPLEMENTOR    : PITFALL B — iface method has a stdlib
 *                                implementor; whole rename rejected
 *   FAIL_DEP_IMPLEMENTOR       : PITFALL B — iface method has a dep
 *                                implementor; whole rename rejected
 *
 * On FAIL_* the caller emits JSON-RPC -32803 RequestFailed.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lsp/facade/edit/rename/collision.h"
#include "lsp/facade/types.h"
#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Server;
struct IronLsp_Document;

/* A single TextEdit inside a per-file edit list. Per-file entries are
 * sorted DESC by range.start offset so applying them left-to-right
 * doesn't invalidate earlier ranges (D-11 invariant). */
typedef struct IronLsp_RenameTextEdit {
    IronLsp_Range  range;        /* encoding-aware LSP Range */
    const char    *new_text;     /* arena-owned */
} IronLsp_RenameTextEdit;

/* One file's contribution to the WorkspaceEdit. */
typedef struct IronLsp_RenameFileEdit {
    const char             *uri;          /* arena-owned file:// URI */
    int32_t                 version;      /* doc->version when open; -1 when closed */
    IronLsp_RenameTextEdit *edits;        /* array; len = edits_n */
    size_t                  edits_n;
} IronLsp_RenameFileEdit;

typedef enum {
    ILSP_RENAME_SUCCESS                 = 0,
    ILSP_RENAME_FAIL_COLLISION          = 1,  /* D-10 */
    ILSP_RENAME_FAIL_STDLIB_IMPLEMENTOR = 2,  /* PITFALL B stdlib */
    ILSP_RENAME_FAIL_DEP_IMPLEMENTOR    = 3,  /* PITFALL B dep */
    ILSP_RENAME_FAIL_CANCELLED          = 4,  /* cancel flag observed */
} IronLsp_RenameOutcome;

typedef struct IronLsp_RenameResult {
    IronLsp_RenameOutcome     outcome;
    /* Only valid when outcome == SUCCESS: */
    IronLsp_RenameFileEdit   *files;                /* array cross-file ASC by URI */
    size_t                    files_n;
    bool                      use_document_changes; /* snapshot of server cap */
    /* Only valid when outcome == FAIL_COLLISION: */
    IronLsp_CollisionResult   collision;
    /* Only valid when outcome in {FAIL_STDLIB_IMPLEMENTOR, FAIL_DEP_IMPLEMENTOR}: */
    const char               *fail_location;         /* arena-owned: path:line:col */
    const char               *fail_message;          /* arena-owned user-facing */
} IronLsp_RenameResult;

/* Build the WorkspaceEdit for renaming the cursor symbol to `new_name`.
 * Arguments:
 *   server    : may be NULL for unit testing (use_document_changes=false)
 *   doc       : open document hosting the cursor (required)
 *   pos       : cursor position
 *   new_name  : proposed new identifier text
 *   cancel    : optional cancel flag; polled at safe points
 *   arena     : caller-owned arena for result strings
 *   out       : required; zero-filled before dispatch
 */
void ilsp_facade_rename(struct IronLsp_Server   *server,
                          struct IronLsp_Document *doc,
                          IronLsp_Position         pos,
                          const char              *new_name,
                          _Atomic bool            *cancel,
                          Iron_Arena              *arena,
                          IronLsp_RenameResult    *out);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_EDIT_RENAME_APPLY_H */
