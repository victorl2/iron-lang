/* Phase 3 Plan 03 Task 02 (NAV-03) -- textDocument/declaration facade.
 *
 * Iron has no separate decl-vs-def split except `extern func` symbols
 * (where the declaration IS the prototype). For every symbol:
 *   - non-extern: declaration == definition; delegate to
 *     ilsp_facade_nav_definition.
 *   - extern:     the decl_node IS the prototype span; the definition
 *                 path would also return the same node, so this is
 *                 effectively a no-op alias.
 *
 * Rather than re-implement the resolver, we delegate directly and
 * rely on the facade's deterministic output. */

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

void ilsp_facade_nav_declaration(IronLsp_Server        *server,
                                  IronLsp_Document      *doc,
                                  IronLsp_Position       pos,
                                  _Atomic bool          *cancel,
                                  Iron_Arena            *arena,
                                  IronLsp_LocationLink **out_links,
                                  size_t                *out_n) {
    /* Same resolver as definition. Phase 3 contract: when the resolved
     * symbol is extern, the decl_node is the prototype -- identical
     * return shape. */
    ilsp_facade_nav_definition(server, doc, pos, cancel, arena,
                                out_links, out_n);
}
