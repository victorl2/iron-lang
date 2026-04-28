#ifndef IRON_LSP_FACADE_NAV_PATCH_LOOKUP_H
#define IRON_LSP_FACADE_NAV_PATCH_LOOKUP_H

/* Phase 11 Plan 11-01 (PATCH-01..05, D-15) -- LSP-only helper module
 * exposing patch-extension semantics ALREADY computed by the compiler
 * (src/analyzer/resolve.{c,h}) to 5 LSP feature surfaces (PATCH-01..05).
 *
 * 3 helpers consumed by 5 call sites:
 *   - ilsp_patch_enclosing_for_method(program, md, wi)
 *       Returns the enclosing patch ObjectDecl for `md`, or NULL if `md`
 *       is not patch-contributed. Searches `program` first; if `wi`
 *       is non-NULL, walks workspace_index snapshot_paths next.
 *       Used by: hover.c (PATCH-04), references.c (PATCH-05).
 *
 *   - ilsp_patch_target_matches_interface(patch_od, ifc, program)
 *       Returns true if the patch's target type is a user-declared
 *       object whose `implements_names[]` contains `ifc->name`.
 *       Returns false for primitive targets (Int/Int32/Float/String/Bool).
 *       Used by: implementation.c (PATCH-01).
 *
 *   - ilsp_patch_for_each_method(program, wi, target_type_name,
 *                                 requester_canonical_path, visit, ud,
 *                                 cancel)
 *       Iterate every patched method on `target_type_name` reachable
 *       from `requester_canonical_path`. Builds Iron_TypePatchRegistry
 *       per program, frees on exit (Pitfall 1). Visibility filter applied
 *       internally via ilsp_vis_can_see on the enclosing patch ObjectDecl.
 *       The visitor returns false to short-circuit. Returns count visited.
 *       Used by: implementation.c (PATCH-01), type_hierarchy.c (PATCH-02),
 *       complete/buckets.c (PATCH-03), references.c (PATCH-05).
 *
 * Cross-file iteration MUST go through workspace_index per RESEARCH
 * Conflict 1 (CONTEXT D-02 said dep_map but IronLsp_DepEntry carries no
 * parsed Iron_Program; only IronLsp_IndexEntry does).
 *
 * Visibility predicate is applied with the enclosing patch ObjectDecl
 * as decl_node. Per RESEARCH Conflict 3, this is functionally a no-op
 * for ObjectDecl today (no is_private/is_pub axis); forward-compat
 * shape activates when a future grammar phase adds patch-level
 * visibility.
 *
 * All helpers are pure (no globals, no I/O) and per-request scoped.
 * Safe to call concurrently from any request thread (CLAUDE.md
 * "Concurrency: requests may run in parallel on shared `iron_compiler`
 * state -- all state must be scoped per request/document"). */

#include "parser/ast.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/* Forward declaration to avoid pulling workspace_index.h through every
 * consumer. Definition lives in src/lsp/store/workspace_index.h. */
typedef struct IronLsp_WorkspaceIndex IronLsp_WorkspaceIndex;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the enclosing patch ObjectDecl for `md`, or NULL.
 * `wi` may be NULL (unit-test path: same-program lookup unconditionally,
 * cross-file gated on wi != NULL). */
Iron_ObjectDecl *ilsp_patch_enclosing_for_method(
    Iron_Program             *program,
    Iron_MethodDecl          *md,
    IronLsp_WorkspaceIndex   *wi);

/* Returns true if `patch_od->target_type_name` resolves to a user-declared
 * object in `program` whose implements_names contains `ifc->name`.
 * Returns false for primitive targets (Int/Int32/Float/String/Bool) and
 * for any target that is not a user-declared Iron_ObjectDecl. */
bool ilsp_patch_target_matches_interface(
    Iron_ObjectDecl          *patch_od,
    const Iron_InterfaceDecl *ifc,
    Iron_Program             *program);

/* Iterate every patched method on `target_type_name` reachable from
 * `requester_canonical_path`. Visitor returns false to short-circuit.
 * `wi` may be NULL (same-program only); `cancel` may be NULL. Returns
 * count of methods visited (after visibility filter). */
size_t ilsp_patch_for_each_method(
    Iron_Program             *program,
    IronLsp_WorkspaceIndex   *wi,
    const char               *target_type_name,
    const char               *requester_canonical_path,
    bool (*visit)(Iron_MethodDecl *md, Iron_ObjectDecl *patch_od, void *ud),
    void                     *userdata,
    _Atomic bool             *cancel);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_PATCH_LOOKUP_H */
