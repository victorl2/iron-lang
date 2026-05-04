/* Phase 11 Plan 11-01 (PATCH-01..05, D-15) -- patch_lookup helpers.
 *
 * Same-program scans use the symbol_id.c:139-156 linear-search idiom.
 * Cross-file iteration uses the workspace_index snapshot_paths walk
 * (mirrors type_hierarchy.c:412-449). RESEARCH Conflict 1: cross-file
 * routes through workspace_index (parsed programs), NOT dep_map
 * (export-name lists only).
 *
 * Per-request registry build/free pairing per Pitfall 1: every call
 * to iron_type_patch_registry_build is matched with
 * iron_type_patch_registry_free in the SAME function scope. Linear
 * search is preferred over registry-build when only the enclosing
 * ObjectDecl is needed (see ilsp_patch_enclosing_for_method). */

#include "lsp/facade/nav/patch_lookup.h"

#include "lsp/facade/nav/visibility.h"
#include "lsp/store/workspace_index.h"
#include "analyzer/resolve.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal helpers ─────────────────────────────────────────────── */

/* Linear scan over program->decls[] for a patch ObjectDecl whose
 * target_type_name matches `md->type_name`. Mirrors the
 * symbol_id.c:139-156 idiom. Sealed-tree contract: read-only AST
 * access. */
static Iron_ObjectDecl *patch_enclosing_in_program(
    Iron_Program    *program,
    Iron_MethodDecl *md)
{
    if (!program || !md || !md->type_name) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
        if (!od->is_patch) continue;
        if (!od->target_type_name) continue;
        if (strcmp(od->target_type_name, md->type_name) == 0) {
            return od;
        }
    }
    return NULL;
}

/* Linear scan for a NON-patch user-declared Iron_ObjectDecl by name.
 * Returns NULL when `name` resolves to a primitive (no Iron_ObjectDecl
 * exists for primitives in program->decls) -- exactly the desired
 * behavior per CONTEXT D-03 ("primitives don't implement interfaces
 * today"). */
static const Iron_ObjectDecl *find_user_object_by_name(
    Iron_Program *program,
    const char   *name)
{
    if (!program || !name) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        const Iron_ObjectDecl *od = (const Iron_ObjectDecl *)d;
        if (od->is_patch) continue;
        if (!od->name) continue;
        if (strcmp(od->name, name) == 0) return od;
    }
    return NULL;
}

/* ── ilsp_patch_enclosing_for_method ──────────────────────────────── */

Iron_ObjectDecl *ilsp_patch_enclosing_for_method(
    Iron_Program             *program,
    Iron_MethodDecl          *md,
    IronLsp_WorkspaceIndex   *wi)
{
    if (!md) return NULL;

    /* Same-program lookup first: avoids the heap-malloc'd registry
     * build (Pitfall 1) when the patch lives in the requester file. */
    Iron_ObjectDecl *here = patch_enclosing_in_program(program, md);
    if (here) return here;

    if (!wi) return NULL;

    /* Cross-program walk via workspace_index snapshot paths
     * (RESEARCH Conflict 1: dep_map carries no parsed programs). */
    size_t n = 0;
    char **paths = ilsp_workspace_index_snapshot_paths(wi, &n);
    if (!paths) return NULL;

    Iron_ObjectDecl *result = NULL;
    for (size_t i = 0; i < n && !result; i++) {
        if (!paths[i]) continue;
        IronLsp_IndexEntry *e = ilsp_workspace_index_lookup(wi, paths[i]);
        if (!e || !e->program) continue;
        if (!e->analyzed) {
            (void)ilsp_workspace_index_analyze_lazy(wi, e, NULL);
        }
        if (e->program == program) continue;  /* same program already done */
        result = patch_enclosing_in_program(e->program, md);
    }

    for (size_t i = 0; i < n; i++) free(paths[i]);
    free(paths);
    return result;
}

/* ── ilsp_patch_target_matches_interface ─────────────────────────── */

bool ilsp_patch_target_matches_interface(
    Iron_ObjectDecl          *patch_od,
    const Iron_InterfaceDecl *ifc,
    Iron_Program             *program)
{
    if (!patch_od || !ifc || !program) return false;
    if (!patch_od->is_patch || !patch_od->target_type_name) return false;
    if (!ifc->name) return false;

    /* Resolve target_type_name to a user object in `program`. Primitives
     * are not Iron_ObjectDecl entries in program->decls, so the lookup
     * misses for primitive targets -- exactly the desired behavior per
     * CONTEXT D-03 ("primitives don't implement interfaces today"). */
    const Iron_ObjectDecl *target =
        find_user_object_by_name(program, patch_od->target_type_name);
    if (!target) return false;

    for (int j = 0; j < target->implements_count; j++) {
        if (target->implements_names[j] &&
            strcmp(target->implements_names[j], ifc->name) == 0) {
            return true;
        }
    }
    return false;
}

/* ── ilsp_patch_for_each_method ──────────────────────────────────── */

/* Visit every patched method on `target_type_name` in a single program.
 *
 * DEVIATION (Rule 3): CONTEXT D-01 prescribed building the
 * Iron_TypePatchRegistry to enumerate methods, but
 * iron_type_patch_registry_build (a) requires `global_scope` to validate
 * user-object targets (resolve.c:1115-1121 only sets
 * `target_is_user_object=true` when global_scope can resolve the target
 * to an IRON_NODE_OBJECT_DECL Iron_Symbol) AND (b) calls iron_diag_emit
 * on the diags pointer unconditionally when the target validates as
 * neither primitive nor user-object (resolve.c:1130). Both make the
 * registry unusable from the LSP boundary where global_scope isn't
 * available and diags are intentionally suppressed.
 *
 * We instead walk `program->decls` directly: for every top-level
 * Iron_MethodDecl whose `type_name` matches `target_type_name`, find
 * the enclosing patch ObjectDecl via patch_enclosing_in_program (the
 * symbol_id.c:139-156 idiom). This mirrors the parser's flushing
 * convention: patched method bodies are emitted as top-level
 * Iron_MethodDecl nodes in program->decls with type_name set to the
 * patch target. Rule 1 fix preserves the public helper signature; the
 * registry build is no longer load-bearing here. Pitfall 1 (build/free
 * pairing) still applies to plans 11-02..03 if they choose registry
 * directly. */
static void for_each_method_in_program(
    Iron_Program     *program,
    Iron_Arena       *req_arena,
    const char       *target_type_name,
    const char       *requester_canonical_path,
    bool (*visit)(Iron_MethodDecl *md, Iron_ObjectDecl *patch_od, void *ud),
    void             *userdata,
    size_t           *out_visited,
    bool             *out_stop)
{
    (void)req_arena;
    if (!program || !visit || !target_type_name) return;

    /* Mirror the resolve.c:1165-1195 flush-aware walk. Patch ObjectDecls
     * are followed by their flushed METHOD_DECLs in declaration order;
     * methods whose type_name matches `last_patch_target` belong to
     * that patch body. A non-patch ObjectDecl (or a non-method/non-func
     * decl) clears `last_patch_target` so a subsequent native method on
     * the same type name is NOT misattributed to a patch. */
    Iron_ObjectDecl *last_patch = NULL;
    const char      *last_patch_target = NULL;

    for (int i = 0; i < program->decl_count; i++) {
        if (*out_stop) break;
        Iron_Node *d = program->decls[i];
        if (!d) continue;

        if (d->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
            if (od->is_patch) {
                last_patch = od;
                last_patch_target =
                    od->target_type_name ? od->target_type_name : od->name;
            } else {
                last_patch = NULL;
                last_patch_target = NULL;
            }
            continue;
        }

        if (d->kind != IRON_NODE_METHOD_DECL) {
            /* Classic func decls do not break the patch run (the parser
             * may interleave a synthesized self-param IRON_NODE_FUNC_DECL
             * but the resolver tolerates it). Other decl kinds clear. */
            if (d->kind != IRON_NODE_FUNC_DECL) {
                last_patch = NULL;
                last_patch_target = NULL;
            }
            continue;
        }

        if (!last_patch || !last_patch_target) continue;
        Iron_MethodDecl *m = (Iron_MethodDecl *)d;
        if (!m->type_name) continue;
        if (strcmp(m->type_name, last_patch_target) != 0) continue;
        if (strcmp(last_patch_target, target_type_name) != 0) continue;

        const char *od_path =
            last_patch->span.filename ? last_patch->span.filename : "";
        if (!ilsp_vis_can_see(od_path,
                               requester_canonical_path,
                               (const Iron_Node *)last_patch)) {
            continue;
        }
        if (!visit(m, last_patch, userdata)) { *out_stop = true; break; }
        (*out_visited)++;
    }
}

size_t ilsp_patch_for_each_method(
    Iron_Program             *program,
    IronLsp_WorkspaceIndex   *wi,
    const char               *target_type_name,
    const char               *requester_canonical_path,
    bool (*visit)(Iron_MethodDecl *md, Iron_ObjectDecl *patch_od, void *ud),
    void                     *userdata,
    _Atomic bool             *cancel)
{
    if (!target_type_name || !visit) return 0;

    size_t visited = 0;
    bool   stop    = false;

    /* Per-request transient arena. Used by registry build for E0254
     * message strdup (we pass diags=NULL so that branch is dead, but
     * the build signature still requires an arena). */
    Iron_Arena req_arena = iron_arena_create(8 * 1024);

    /* Same-program walk first. */
    if (program) {
        for_each_method_in_program(
            program, &req_arena, target_type_name,
            requester_canonical_path, visit, userdata,
            &visited, &stop);
    }

    /* Cross-program walk via workspace_index (RESEARCH Conflict 1). */
    if (!stop && wi) {
        size_t n = 0;
        char **paths = ilsp_workspace_index_snapshot_paths(wi, &n);
        if (paths) {
            for (size_t i = 0; i < n && !stop; i++) {
                if (cancel && atomic_load(cancel)) break;  /* Pitfall 7 */
                if (!paths[i]) continue;
                IronLsp_IndexEntry *e =
                    ilsp_workspace_index_lookup(wi, paths[i]);
                if (!e || !e->program) continue;
                if (!e->analyzed) {
                    (void)ilsp_workspace_index_analyze_lazy(wi, e, cancel);
                    if (cancel && atomic_load(cancel)) break;
                }
                if (e->program == program) continue;  /* already visited */

                for_each_method_in_program(
                    e->program, &req_arena, target_type_name,
                    requester_canonical_path, visit, userdata,
                    &visited, &stop);
            }
            for (size_t i = 0; i < n; i++) free(paths[i]);
            free(paths);
        }
    }

    iron_arena_free(&req_arena);
    return visited;
}
