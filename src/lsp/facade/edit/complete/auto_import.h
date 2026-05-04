#ifndef ILSP_AUTO_IMPORT_H
#define ILSP_AUTO_IMPORT_H

/* Phase 4 Plan 04-03 Task 01 (EDIT-05, D-02) -- additionalTextEdits
 * builder for auto-import on bucket 4 (stdlib) and bucket 5 (deps)
 * completion candidates.
 *
 * Locked behaviour (04-CONTEXT.md D-02):
 *   - Walk Iron_Program->decls[] while decls[i]->kind ==
 *     IRON_NODE_IMPORT_DECL. Skip IRON_NODE_ERROR and any import whose
 *     span.end_line runs past the next top-level decl (mid-edit per
 *     PITFALL C).
 *   - Insertion site = line immediately AFTER the last valid anchor.
 *     0-indexed (LSP Position.line). If no valid anchor exists,
 *     insertion site = line 0 col 0 with newText "import <mod>\n\n"
 *     (trailing blank line per D-02 "start of file with a trailing
 *     blank line").
 *   - Dedup-on-build: if the file already has `import <mod>` (bare,
 *     no alias), emit NO edit.
 *   - Alias honour: if the file already has `import <mod> as <y>`,
 *     emit NO edit and return the alias in *out_alias — caller
 *     rewrites insertText to use the alias prefix.
 *   - Nested module paths (containing '.') — return no edit per
 *     CONTEXT.md Deferred Ideas.
 *   - newText ends with '\n' (LF); client normalises to file EOL.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "parser/ast.h"      /* Iron_Program (typedef to anonymous struct) */
#include "util/arena.h"      /* Iron_Arena (typedef to anonymous struct) */

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Document;

typedef struct {
    /* LSP TextEdit: zero-width range at (line, character) + newText.
     * When new_text == NULL the struct represents "no edit needed". */
    uint32_t    line;       /* 0-indexed LSP Position.line */
    uint32_t    character;  /* 0-indexed LSP Position.character (byte-col) */
    const char *new_text;   /* arena-owned; ends with '\n' (or NULL) */
} IronLsp_AutoImportEdit;

/* Compute the additionalTextEdits needed to bring `module_stem` into
 * scope.
 * Outputs:
 *   out_edit->new_text == NULL   -> no edit needed (already imported,
 *                                   aliased, or nested path).
 *   out_edit->new_text != NULL   -> single insertion TextEdit at
 *                                   (out_edit->line, out_edit->character).
 *   *out_alias != NULL           -> module is already aliased; caller
 *                                   should rewrite insertText to use
 *                                   this alias instead of module_stem.
 * `out_alias` may be NULL (caller does not care about alias). */
void ilsp_auto_import_edit(const Iron_Program           *program,
                            const struct IronLsp_Document *doc,
                            const char                   *module_stem,
                            Iron_Arena                   *arena,
                            IronLsp_AutoImportEdit       *out_edit,
                            const char                  **out_alias);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_AUTO_IMPORT_H */
