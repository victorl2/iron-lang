#ifndef IRON_LSP_FACADE_EDIT_RENAME_COLLISION_H
#define IRON_LSP_FACADE_EDIT_RENAME_COLLISION_H

/* Phase 4 Plan 04-06 Task 01 (EDIT-12, D-10) -- rename collision check.
 *
 * Inputs:
 *   target         : the symbol being renamed (resolved at cursor)
 *   old_name       : target->name (snapshot; caller-owned)
 *   new_name       : the proposed new identifier text
 *   ref_site_scopes: enclosing Iron_Scope* at every reference site
 *                    (rename/apply.c gathers these via lazy analyze)
 *
 * Check order (LOCKED per D-10):
 *   1. new_name == NULL or empty   -> EMPTY_NAME
 *   2. new_name == old_name        -> SAME_NAME (caller emits empty
 *                                     WorkspaceEdit — no error, no edits)
 *   3. new_name matches Iron lexer
 *      keyword table               -> KEYWORD (keyword guard)
 *   4. for each scope in ref_site_scopes[]:
 *        if iron_scope_lookup(scope, new_name) resolves to a different
 *        Iron_Symbol than target  -> SCOPE_CONFLICT (first hit wins)
 *   5. otherwise                   -> NONE (safe to rename)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Iron_Symbol;
struct Iron_Scope;
struct IronLsp_WorkspaceIndex;

typedef enum {
    ILSP_COLLISION_NONE           = 0,
    ILSP_COLLISION_SAME_NAME      = 1,  /* new == old -> empty WorkspaceEdit */
    ILSP_COLLISION_EMPTY_NAME     = 2,
    ILSP_COLLISION_KEYWORD        = 3,  /* new_name is a lexer keyword */
    ILSP_COLLISION_SCOPE_CONFLICT = 4,  /* shadow in at least one ref-site scope */
} IronLsp_CollisionKind;

typedef struct IronLsp_CollisionResult {
    IronLsp_CollisionKind kind;
    /* Only valid when kind == SCOPE_CONFLICT: */
    const char *conflict_file;   /* arena-owned canonical_path */
    uint32_t    conflict_line;   /* 1-based */
    uint32_t    conflict_col;    /* 1-based */
    const char *conflict_name;   /* arena-owned; the colliding symbol's name */
} IronLsp_CollisionResult;

/* Check whether renaming `target` to `new_name` would produce a
 * collision anywhere in `ref_site_scopes`.
 *
 * Arguments:
 *   wi              : may be NULL (unused at the moment; kept for future
 *                     cross-entry conflict surfaces).
 *   target          : the symbol being renamed. May be NULL (treated
 *                     defensively as "no target" -> non-target scope
 *                     conflicts still fire).
 *   old_name        : target->name (caller-owned; comparison only).
 *   new_name        : proposed new name.
 *   ref_site_scopes : array of Iron_Scope* (one per use-site).
 *                     Duplicates allowed (per-file scopes often repeat);
 *                     the checker scans each.
 *   n_scopes        : count of ref_site_scopes[].
 *   arena           : for conflict-string interning. Required when
 *                     kind == SCOPE_CONFLICT.
 *   out             : required; zero-filled before dispatch.
 */
void ilsp_rename_collision_check(struct IronLsp_WorkspaceIndex *wi,
                                    const struct Iron_Symbol    *target,
                                    const char                  *old_name,
                                    const char                  *new_name,
                                    struct Iron_Scope * const   *ref_site_scopes,
                                    size_t                        n_scopes,
                                    Iron_Arena                   *arena,
                                    IronLsp_CollisionResult      *out);

/* Accessor used by unit tests and apply.c to centralise the keyword
 * list. Returns true iff `name` is one of Iron's 37 keywords (via the
 * configure_file-generated ILSP_COMPLETION_KEYWORDS mirror from Plan
 * 04-02). */
bool ilsp_rename_is_iron_keyword(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_EDIT_RENAME_COLLISION_H */
