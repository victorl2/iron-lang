#ifndef IRON_LSP_FACADE_NAV_VISIBILITY_H
#define IRON_LSP_FACADE_NAV_VISIBILITY_H

/* Phase 10 Plan 10-01 (VIS-01..04, D-02) -- LSP-only visibility adapter.
 *
 * The v3 AST is non-uniform: Iron_Field carries positive `is_pub`, while
 * Iron_FuncDecl / Iron_MethodDecl carry the v2-inverse `is_private`.
 * Iron_ObjectDecl / Iron_InterfaceDecl / Iron_EnumDecl / Iron_ValDecl /
 * Iron_VarDecl have NO visibility axis at all -- the parser drops the
 * `private` keyword for those (parser.c:4047, 4606, 4737). Phase 10
 * does NOT add `is_pub` to those decls (D-01); it adapts at the LSP
 * boundary.
 *
 *   ilsp_vis_is_public(decl_node)
 *     true iff `decl_node` is publicly visible across modules.
 *     NULL -> false. Non-decl kinds (params/locals/expressions/decls
 *     without a visibility axis) -> true (not addressable cross-file).
 *
 *   ilsp_vis_can_see(decl_canonical, requester_canonical, decl_node)
 *     Cross-module gate. Order:
 *       - decl_canonical/requester_canonical NULL                 -> true
 *       - decl_canonical == requester_canonical (pointer equality) -> true
 *       - strcmp(decl_canonical, requester_canonical) == 0          -> true
 *       - ilsp_nav_path_is_stdlib(decl_canonical)                  -> true (D-08)
 *       - ilsp_vis_is_public(decl_node)                            -> true
 *       - otherwise                                                 -> false
 *
 * Both functions are pure: no allocation, no I/O, no globals. Safe
 * to call concurrently from any request thread (per CLAUDE.md
 * "Concurrency: requests may run in parallel on shared `iron_compiler`
 * state -- all state must be scoped per request/document"). */

#include "parser/ast.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true when `decl_node` is publicly visible across modules.
 * NULL input -> false. Non-decl kinds -> true (params/locals are
 * scope-local; cross-module visibility is moot). */
bool ilsp_vis_is_public(const Iron_Node *decl_node);

/* Cross-module gate. Both paths SHOULD be the canonical realpath form
 * already populated by workspace_index (see workspace_index.h:67). The
 * pointer-equality fast path falls out of strcmp (Phase 3 NAV-15 arena
 * interning). NULL paths default-allow per D-04 (open-doc-without-
 * canonical-path treated as same-module). */
bool ilsp_vis_can_see(const char       *decl_canonical,
                      const char       *requester_canonical,
                      const Iron_Node  *decl_node);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_VISIBILITY_H */
