/* Phase 4 Plan 04-05 Task 01 (EDIT-09, D-08) -- source.organizeImports
 * facade implementation.
 *
 * Algorithm (D-08 LOCKED):
 *   1. Walk program->decls[] starting at index 0 while decls[i]->kind ==
 *      IRON_NODE_IMPORT_DECL (skip IRON_NODE_ERROR). Stop at first non-
 *      import, non-error node.
 *   2. If collected run is empty: emit no edit.
 *   3. Classify each Iron_ImportDecl by path:
 *        Group A (stdlib): ilsp_stdlib_cache_get(wi->stdlib, path) != NULL
 *        Group B (dep)   : ilsp_dep_map_lookup(wi->deps, path)     != NULL
 *        Group C (local) : everything else
 *   4. Dedup within the kept set: a ({path,alias}) key is considered the
 *      same. First occurrence wins.
 *   5. Unused-removal (gated): when wi->bulk_analyze_done == true, drop
 *      every kept import whose span.line hosts an IRON_WARN_UNUSED_IMPORT
 *      diagnostic in the caller-provided Iron_DiagList. When the flag is
 *      false, set cold_workspace_warning and SKIP the removal pass.
 *   6. Sort the kept set by (group_rank asc, path asc, alias-presence asc
 *      [non-aliased first], alias asc).
 *   7. Emit TextEdit:
 *        - Range start = (span.line-1, 0) of the FIRST import's effective
 *          top (include its doc_comment lines).
 *        - Range end   = (span.end_line, 0) of the LAST import -- one full
 *          line past its trailing newline so the block cleanly abuts the
 *          next decl.
 *        - newText = reformatted block; groups separated by one blank
 *          line; empty groups omit the separator.
 *   8. Emit per doc_comment: each stored line re-rendered as `/// <line>\n`.
 *
 * Threat register:
 *   T-4-2: stop at first Iron_ErrorNode inside the run -> no edit emitted.
 *   T-4-3: `context.only` filter enforcement lives in the caller
 *          (codeaction.c); organizeImports only runs when only[] explicitly
 *          contains "source.organizeImports".
 */

#include "lsp/facade/edit/codeaction/organize_imports.h"

#include "lsp/store/document.h"
#include "lsp/store/dep_map.h"
#include "lsp/store/stdlib_cache.h"
#include "lsp/store/workspace_index.h"

#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Group rank: used as the primary sort key. */
#define GROUP_STDLIB 0
#define GROUP_DEP    1
#define GROUP_LOCAL  2

typedef struct {
    const Iron_ImportDecl *imp;     /* source AST node */
    int                    group;   /* GROUP_STDLIB / DEP / LOCAL */
    bool                   dropped; /* true after dedup/unused-remove */
} OrgEntry;

/* ── Small string-builder backed by an arena ─────────────────────── */

typedef struct {
    char        *buf;
    size_t       len;
    size_t       cap;
    Iron_Arena  *arena;
} SB;

static void sb_init(SB *sb, Iron_Arena *arena, size_t initial_cap) {
    sb->arena = arena;
    sb->cap   = initial_cap;
    sb->len   = 0;
    sb->buf   = (char *)iron_arena_alloc(arena, sb->cap, 1);
    if (sb->buf) sb->buf[0] = '\0';
}

static void sb_reserve(SB *sb, size_t need) {
    if (!sb->buf) return;
    if (sb->len + need + 1 <= sb->cap) return;
    size_t ncap = sb->cap;
    while (ncap < sb->len + need + 1) ncap *= 2;
    char *nb = (char *)iron_arena_alloc(sb->arena, ncap, 1);
    if (!nb) return;
    memcpy(nb, sb->buf, sb->len + 1);
    sb->buf = nb;
    sb->cap = ncap;
}

static void sb_append_n(SB *sb, const char *s, size_t n) {
    if (!sb->buf || !s || n == 0) return;
    sb_reserve(sb, n);
    if (!sb->buf) return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append(SB *sb, const char *s) {
    if (!s) return;
    sb_append_n(sb, s, strlen(s));
}

static void sb_append_char(SB *sb, char c) {
    sb_append_n(sb, &c, 1);
}

/* ── Classification ──────────────────────────────────────────────── */

static int classify_import(const Iron_ImportDecl        *imp,
                            struct IronLsp_WorkspaceIndex *wi) {
    if (!imp || !imp->path || !wi) return GROUP_LOCAL;
    /* Group A: stdlib lookup. */
    if (wi->stdlib && ilsp_stdlib_cache_get(wi->stdlib, imp->path)) {
        return GROUP_STDLIB;
    }
    /* Group B: dep lookup. */
    if (wi->deps && ilsp_dep_map_lookup(wi->deps, imp->path)) {
        return GROUP_DEP;
    }
    return GROUP_LOCAL;
}

/* ── Dedup ──────────────────────────────────────────────────────── */

/* Exact (path, alias) equivalence. NULL alias is its own bucket so that
 * `import x` and `import x as y` are NOT deduped. */
static bool same_binding(const Iron_ImportDecl *a, const Iron_ImportDecl *b) {
    if (!a || !b || !a->path || !b->path) return false;
    if (strcmp(a->path, b->path) != 0) return false;
    const char *aa = a->alias;
    const char *ba = b->alias;
    if (!aa && !ba) return true;
    if (!aa || !ba) return false;
    return strcmp(aa, ba) == 0;
}

static void dedup_entries(OrgEntry *entries, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (entries[i].dropped) continue;
        for (size_t j = i + 1; j < n; j++) {
            if (entries[j].dropped) continue;
            if (same_binding(entries[i].imp, entries[j].imp)) {
                entries[j].dropped = true;
            }
        }
    }
}

/* ── Unused detection via Iron_DiagList ─────────────────────────── */

/* Returns true when `diags` contains an IRON_WARN_UNUSED_IMPORT at the
 * same 1-indexed source line as `imp->span.line`. The compiler only
 * emits this warning for aliased imports today (see
 * src/analyzer/resolve.c:emit_unused_imports), so organizeImports'
 * unused-removal also effectively operates on aliased imports only --
 * a conservative + correct stance. */
static bool import_is_flagged_unused(const Iron_ImportDecl   *imp,
                                       const Iron_DiagList    *diags) {
    if (!imp || !diags || diags->count <= 0) return false;
    uint32_t line = imp->span.line;
    if (line == 0) return false;
    for (int i = 0; i < diags->count; i++) {
        const Iron_Diagnostic *d = &diags->items[i];
        if (d->code != IRON_WARN_UNUSED_IMPORT) continue;
        if (d->span.line == line) return true;
    }
    return false;
}

/* ── Comparator for qsort ─────────────────────────────────────── */

static int cmp_entry(const void *a, const void *b) {
    const OrgEntry *ea = (const OrgEntry *)a;
    const OrgEntry *eb = (const OrgEntry *)b;

    /* Keep dropped entries at the tail so we can ignore them in the
     * emit pass and still benefit from a stable qsort comparator. */
    if (ea->dropped != eb->dropped) return ea->dropped - eb->dropped;

    if (ea->group != eb->group) return ea->group - eb->group;

    const char *pa = ea->imp->path ? ea->imp->path : "";
    const char *pb = eb->imp->path ? eb->imp->path : "";
    int pc = strcmp(pa, pb);
    if (pc != 0) return pc;

    /* Same path: non-aliased first. */
    int ha = ea->imp->alias ? 1 : 0;
    int hb = eb->imp->alias ? 1 : 0;
    if (ha != hb) return ha - hb;

    /* Both aliased: order by alias. */
    if (ha && hb) {
        return strcmp(ea->imp->alias, eb->imp->alias);
    }
    return 0;
}

/* ── Emission ─────────────────────────────────────────────────── */

/* Emit a doc_comment run into sb, one `/// <line>` per joined `\n`. */
static void emit_doc_comment(SB *sb, const char *doc) {
    if (!doc || !*doc) return;
    const char *p = doc;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        size_t line_len = (size_t)(p - line_start);
        sb_append_n(sb, "/// ", 4);
        sb_append_n(sb, line_start, line_len);
        sb_append_char(sb, '\n');
        if (*p == '\n') p++;
    }
}

/* Emit `import <path>[ as <alias>]` followed by a newline. */
static void emit_import_line(SB *sb, const Iron_ImportDecl *imp) {
    if (!imp || !imp->path) return;
    sb_append_n(sb, "import ", 7);
    sb_append(sb, imp->path);
    if (imp->alias && *imp->alias) {
        sb_append_n(sb, " as ", 4);
        sb_append(sb, imp->alias);
    }
    sb_append_char(sb, '\n');
}

/* Emit one group of kept entries (in already-sorted order). Returns
 * true when the group produced any output (used to drive blank-line
 * separator between non-empty groups). */
static bool emit_group(SB *sb, const OrgEntry *entries, size_t n, int group) {
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (entries[i].dropped) continue;
        if (entries[i].group != group) continue;
        const Iron_ImportDecl *imp = entries[i].imp;
        emit_doc_comment(sb, imp->doc_comment);
        emit_import_line(sb, imp);
        any = true;
    }
    return any;
}

/* ── Top-level ────────────────────────────────────────────────── */

void ilsp_organize_imports(const Iron_Program              *program,
                            struct IronLsp_Document         *doc,
                            const Iron_DiagList             *diags,
                            struct IronLsp_WorkspaceIndex   *wi,
                            _Atomic bool                    *cancel,
                            Iron_Arena                      *arena,
                            IronLsp_OrganizeImportsResult   *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    (void)doc;  /* Reserved for future URI/line-count-dependent heuristics. */
    if (!arena || !program) return;
    if (cancel && atomic_load(cancel)) return;

    /* Step 1: collect the leading consecutive Iron_ImportDecl run. */
    int run_len = 0;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) break;
        if (d->kind == IRON_NODE_ERROR) return;   /* T-4-2: refuse cleanly */
        if (d->kind != IRON_NODE_IMPORT_DECL) break;
        run_len++;
    }
    if (run_len <= 0) return;       /* no imports -- no edit */

    if (cancel && atomic_load(cancel)) return;

    /* Step 2: allocate classified entry vector. */
    OrgEntry *entries = (OrgEntry *)iron_arena_alloc(
        arena, (size_t)run_len * sizeof(OrgEntry), _Alignof(OrgEntry));
    if (!entries) return;
    memset(entries, 0, (size_t)run_len * sizeof(OrgEntry));

    for (int i = 0; i < run_len; i++) {
        const Iron_ImportDecl *imp = (const Iron_ImportDecl *)program->decls[i];
        entries[i].imp     = imp;
        entries[i].group   = classify_import(imp, wi);
        entries[i].dropped = false;
    }

    if (cancel && atomic_load(cancel)) return;

    /* Step 3: dedup. */
    dedup_entries(entries, (size_t)run_len);

    /* Step 4: unused-removal (gated). */
    if (wi && wi->bulk_analyze_done) {
        if (diags) {
            for (int i = 0; i < run_len; i++) {
                if (entries[i].dropped) continue;
                if (import_is_flagged_unused(entries[i].imp, diags)) {
                    entries[i].dropped = true;
                }
            }
        }
    } else {
        /* Cold workspace: record the signal and SKIP unused removal. */
        out->cold_workspace_warning = true;
    }

    if (cancel && atomic_load(cancel)) return;

    /* Step 5: sort kept entries. */
    qsort(entries, (size_t)run_len, sizeof(OrgEntry), cmp_entry);

    /* Step 6: compute the replaced range.
     *   start = (first_import->span.line - 1, 0)
     *     -- if the first import has a doc_comment, we do NOT shift
     *     upward to cover it. The parser attaches doc_comment to the
     *     import but the import's own span starts at the `import` keyword
     *     line. For simplicity and round-trip correctness we rewrite
     *     starting at the import line and emit the doc_comment as part
     *     of the replacement. The file-header preservation rule only
     *     requires that blank lines BEFORE the first import stay
     *     untouched; since we replace from the first-import line down,
     *     anything above is preserved naturally. */
    /* Use the ORIGINAL first import -- entries[] is sorted now, so we
     * need to locate the smallest span.line across the ORIGINAL run. */
    uint32_t min_line = 0;
    uint32_t max_end_line = 0;
    for (int i = 0; i < run_len; i++) {
        const Iron_ImportDecl *imp =
            (const Iron_ImportDecl *)program->decls[i];
        if (!imp || imp->span.line == 0) continue;
        if (min_line == 0 || imp->span.line < min_line) {
            min_line = imp->span.line;
        }
        /* span.end_line may be 0 or already one past the import's last
         * line when the parser merged into the following decl's span --
         * defensively clamp to span.line when end_line == 0. */
        uint32_t eln = imp->span.end_line > 0 ? imp->span.end_line
                                               : imp->span.line;
        if (eln > max_end_line) max_end_line = eln;
    }
    if (min_line == 0 || max_end_line == 0) return;

    /* Step 7: also include doc_comment lines BEFORE the first import
     * that belong to that import (the parser stores them joined by \n
     * in doc_comment; we need to shift range_start up by the number of
     * doc_comment lines so the old `/// foo` prefix gets replaced
     * atomically). Count doc-lines on the ORIGINAL first-import. */
    int doc_line_shift = 0;
    for (int i = 0; i < run_len; i++) {
        const Iron_ImportDecl *imp =
            (const Iron_ImportDecl *)program->decls[i];
        if (!imp || imp->span.line != min_line) continue;
        if (!imp->doc_comment || !*imp->doc_comment) break;
        /* Count `\n` separators + 1 to get the stored doc_comment line
         * count. */
        int lines = 1;
        for (const char *p = imp->doc_comment; *p; p++) {
            if (*p == '\n') lines++;
        }
        doc_line_shift = lines;
        break;
    }
    uint32_t start_line0 = (min_line - 1);
    if ((int)start_line0 >= doc_line_shift) {
        start_line0 -= (uint32_t)doc_line_shift;
    } else {
        start_line0 = 0;
    }
    out->range_start_line = start_line0;
    out->range_start_char = 0;
    /* End = (max_end_line, 0) so the full last-import line is replaced.
     * NOTE: span.end_line is 1-indexed; LSP line is 0-indexed. For a
     * one-line import at line L the span is (L..L+1) in our parser's
     * merged-span convention OR (L..L) -- either way, taking max_end_line
     * as the LSP end line points at the first column of the line
     * immediately after the last import, which is exactly what we want
     * for a full-run delete. */
    /* If end_line == min_line we bump by 1 so the last import's newline
     * is consumed. This handles single-line single-import files. */
    uint32_t end_line0 = max_end_line;
    if (end_line0 <= start_line0) {
        end_line0 = start_line0 + 1;
    }
    out->range_end_line = end_line0;
    out->range_end_char = 0;

    if (cancel && atomic_load(cancel)) return;

    /* Step 8: emit the reformatted block into an arena-owned string. */
    SB sb; sb_init(&sb, arena, 512);

    bool emitted_stdlib = emit_group(&sb, entries, (size_t)run_len,
                                       GROUP_STDLIB);
    bool emitted_dep    = false;
    bool emitted_local  = false;

    if (emitted_stdlib) {
        /* Peek: any DEP or LOCAL kept? If yes, emit a blank line. */
        for (int i = 0; i < run_len; i++) {
            if (entries[i].dropped) continue;
            if (entries[i].group != GROUP_STDLIB) {
                sb_append_char(&sb, '\n');
                break;
            }
        }
    }

    emitted_dep = emit_group(&sb, entries, (size_t)run_len, GROUP_DEP);

    if (emitted_dep) {
        for (int i = 0; i < run_len; i++) {
            if (entries[i].dropped) continue;
            if (entries[i].group == GROUP_LOCAL) {
                sb_append_char(&sb, '\n');
                break;
            }
        }
    }

    emitted_local = emit_group(&sb, entries, (size_t)run_len, GROUP_LOCAL);
    (void)emitted_local;  /* track the last group for symmetry */

    if (!sb.buf || sb.len == 0) {
        /* Every import got dedup'd or dropped; emit empty new_text so
         * the full-line range is replaced with nothing (deletes the
         * import block). */
        out->new_text = iron_arena_strdup(arena, "", 0);
        if (!out->new_text) out->new_text = "";
        return;
    }

    out->new_text = sb.buf;
}
