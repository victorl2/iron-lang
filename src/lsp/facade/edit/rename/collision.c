/* Phase 4 Plan 04-06 Task 01 (EDIT-12, D-10) -- rename collision check.
 *
 * Keyword guard reuses the configure_file-generated keyword mirror
 * (ILSP_COMPLETION_KEYWORDS from Plan 04-02 Task 01). The mirror is
 * kept byte-identical with the lexer's kw_table by the CMake
 * configure_file pipeline (+ the test_completion_keyword_mirror drift
 * guard); using it here avoids a second place to update when the
 * lexer gains a new keyword.
 *
 * Scope conflict detection uses the already-exposed iron_scope_lookup
 * directly (walks the parent chain). The distinguishing condition is
 * "lookup resolves to a DIFFERENT Iron_Symbol than the target" --
 * renaming is fine when the only match is the target's own decl.
 */

#include "lsp/facade/edit/rename/collision.h"

#include "analyzer/scope.h"
#include "diagnostics/diagnostics.h"  /* Iron_Span */
#include "util/arena.h"

/* Plan 04-02 configure_file-generated keyword mirror. Same list as
 * src/lexer/lexer.c:kw_table, asserted by
 * test_completion_keyword_mirror. */
#include "keyword_mirror.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

bool ilsp_rename_is_iron_keyword(const char *name) {
    if (!name || !*name) return false;
    for (size_t i = 0; i < ILSP_COMPLETION_KEYWORD_COUNT; i++) {
        if (strcmp(name, ILSP_COMPLETION_KEYWORDS[i]) == 0) return true;
    }
    return false;
}

void ilsp_rename_collision_check(struct IronLsp_WorkspaceIndex *wi,
                                    const Iron_Symbol            *target,
                                    const char                   *old_name,
                                    const char                   *new_name,
                                    Iron_Scope * const           *ref_site_scopes,
                                    size_t                         n_scopes,
                                    Iron_Arena                    *arena,
                                    IronLsp_CollisionResult       *out) {
    (void)wi;  /* reserved for future cross-entry conflict surfaces */
    if (!out) return;
    out->kind          = ILSP_COLLISION_NONE;
    out->conflict_file = NULL;
    out->conflict_line = 0;
    out->conflict_col  = 0;
    out->conflict_name = NULL;

    /* Step 1: empty-name guard. */
    if (!new_name || new_name[0] == '\0') {
        out->kind = ILSP_COLLISION_EMPTY_NAME;
        return;
    }

    /* Step 2: same-name short-circuit. Caller emits empty WorkspaceEdit
     * per D-10 — this is not an error, just a no-op. */
    if (old_name && strcmp(new_name, old_name) == 0) {
        out->kind = ILSP_COLLISION_SAME_NAME;
        return;
    }

    /* Step 3: keyword guard (regardless of scope). */
    if (ilsp_rename_is_iron_keyword(new_name)) {
        out->kind = ILSP_COLLISION_KEYWORD;
        return;
    }

    /* Step 4: per-scope lookup. First hit wins. */
    if (!ref_site_scopes || n_scopes == 0) {
        out->kind = ILSP_COLLISION_NONE;
        return;
    }
    for (size_t i = 0; i < n_scopes; i++) {
        Iron_Scope *s = ref_site_scopes[i];
        if (!s) continue;
        Iron_Symbol *found = iron_scope_lookup(s, new_name);
        if (!found) continue;
        if (target && found == target) continue;  /* self-match -- OK */
        /* Collision. Populate details for the handler -> -32803 message. */
        out->kind = ILSP_COLLISION_SCOPE_CONFLICT;
        out->conflict_line = found->span.line;
        out->conflict_col  = found->span.col;
        const char *fn = found->span.filename ? found->span.filename : "";
        const char *nm = found->name ? found->name : "";
        if (arena) {
            out->conflict_file = iron_arena_strdup(arena, fn, strlen(fn));
            out->conflict_name = iron_arena_strdup(arena, nm, strlen(nm));
        } else {
            out->conflict_file = fn;
            out->conflict_name = nm;
        }
        if (!out->conflict_file) out->conflict_file = "";
        if (!out->conflict_name) out->conflict_name = "";
        return;
    }

    /* Step 5: no conflict. */
    out->kind = ILSP_COLLISION_NONE;
}
