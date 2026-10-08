#ifndef IRON_LSP_FACADE_NAV_NODE_AT_H
#define IRON_LSP_FACADE_NAV_NODE_AT_H

/* Phase 3 Plan 01 Task 04 (NAV-16) -- cursor -> AST node lookup.
 *
 * Given a sealed Iron_Program and an LSP Position (client-encoded), return
 * the innermost AST node whose Iron_Span covers the byte offset, or NULL
 * when the cursor is in whitespace / outside the file. This is the
 * primitive every NAV endpoint consumes (hover, definition, references,
 * highlight, rename...).
 *
 * The helper is pure -- zero mutation, zero global state -- and tolerates
 * Iron_ErrorNode children: they are skipped so siblings still participate
 * in the walk.
 */

#include "lsp/facade/types.h"  /* IronLsp_Position, IronLsp_PositionEncoding */
#include "parser/ast.h"        /* Iron_Program, Iron_Node typedefs */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls keep the header light. */
struct IronLsp_Document;

/* Return the innermost Iron_Node covering `pos` in `doc`: an identifier,
 * a method call or field access (cursor on the member name), a type
 * annotation, a literal, a binding, or the enclosing decl when the cursor
 * is on its name or keywords. NULL when:
 *   - doc or program is NULL
 *   - the cursor falls outside the file (line >= line count)
 *   - no decl span contains the computed byte offset (whitespace hit).
 *
 * The enc argument tells us how to decode pos.character into a byte
 * offset within the line; it must match the encoding the document was
 * mutated under (i.e. server->position_encoding). */
Iron_Node *ilsp_nav_node_at(const struct IronLsp_Document *doc,
                             const Iron_Program            *program,
                             IronLsp_Position               pos,
                             IronLsp_PositionEncoding       enc);

/* The declaration under `pos`: the smallest top-level decl covering it
 * (a method hoisted out of an object body rather than the object), or the
 * object field / interface signature / enum variant under it. NULL as for
 * ilsp_nav_node_at. */
Iron_Node *ilsp_nav_decl_at(const struct IronLsp_Document *doc,
                             const Iron_Program            *program,
                             IronLsp_Position               pos,
                             IronLsp_PositionEncoding       enc);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_NODE_AT_H */
