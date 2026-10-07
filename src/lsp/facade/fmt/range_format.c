/* Phase 5 Plan 05-03 (FMT-03, D-04, D-06, D-10, D-12, D-17) -- range formatting.
 *
 * Walks Iron_Program.decls[] once, intersects each top-level decl's
 * 1-based inclusive line-span with the 0-based LSP Range, merges the
 * overlapping spans, and emits one TextEdit per merged span replacing
 * its lines with the whitespace layout of a whole-document format
 * (iron_fmt_layout). Emitted edits are sorted descending by offset
 * (D-06) so the client applies later edits first and avoids offset
 * invalidation.
 *
 * Single-call-site preservation (D-10): this TU does NOT call
 * iron_format_source. It consumes the lex+parse-only helper
 * ilsp_facade_fmt_lex_parse from format_internal.h (implementation in
 * format.c). The only compile-format-primitive site under src/lsp/
 * remains in format.c's facade_format static helper.
 *
 * Cancel polling (D-12 / D-17): 4 stage boundaries (pre-lex, pre-parse,
 * pre-walk, pre-emit) plus one poll per-decl during the intersection
 * walk (D-17 = D-16 iteration-boundary rule).
 *
 * Intersection semantics (D-04 "any overlap"): decl N's inclusive line
 * span and the LSP Range are normalised to 1-based inclusive before
 * comparison. An LSP Range ending at line N character 0 does NOT
 * include line N; ending at line N character >0 DOES include line N.
 * RESEARCH Pitfall 4. */

#include "lsp/facade/fmt/format.h"
#include "lsp/facade/fmt/format_internal.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "diagnostics/diagnostics.h"
#include "fmt/options.h"
#include "fmt/layout.h"
#include "lsp/facade/types.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "parser/ast.h"
#include "parser/printer.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

/* D-04 valid top-level decl kinds. Nested blocks roll up to the
 * enclosing top-level construct per D-04; method decls inside object
 * bodies are considered nested and do NOT appear in
 * Iron_Program.decls[], so the kind filter here is exactly the set of
 * kinds the parser emits at program scope. */
static bool is_top_level_decl(Iron_NodeKind k) {
    switch ((int)k) {
        case IRON_NODE_FUNC_DECL:
        case IRON_NODE_METHOD_DECL:
        case IRON_NODE_OBJECT_DECL:
        case IRON_NODE_INTERFACE_DECL:
        case IRON_NODE_ENUM_DECL:
        case IRON_NODE_VAL_DECL:
        case IRON_NODE_VAR_DECL:
        case IRON_NODE_IMPORT_DECL:
            return true;
        default:
            return false;
    }
}

/* D-04 "any overlap" intersection.
 *
 * Iron_Span uses 1-based inclusive line numbers (span.line ..
 * span.end_line). LSP Range uses 0-based half-open (start inclusive,
 * end exclusive). Normalise both sides to 1-based inclusive before
 * comparison.
 *
 * RESEARCH Pitfall 4: a range ending at line N character 0 does NOT
 * include line N (end is exclusive at column 0 = start of that line).
 * A range ending at line N character > 0 DOES include line N. */
static bool decl_intersects_range(Iron_Span span, IronLsp_Range r) {
    uint32_t decl_start = span.line;        /* 1-based, inclusive */
    uint32_t decl_end   = span.end_line;    /* 1-based, inclusive */

    uint32_t req_start  = r.start.line + 1; /* 0-based -> 1-based */

    /* Compute the inclusive last line of the LSP range. */
    uint32_t req_end_excl = r.end.line + 1;  /* 1-based exclusive end */
    uint32_t req_end_incl;
    if (req_end_excl == 0) {
        /* Defensive: 0 would underflow; treat as empty range. */
        return false;
    }
    if (r.end.character == 0 && req_end_excl > req_start) {
        /* End is at column 0 of some line past start -> exclusive. */
        req_end_incl = req_end_excl - 1;
    } else {
        /* End is past column 0 OR collapses to the start line. */
        req_end_incl = req_end_excl;
    }

    if (decl_end   < req_start)    return false;   /* decl ends before range */
    if (decl_start > req_end_incl) return false;   /* decl starts after range */
    return true;
}

/* D-06 descending offset sort comparator.
 *
 * Later edits (larger start offset) sort FIRST so the client applies
 * them before earlier edits. Applying later first prevents offset
 * invalidation in earlier edits. Mirrors Phase 4 D-11 WorkspaceEdit
 * ordering. */
static int cmp_edit_offset_desc(const void *a, const void *b) {
    const IronLsp_TextEdit *ea = (const IronLsp_TextEdit *)a;
    const IronLsp_TextEdit *eb = (const IronLsp_TextEdit *)b;
    if (eb->range.start.line != ea->range.start.line) {
        /* Descending on line. */
        return (eb->range.start.line > ea->range.start.line) ? 1 : -1;
    }
    if (eb->range.start.character != ea->range.start.character) {
        return (eb->range.start.character > ea->range.start.character) ? 1 : -1;
    }
    return 0;
}

IronLsp_TextEditList ilsp_facade_format_range(
    const struct IronLsp_Document       *doc,
    const struct IronLsp_WorkspaceIndex *ws,
    IronLsp_Range                        range,
    const IronFmtOptions                *opts_in,
    Iron_Arena                          *arena,
    const _Atomic bool                  *cancel)
{
    IronLsp_TextEditList empty = { NULL, 0 };

    /* Cancel poll 1: pre-lex (entry). */
    if (cancel && atomic_load(cancel)) return empty;
    if (!doc || !doc->text || !arena) return empty;

    /* Defensive: reject inverted ranges. Handlers also guard this, but
     * repeat here so direct-Unity calls stay honest. */
    if (range.end.line < range.start.line ||
        (range.end.line == range.start.line &&
         range.end.character < range.start.character)) {
        return empty;
    }

    /* Resolve options: explicit > workspace cache > defaults. */
    IronFmtOptions opts = iron_fmt_options_default();
    if (opts_in)      opts = *opts_in;
    else if (ws)      opts = ws->fmt_opts;

    /* Per-request diaglist (HARD-06). */
    Iron_DiagList diags = iron_diaglist_create();

    /* Cancel poll 2: pre-parse. */
    if (cancel && atomic_load(cancel)) {
        iron_diaglist_free(&diags);
        return empty;
    }

    IronFmtParseResult parse = ilsp_facade_fmt_lex_parse(doc, arena, &diags,
                                                          cancel);
    if (!parse.ok || !parse.program) {
        iron_diaglist_free(&diags);
        return empty;
    }

    /* Cancel poll 3: pre-walk. */
    if (cancel && atomic_load(cancel)) {
        iron_diaglist_free(&diags);
        return empty;
    }

    if (parse.program->kind != IRON_NODE_PROGRAM) {
        iron_diaglist_free(&diags);
        return empty;
    }
    Iron_Program *program = (Iron_Program *)parse.program;

    /* Collect the line spans of the intersecting decls, then merge the
     * overlapping ones: a method's span lies inside its object's. */
    typedef struct { uint32_t first, last; } LineSpan;
    LineSpan *spans = NULL;
    for (int i = 0; i < program->decl_count; i++) {
        /* Per-decl cancel poll (D-17 = D-16 iteration-boundary). */
        if (cancel && atomic_load(cancel)) {
            arrfree(spans);
            iron_diaglist_free(&diags);
            return empty;
        }

        Iron_Node *decl = program->decls[i];
        if (!decl) continue;
        if (decl->kind == IRON_NODE_ERROR) continue;
        if (!is_top_level_decl(decl->kind)) continue;
        if (!decl_intersects_range(decl->span, range)) continue;
        if (decl->span.line == 0) continue;
        LineSpan ls = { decl->span.line,
                        decl->span.end_line >= decl->span.line ? decl->span.end_line
                                                               : decl->span.line };
        arrput(spans, ls);
    }
    for (int a = 1; a < (int)arrlen(spans); a++)
        for (int b = a; b > 0 && spans[b - 1].first > spans[b].first; b--) {
            LineSpan t = spans[b]; spans[b] = spans[b - 1]; spans[b - 1] = t;
        }

    IronLsp_TextEdit *edits = NULL;
    for (int a = 0; a < (int)arrlen(spans); a++) {
        LineSpan m = spans[a];
        while (a + 1 < (int)arrlen(spans) && spans[a + 1].first <= m.last) {
            if (spans[a + 1].last > m.last) m.last = spans[a + 1].last;
            a++;
        }
        /* The same whitespace layout as a whole-document format, cut to
         * these lines: indentation comes from the whole file. */
        char *rendered = iron_fmt_layout(doc->text, &opts, arena, m.first, m.last);
        if (!rendered) continue;

        /* Replace whole lines: col 0 of the first line to col 0 of the
         * line after the last (LSP exclusive end, 0-based). */
        IronLsp_Range er;
        er.start.line      = m.first - 1;
        er.start.character = 0;
        er.end.line        = m.last;
        er.end.character   = 0;

        IronLsp_TextEdit edit;
        edit.range    = er;
        edit.new_text = rendered;
        arrpush(edits, edit);
    }
    arrfree(spans);

    /* Cancel poll 4: pre-emit. */
    if (cancel && atomic_load(cancel)) {
        arrfree(edits);
        iron_diaglist_free(&diags);
        return empty;
    }

    size_t count = (size_t)arrlen(edits);
    if (count == 0) {
        arrfree(edits);
        iron_diaglist_free(&diags);
        return empty;
    }

    /* D-06 descending sort by (line, character). */
    qsort(edits, count, sizeof(IronLsp_TextEdit), cmp_edit_offset_desc);

    /* Copy the stb_ds heap array body into an arena-owned contiguous
     * array so lifetime is pinned to the caller's arena. */
    IronLsp_TextEdit *out_edits = (IronLsp_TextEdit *)iron_arena_alloc(
        arena, count * sizeof(IronLsp_TextEdit),
        _Alignof(IronLsp_TextEdit));
    if (!out_edits) {
        arrfree(edits);
        iron_diaglist_free(&diags);
        return empty;
    }
    memcpy(out_edits, edits, count * sizeof(IronLsp_TextEdit));
    arrfree(edits);

    IronLsp_TextEditList list;
    list.edits = out_edits;
    list.count = count;

    iron_diaglist_free(&diags);
    return list;
}
