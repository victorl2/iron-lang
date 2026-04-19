/* Phase 4 Plan 04-03 Task 01 (EDIT-05, D-02) -- additionalTextEdits
 * builder for auto-import on bucket 4 (stdlib) and bucket 5 (deps)
 * completion candidates.
 *
 * The walker visits the consecutive top-of-file run of
 * IRON_NODE_IMPORT_DECL. It stops on the first non-import decl, and
 * inside the run it skips any IRON_NODE_ERROR node (parser error
 * recovery sentinel) and any import whose span is mid-edit (PITFALL
 * C: span.end_line >= the next top-level decl's start line, which
 * means the parser consumed the next decl's first token as part of
 * the broken import).
 *
 * The insertion line we emit is LSP-compliant 0-indexed (Iron_Span
 * fields are 1-indexed). newText always ends with a single '\n' so
 * the client can normalize to its file EOL. When we emit at the top
 * of a file that has no other imports, we append a trailing blank
 * line (`"import <m>\n\n"`) to separate the fresh import block from
 * the code beneath it, per D-02 ("start of file with a trailing
 * blank line"). */

#include "lsp/facade/edit/complete/auto_import.h"

#include "parser/ast.h"
#include "util/arena.h"

#include <stdint.h>
#include <string.h>

/* Does the stem contain a '.' character? Nested module paths are
 * deferred per CONTEXT.md D-02 -- we return no edit and leave the
 * caller to insert the candidate label as-is. */
static bool has_dot(const char *s) {
    for (const char *p = s; p && *p; p++) {
        if (*p == '.') return true;
    }
    return false;
}

/* PITFALL C: the parser's mid-edit recovery may leave an import with
 * a corrupted or placeholder path. We detect two well-formed shapes:
 *   - imp->path is non-NULL, non-empty, and NOT the parser's HARD-09
 *     placeholder literal `"?"`.
 *   - imp->span.line > 0 (a real source location was set).
 * We do NOT use end_line alone as the witness because the parser
 * merges the import's span with the *next* token's span, so a healthy
 * `import math\n\nfunc ...` has end_line pointing at `func`. That
 * span breadth is structural, not a symptom of breakage. */
static bool import_anchor_well_formed(const Iron_ImportDecl *imp) {
    if (!imp) return false;
    if (imp->span.line == 0) return false;
    if (!imp->path || !*imp->path) return false;
    if (strcmp(imp->path, "?") == 0) return false;
    return true;
}

/* Build the newText for an insertion. If `with_blank_line` is true the
 * result is `"import <stem>\n\n"`; otherwise `"import <stem>\n"`. */
static const char *build_new_text(const char *stem, bool with_blank_line,
                                   Iron_Arena *arena) {
    size_t stem_len = strlen(stem);
    /* "import " (7) + stem + "\n" (1) + optional "\n" (1) + NUL */
    size_t cap = 7 + stem_len + 1 + (with_blank_line ? 1 : 0) + 1;
    char *buf = (char *)iron_arena_alloc(arena, cap, 1);
    if (!buf) return NULL;
    memcpy(buf, "import ", 7);
    memcpy(buf + 7, stem, stem_len);
    size_t pos = 7 + stem_len;
    buf[pos++] = '\n';
    if (with_blank_line) buf[pos++] = '\n';
    buf[pos] = '\0';
    return buf;
}

void ilsp_auto_import_edit(const Iron_Program           *program,
                            const struct IronLsp_Document *doc,
                            const char                   *module_stem,
                            Iron_Arena                   *arena,
                            IronLsp_AutoImportEdit       *out_edit,
                            const char                  **out_alias) {
    (void)doc;  /* not needed at this layer -- span already carries line info */
    if (out_edit) {
        out_edit->line = 0;
        out_edit->character = 0;
        out_edit->new_text = NULL;
    }
    if (out_alias) *out_alias = NULL;
    if (!out_edit || !arena || !module_stem || !*module_stem) return;

    /* Nested module paths deferred per D-02. Caller inserts candidate
     * label verbatim. */
    if (has_dot(module_stem)) return;

    /* No program -> treat as empty file: insert at line 0 col 0 with
     * trailing blank line. Callers may pass NULL during cold-start. */
    if (!program || program->decl_count <= 0) {
        out_edit->line = 0;
        out_edit->character = 0;
        out_edit->new_text = build_new_text(module_stem,
                                             /*with_blank_line=*/true, arena);
        return;
    }

    /* Walk the consecutive top-of-file run of imports. */
    const Iron_ImportDecl *last_anchor = NULL;
    int i = 0;
    for (; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) break;
        if (d->kind == IRON_NODE_ERROR) {
            /* PITFALL C: skip error-recovery sentinels inside the
             * import prelude; keep scanning the run. */
            continue;
        }
        if (d->kind != IRON_NODE_IMPORT_DECL) {
            /* End of the consecutive import run. */
            break;
        }

        const Iron_ImportDecl *imp = (const Iron_ImportDecl *)d;

        /* PITFALL C: reject mid-edit imports whose span swallowed a
         * subsequent line. We still look for dedup/alias on them (the
         * .path may be correct) but we DO NOT treat them as the
         * anchor for a new insertion. */
        bool well_formed = import_anchor_well_formed(imp);

        /* Dedup / alias honor: compare this import's path to the
         * requested module_stem. Case-sensitive string match. */
        if (imp->path && strcmp(imp->path, module_stem) == 0) {
            if (imp->alias && *imp->alias) {
                /* Alias honour: no edit, return alias to caller. */
                if (out_alias) {
                    *out_alias = iron_arena_strdup(arena, imp->alias,
                                                     strlen(imp->alias));
                }
                out_edit->new_text = NULL;
                return;
            }
            /* Bare match -> dedup, no edit. */
            out_edit->new_text = NULL;
            return;
        }

        if (well_formed) last_anchor = imp;
    }

    /* No existing import for module_stem. Emit an insertion TextEdit. */
    if (last_anchor) {
        /* Insert on the line AFTER the last valid anchor. Iron_Span
         * uses 1-based line numbers; LSP Position.line is 0-based.
         * We deliberately use anchor.span.line (the import's START
         * line) rather than end_line, because the parser's span
         * merge extends end_line to the *next* token's end line --
         * for a well-formed `import math\n\nfunc main()` that is
         * the `func` line, not a line we want to insert on. Imports
         * are always single-line in Iron surface syntax, so the
         * start line is the canonical anchor line; the line below
         * it (1-indexed `line + 1`) is LSP 0-indexed `line`. */
        out_edit->line = last_anchor->span.line;
        out_edit->character = 0;
        out_edit->new_text = build_new_text(module_stem,
                                             /*with_blank_line=*/false, arena);
    } else {
        /* No valid anchor seen. Emit at top of file with trailing
         * blank line per D-02. */
        out_edit->line = 0;
        out_edit->character = 0;
        out_edit->new_text = build_new_text(module_stem,
                                             /*with_blank_line=*/true, arena);
    }
}
