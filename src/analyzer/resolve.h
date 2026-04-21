#ifndef IRON_RESOLVE_H
#define IRON_RESOLVE_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

/* Run name resolution on the given program.
 * Builds scope tree, populates symbol tables, and sets resolved_sym on all
 * Iron_Ident nodes.
 * Returns the global scope (root of the scope tree).
 * Errors are accumulated in diags.
 */
Iron_Scope *iron_resolve(Iron_Program *program, Iron_Arena *arena,
                          Iron_DiagList *diags);

/* ── Phase 86 PATCH-02/04: program-global patch registry. ─────────────────── */

/* Opaque handle to the per-program type-patch registry. Built by
 * iron_type_patch_registry_build from an Iron_Program that has already been
 * parsed; consumed by the typechecker to (a) run the cross-patch collision
 * scan (E0255) and (b) extend method/init call dispatch to consider patched
 * members after the in-object lookup fails.
 *
 * Storage lifetime: the registry is allocated on the compilation arena by
 * iron_type_patch_registry_build. Its PatchEntry.methods[] arrays are
 * stb_ds dynamic arrays whose backing storage is heap-owned by stb_ds;
 * callers MUST invoke iron_type_patch_registry_free before arena teardown
 * to release the stb_ds top-level containers. The MethodDecl * elements
 * inside those arrays are SHARED with the Iron_Program arena — we never
 * free them directly. */
typedef struct Iron_TypePatchRegistry Iron_TypePatchRegistry;

/* Build a patch registry from a fully-parsed Iron_Program. Walks every
 * top-level Iron_ObjectDecl; entries where is_patch=true are keyed by
 * target_type_name and their patch-contributed MethodDecl * pointers are
 * gathered (sourced via the `type_name == target_type_name` MethodDecl
 * nodes Plan 86-01 flushed through extra_decls_out).
 *
 * Primitive target names (Int, Int32, Float, String, Bool) are accepted as
 * valid keys without requiring a corresponding IRON_NODE_OBJECT_DECL. A
 * patch target that is neither a user object nor a primitive in the
 * allowlist emits IRON_ERR_PATCH_TARGET_NOT_FOUND (E0254) into `diags` and
 * is NOT registered; the patch's methods remain syntactically intact but
 * are never dispatched.
 *
 * Caller owns the returned pointer; release via iron_type_patch_registry_free. */
Iron_TypePatchRegistry *iron_type_patch_registry_build(Iron_Program *program,
                                                       Iron_Scope    *global_scope,
                                                       Iron_Arena    *arena,
                                                       Iron_DiagList *diags);

/* Lookup all patched MethodDecl * contributions for a given target type.
 * Returns a stb_ds array of Iron_MethodDecl * pointers — possibly empty.
 * The array belongs to the registry; callers MUST NOT arrfree it. Order:
 * declaration order across patches (Plan 86-02 dispatch treats this as
 * defense-in-depth — the collision scan guarantees same-name uniqueness
 * across patches of the same target). */
Iron_MethodDecl **iron_type_patch_lookup(Iron_TypePatchRegistry *reg,
                                          const char              *target_type_name);

/* Release the registry: arrfree each PatchEntry.methods top-level stb_ds
 * container and the entries container itself. Does NOT free the
 * MethodDecl * elements (arena-owned). Safe to call on NULL. */
void iron_type_patch_registry_free(Iron_TypePatchRegistry *reg);

#endif /* IRON_RESOLVE_H */
