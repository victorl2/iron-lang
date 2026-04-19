/* Phase 5 Plan 05-04 (FMT-04, D-05, D-06, D-10, D-12, D-14, D-17) --
 * on-type formatting on `}` trigger.
 *
 * Walks Iron_Program.decls[] to find the deepest enclosing block whose
 * 1-based inclusive Iron_Span covers the 0-based LSP Position provided
 * by the client. For every non-blank line in that block whose current
 * leading whitespace differs from the canonical indent we emit one
 * TextEdit replacing columns [0, ws_end) with the canonical indent.
 * Emitted edits are sorted descending by line (D-06) so the client
 * applies later edits first and avoids offset invalidation.
 *
 * Single-call-site preservation (D-10): this TU does NOT reach the
 * compiler-side format primitive -- that lives in format.c's
 * facade_format static helper behind the sole compile-format-primitive
 * site under src/lsp/. On-type consumes the lex+parse-only helper
 * ilsp_facade_fmt_lex_parse from format_internal.h and computes
 * indentation itself.
 *
 * Cancel polling (D-12 / D-17): 4 stage boundaries (pre-lex, pre-parse,
 * pre-walk, pre-emit) plus one poll per line during the indent-edit
 * emission loop (D-17 = D-16 iteration-boundary rule).
 *
 * Outer-depth heuristic (KNOWN v1 LIMITATION per PLAN.md
 * <known_limitations>): depth of the enclosing block is computed by
 * counting '{' and '}' byte characters in the document buffer from
 * offset 0 up to the block start. String literals and comments that
 * contain balanced braces (the common case) cancel out; unbalanced
 * embedded braces (format-string templates etc.) may skew depth. The
 * failure mode is cosmetic (wrong indent column count), not corrupting.
 * v1.x upgrade path: thread ancestor count through find_enclosing_block
 * and count enclosing-block ancestors instead. */

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
#include "lsp/facade/types.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/workspace_index.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

/* 1-based (line, col) test: does the span cover the position?
 * Mirrors src/lsp/facade/edit/selection_range.c:47-54. */
static bool span_covers_1b(const Iron_Span *sp, uint32_t line, uint32_t col) {
    if (!sp) return false;
    if (sp->line == 0 && sp->end_line == 0) return false;
    if (line < sp->line || line > sp->end_line) return false;
    if (line == sp->line     && col < sp->col)     return false;
    if (line == sp->end_line && col > sp->end_col) return false;
    return true;
}

/* Descend into `n` while its span covers (line, col); if it is an
 * Iron_Block, try each stmt in order and return the deepest matching
 * block (or this block if no child block covers). For top-level decls
 * that carry a body, recurse into the body. Returns NULL when no block
 * at this subtree covers. */
static Iron_Node *descend_block(Iron_Node *n, uint32_t line, uint32_t col) {
    if (!n) return NULL;
    if (!span_covers_1b(&n->span, line, col)) return NULL;

    if (n->kind == IRON_NODE_BLOCK) {
        Iron_Block *blk = (Iron_Block *)n;
        for (int i = 0; i < blk->stmt_count; i++) {
            Iron_Node *deeper = descend_block(blk->stmts[i], line, col);
            if (deeper) return deeper;
        }
        return n;   /* this is the deepest block at (line, col) */
    }

    /* Non-block nodes: only descend into fields/bodies that may hold a
     * block. Iron_FuncDecl.body and Iron_MethodDecl.body are the v1
     * carriers. Object / interface / enum bodies nest methods that are
     * themselves decl nodes in the parent scope, not block nodes here,
     * so the on-type walker does not descend into them for v1 (matches
     * PLAN.md §known_limitations 4th bullet). */
    switch ((int)n->kind) {
        case IRON_NODE_FUNC_DECL: {
            Iron_FuncDecl *f = (Iron_FuncDecl *)n;
            if (f->body) return descend_block(f->body, line, col);
            return NULL;
        }
        case IRON_NODE_METHOD_DECL: {
            Iron_MethodDecl *m = (Iron_MethodDecl *)n;
            if (m->body) return descend_block(m->body, line, col);
            return NULL;
        }
        default:
            return NULL;
    }
}

/* Walk Iron_Program.decls[] and return the deepest enclosing block
 * whose span covers (line, col). Returns NULL if no block matches
 * (typed in a string literal, between decls, malformed source, etc.). */
static Iron_Node *find_enclosing_block(Iron_Node *root,
                                         uint32_t   line,
                                         uint32_t   col) {
    if (!root || root->kind != IRON_NODE_PROGRAM) return NULL;
    Iron_Program *prog = (Iron_Program *)root;
    for (int i = 0; i < prog->decl_count; i++) {
        Iron_Node *m = descend_block(prog->decls[i], line, col);
        if (m) return m;
    }
    return NULL;
}

/* Canonical indent width in bytes for a given depth. */
static size_t expected_indent_bytes(int depth, const IronFmtOptions *opts) {
    if (depth <= 0) return 0;
    if (opts && opts->use_tabs) return (size_t)depth;    /* 1 tab per level */
    int width = (opts && opts->indent_width > 0) ? opts->indent_width : 2;
    return (size_t)(depth * width);
}

/* Compute the leading-whitespace window for a 0-based line. On return
 * *line_start is the byte offset of the first byte on the line and
 * *ws_end is the byte offset of the first non-whitespace byte (or
 * line_start if the line begins with a non-whitespace byte, or
 * doc->text_len for a line at EOF). */
static void measure_line_indent(const struct IronLsp_Document *doc,
                                 uint32_t line_0,
                                 size_t *out_first_byte,
                                 size_t *out_last_ws_byte) {
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, line_0);
    size_t i = line_start;
    while (i < doc->text_len && (doc->text[i] == ' ' || doc->text[i] == '\t')) {
        i++;
    }
    *out_first_byte   = line_start;
    *out_last_ws_byte = i;
}

/* Descending-line sort so the client applies later edits first. */
static int cmp_edit_line_desc(const void *a, const void *b) {
    const IronLsp_TextEdit *ea = (const IronLsp_TextEdit *)a;
    const IronLsp_TextEdit *eb = (const IronLsp_TextEdit *)b;
    if (eb->range.start.line != ea->range.start.line) {
        return (eb->range.start.line > ea->range.start.line) ? 1 : -1;
    }
    if (eb->range.start.character != ea->range.start.character) {
        return (eb->range.start.character > ea->range.start.character) ? 1 : -1;
    }
    return 0;
}

IronLsp_TextEditList ilsp_facade_format_on_type(
    const struct IronLsp_Document       *doc,
    const struct IronLsp_WorkspaceIndex *ws,
    IronLsp_Position                     pos,
    char                                 trigger_char,
    const IronFmtOptions                *opts_in,
    Iron_Arena                          *arena,
    const _Atomic bool                  *cancel)
{
    IronLsp_TextEditList empty = { NULL, 0 };

    /* Cancel poll 1: pre-lex (entry). */
    if (cancel && atomic_load(cancel)) return empty;
    if (!doc || !doc->text || !arena) return empty;

    /* D-05: only `}` triggers in v1. Other triggers advertised to the
     * client would put us in a position to receive them (we do not
     * advertise them), but defend against stale client state. */
    if (trigger_char != '}') return empty;

    /* Resolve options: explicit > workspace cache > defaults. */
    IronFmtOptions opts = iron_fmt_options_default();
    if (opts_in)      opts = *opts_in;
    else if (ws)      opts = ws->fmt_opts;

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

    /* LSP 0-based -> 1-based for span_covers_1b. */
    uint32_t line_1b = pos.line + 1;
    uint32_t col_1b  = pos.character + 1;

    Iron_Node *block = find_enclosing_block(parse.program, line_1b, col_1b);
    if (!block) {
        /* `}` outside any block -- string literal, malformed, typed
         * before an existing `}`, etc. D-05 step 4 + RESEARCH Pitfall
         * 5 + Claude's Discretion §381: empty edits. */
        iron_diaglist_free(&diags);
        return empty;
    }

    /* Compute outer-block depth via brace counting up to block start
     * (v1 heuristic -- see file-level comment). block->span.line is
     * 1-based inclusive; convert to 0-based for byte_of_line. */
    uint32_t block_start_line0 =
        block->span.line > 0 ? block->span.line - 1 : 0;
    size_t block_start_offset =
        ilsp_byte_of_line(&doc->line_idx, block_start_line0);
    size_t open_braces  = 0;
    size_t close_braces = 0;
    for (size_t i = 0; i < block_start_offset && i < doc->text_len; i++) {
        if (doc->text[i] == '{')      open_braces++;
        else if (doc->text[i] == '}') close_braces++;
    }
    int outer_depth = 0;
    if (open_braces > close_braces) {
        outer_depth = (int)(open_braces - close_braces);
    }

    IronLsp_TextEdit *edits = NULL;

    for (uint32_t l_1b = block->span.line;
         l_1b <= block->span.end_line;
         l_1b++) {
        /* Per-line cancel poll (D-17 iteration-boundary). */
        if (cancel && atomic_load(cancel)) {
            arrfree(edits);
            iron_diaglist_free(&diags);
            return empty;
        }

        if (l_1b == 0) continue;    /* defensive */
        uint32_t l_0b = l_1b - 1;

        size_t line_start = 0;
        size_t ws_end     = 0;
        measure_line_indent(doc, l_0b, &line_start, &ws_end);

        /* Skip blank lines (empty or whitespace-only). */
        if (ws_end >= doc->text_len) continue;
        if (doc->text[ws_end] == '\n') continue;

        /* Brace lines (the `{`-opening line and the `}`-closing line)
         * take the outer depth; interior lines take outer+1. */
        bool is_brace_line =
            (l_1b == block->span.line) || (l_1b == block->span.end_line);
        int depth = is_brace_line ? outer_depth : outer_depth + 1;

        size_t expected = expected_indent_bytes(depth, &opts);
        size_t current  = ws_end - line_start;

        if (expected == current) continue;   /* minimal: no edit */

        /* Build the replacement indent string (arena-owned). */
        char *new_text = (char *)iron_arena_alloc(arena, expected + 1, 1);
        if (!new_text) continue;
        char fill = opts.use_tabs ? '\t' : ' ';
        for (size_t i = 0; i < expected; i++) new_text[i] = fill;
        new_text[expected] = '\0';

        IronLsp_TextEdit edit;
        edit.range.start.line      = l_0b;
        edit.range.start.character = 0;
        edit.range.end.line        = l_0b;
        edit.range.end.character   = (uint32_t)current;
        edit.new_text              = new_text;
        arrpush(edits, edit);
    }

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

    qsort(edits, count, sizeof(IronLsp_TextEdit), cmp_edit_line_desc);

    /* Copy to arena-owned contiguous array; arrfree the stb_ds heap
     * header (matches Plan 05-03 range_format.c pattern). */
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
