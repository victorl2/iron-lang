/* Phase 10 Plan 10-01 (VIS-01..04, D-02) -- LSP-only visibility adapter
 * implementation. Per RESEARCH Conflict 3, only 3 decl kinds carry a
 * visibility bit in the rebased v3 AST; all others default-true.
 *
 * Two pure functions:
 *   ilsp_vis_is_public  -- per-decl-kind switch over the v3 AST
 *   ilsp_vis_can_see    -- cross-module gate (path equality +
 *                          stdlib carve-out + visibility)
 *
 * No allocation, no globals, no I/O. NULL inputs return defensively
 * (false for is_public; true for can_see same-module-without-canonical
 * graceful fallback per D-04 Pitfall 5). Phase 1 HARD-04 abort-audit
 * posture preserved -- no iron_ice / iron_oom_abort sites added. */

#include "lsp/facade/nav/visibility.h"

#include "lsp/facade/nav/nav_common.h"  /* ilsp_nav_path_is_stdlib (D-08) */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

bool ilsp_vis_is_public(const Iron_Node *d) {
    if (!d) return false;
    /* Cast to int to silence -Werror=switch-enum on the explicit default
     * arm; the analyzer convention (e.g. buckets.c:104, workspace_symbol.c:62)
     * uses the same cast for partial-enum switches. */
    switch ((int)d->kind) {
        case IRON_NODE_FIELD:
            return ((const Iron_Field *)d)->is_pub;
        case IRON_NODE_FUNC_DECL:
            return !((const Iron_FuncDecl *)d)->is_private;
        case IRON_NODE_METHOD_DECL:
            return !((const Iron_MethodDecl *)d)->is_private;
        default:
            /* RESEARCH Conflict 3: ObjectDecl/InterfaceDecl/EnumDecl/
             * ValDecl/VarDecl have NO is_private field; parser drops
             * the keyword. Params/locals are scope-local. All default
             * to true because they are not cross-file-hideable. */
            return true;
    }
}

bool ilsp_vis_can_see(const char       *decl_canonical,
                      const char       *requester_canonical,
                      const Iron_Node  *decl_node) {
    /* D-04 + Pitfall 5 defensive default-allow when paths are
     * unavailable (open-doc-without-canonical-path treated as
     * same-module). */
    if (!decl_canonical || !requester_canonical) return true;
    /* D-13 pointer-equality fast path; arena-interned canonicals
     * may compare equal by pointer (Phase 3 NAV-15). */
    if (decl_canonical == requester_canonical) return true;
    if (strcmp(decl_canonical, requester_canonical) == 0) return true;
    /* XXX_PHASE_14 MIG-01: stdlib carve-out lives until MIG-01 stamps
     * `pub` onto the stdlib .iron surface (src/stdlib + src/runtime).
     * After that flip this branch is removed and stdlib symbols obey
     * the same visibility predicate as user-workspace symbols. */
    if (ilsp_nav_path_is_stdlib(decl_canonical)) return true;
    return ilsp_vis_is_public(decl_node);
}
