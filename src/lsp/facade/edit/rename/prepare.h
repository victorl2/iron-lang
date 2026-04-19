#ifndef IRON_LSP_FACADE_EDIT_RENAME_PREPARE_H
#define IRON_LSP_FACADE_EDIT_RENAME_PREPARE_H

/* Phase 4 Plan 04-06 Task 01 (EDIT-10, D-09) -- textDocument/prepareRename
 * facade.
 *
 * Classifies the cursor position into one of 5 outcomes (6 semantic
 * categories; two SILENT cases share a single outcome):
 *   ACCEPT             : cursor on a renameable ident -> {range, placeholder}
 *   REJECT_SILENT      : cursor on keyword/literal/whitespace/comment OR
 *                        cursor on ident with resolved_sym == NULL
 *                        (D-09 categories 1 + 2: null, NO showMessage)
 *   REJECT_STDLIB      : cursor on symbol whose decl lives in stdlib://
 *                        -> null + window/showMessage Info
 *   REJECT_DEP         : cursor on symbol whose decl lives in dep://
 *                        -> null + window/showMessage Info
 *   REJECT_EXTERN      : cursor on Iron_Symbol.is_extern == true
 *                        -> null + window/showMessage Info
 *   REJECT_BUILTIN_TYPE: cursor on a builtin primitive type alias
 *                        -> null + window/showMessage Info
 *
 * The 4 user-worth-explaining REJECT cases carry a `show_message` arena
 * string the handler layer forwards to window/showMessage (type=3 Info).
 */

#include "lsp/facade/types.h"  /* IronLsp_Position */
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Server;
struct IronLsp_Document;

typedef enum {
    ILSP_PREPARE_RENAME_ACCEPT            = 0,
    ILSP_PREPARE_RENAME_REJECT_SILENT     = 1,   /* D-09 categories 1 + 2 */
    ILSP_PREPARE_RENAME_REJECT_STDLIB     = 2,   /* D-09 category 3 */
    ILSP_PREPARE_RENAME_REJECT_DEP        = 3,   /* D-09 category 4 */
    ILSP_PREPARE_RENAME_REJECT_EXTERN     = 4,   /* D-09 category 5 */
    ILSP_PREPARE_RENAME_REJECT_BUILTIN_TYPE = 5, /* D-09 category 6 */
} IronLsp_PrepareRenameKind;

typedef struct IronLsp_PrepareRenameResult {
    IronLsp_PrepareRenameKind kind;
    /* Only valid when kind == ACCEPT: */
    IronLsp_Range              range;
    const char                *placeholder;  /* arena-owned; sym->name */
    /* Only valid when kind in
     * {REJECT_STDLIB, REJECT_DEP, REJECT_EXTERN, REJECT_BUILTIN_TYPE}: */
    const char                *show_message; /* arena-owned; "Cannot rename: ..." */
} IronLsp_PrepareRenameResult;

/* Classify the cursor. Writes the full result into *out.
 *
 * Arguments:
 *   server : may be NULL in unit tests (positionEncoding falls back to UTF-8).
 *   doc    : required; the open document hosting the cursor.
 *   pos    : LSP Position in the server's negotiated encoding.
 *   cancel : optional; polled once after analyze.
 *   arena  : caller-owned; all arena-owned out-strings allocated here.
 *   out    : required; zero-filled before dispatch.
 */
void ilsp_facade_prepare_rename(struct IronLsp_Server        *server,
                                  struct IronLsp_Document      *doc,
                                  IronLsp_Position              pos,
                                  _Atomic bool                 *cancel,
                                  Iron_Arena                   *arena,
                                  IronLsp_PrepareRenameResult  *out);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_EDIT_RENAME_PREPARE_H */
