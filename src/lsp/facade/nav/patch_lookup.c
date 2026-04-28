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
 * Builds + frees an Iron_TypePatchRegistry inside this function scope
 * per Pitfall 1. Returns false via *out_stop when the visitor requests
 * short-circuit; *out_visited is incremented for each method that
 * passes the visibility filter. */
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
    if (!program || !visit) return;

    Iron_TypePatchRegistry *reg = iron_type_patch_registry_build(
        program, /*global_scope=*/NULL, req_arena, /*diags=*/NULL);
    if (!reg) return;

    Iron_MethodDecl **methods =
        iron_type_patch_lookup(reg, target_type_name);
    if (methods) {
        ptrdiff_t mlen = arrlen(methods);
        for (ptrdiff_t i = 0; i < mlen; i++) {
            if (*out_stop) break;
            Iron_MethodDecl *m = methods[i];
            if (!m) continue;
            Iron_ObjectDecl *od = patch_enclosing_in_program(program, m);
            if (!od) continue;
            const char *od_path =
                od->span.filename ? od->span.filename : "";
            if (!ilsp_vis_can_see(od_path,
                                   requester_canonical_path,
                                   (const Iron_Node *)od)) {
                continue;
            }
            if (!visit(m, od, userdata)) { *out_stop = true; break; }
            (*out_visited)++;
        }
    }

    iron_type_patch_registry_free(reg);
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
