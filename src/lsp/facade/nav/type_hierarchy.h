#ifndef IRON_LSP_FACADE_NAV_TYPE_HIERARCHY_H
#define IRON_LSP_FACADE_NAV_TYPE_HIERARCHY_H

/* Phase 3 Plan 05 Task 02 (NAV-11, D-08) -- typeHierarchy facade.
 *
 * Three-method LSP protocol:
 *   prepareTypeHierarchy        -- cursor -> single item (or empty)
 *   typeHierarchy/supertypes    -- item.data round-trip -> parents
 *   typeHierarchy/subtypes      -- item.data round-trip -> children
 *
 * All three share the IronLsp_TypeHierarchyItem shape (name, kind,
 * uri, range, selectionRange, triple) so JSON serialization is
 * symmetrical in both directions.  Clients echo back the `data` blob
 * verbatim -- the triple encodes the identity so the server never has
 * to re-walk the AST a second time to find what was prepared. */

#include "lsp/facade/nav/symbol_id.h"
#include "lsp/facade/types.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Server;
struct IronLsp_Document;

typedef struct IronLsp_TypeHierarchyItem {
    const char       *name;
    int               kind;             /* LSP SymbolKind */
    const char       *detail;           /* Phase 11 PATCH-02 (Plan 11-02 Task 1):
                                         * optional per LSP 3.17 spec.
                                         * NULL for native subtypes; non-NULL
                                         * "[patch from <module>]" for patch
                                         * method virtual entries (D-06). */
    const char       *uri;
    IronLsp_Range     range;
    IronLsp_Range     selection_range;
    IronLsp_SymbolId  triple;           /* round-trip into `data` on wire */
} IronLsp_TypeHierarchyItem;

void ilsp_facade_nav_prepare_type_hierarchy(
    struct IronLsp_Server        *server,
    struct IronLsp_Document      *doc,
    IronLsp_Position              pos,
    _Atomic bool                 *cancel,
    Iron_Arena                   *arena,
    IronLsp_TypeHierarchyItem   **out,
    size_t                       *out_n);

void ilsp_facade_nav_type_hierarchy_supertypes(
    struct IronLsp_Server        *server,
    IronLsp_SymbolId              item_triple,
    _Atomic bool                 *cancel,
    Iron_Arena                   *arena,
    IronLsp_TypeHierarchyItem   **out,
    size_t                       *out_n);

void ilsp_facade_nav_type_hierarchy_subtypes(
    struct IronLsp_Server        *server,
    IronLsp_SymbolId              item_triple,
    _Atomic bool                 *cancel,
    Iron_Arena                   *arena,
    IronLsp_TypeHierarchyItem   **out,
    size_t                       *out_n);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_TYPE_HIERARCHY_H */
