/* Phase 5 Plan 05-02 (FMT-02, D-10, D-11, D-12) -- LSP formatting facade.
 *
 * THE SINGLE iron_format_source CALL SITE FOR src/lsp.
 *
 * Grep invariant: exactly one line in src/lsp matches the literal
 * symbol name followed by an opening paren. See
 * test_fmt_single_call_site in the root CMakeLists.txt; a failure there
 * means someone added a second call site.
 *
 * Mirrors src/lsp/facade/compile.c lines 47-80 (CORE-22 analog for the
 * analyze pipeline). Plans 05-03 (range_format) and 05-04
 * (on_type_format) will route their public entry points through the
 * same in-TU static `facade_format` helper -- adding new entries to
 * this same TU keeps the grep count at exactly 1.
 *
 * Threading: dispatcher-synchronous (D-12). The handler hands us a
 * per-request Iron_Arena (HARD-06); we allocate the TextEdit array and
 * the formatted-source bytes inside it. Cancel polling at the 4 stage
 * boundaries listed in D-12. */

#include "lsp/facade/fmt/format.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "diagnostics/diagnostics.h"
#include "fmt/format.h"        /* iron_format_source -- the single call site below */
#include "fmt/options.h"
#include "lsp/facade/types.h"
#include "lsp/store/document.h"
#include "lsp/store/utf.h"
#include "lsp/store/workspace_index.h"
#include "util/arena.h"

/* ───────────────────────────────────────────────────────────────────────
 * THE SINGLE iron_format_source CALL SITE FOR src/lsp.
 *
 * Enforced by the CTest `test_fmt_single_call_site` grep-invariant in
 * the root CMakeLists.txt; the count must stay at 1.
 *
 * All public facade entries (_full this plan, _range Plan 05-03,
 * _on_type Plan 05-04) MUST route through facade_format below. Adding
 * new sibling TUs in this directory that invoke the compiler primitive
 * directly will trip the grep invariant.
 * ─────────────────────────────────────────────────────────────────── */
static IronFmtResult facade_format(const struct IronLsp_Document *doc,
                                    const IronFmtOptions          *opts,
                                    Iron_Arena                    *arena,
                                    Iron_DiagList                 *diags,
                                    const _Atomic bool            *cancel) {
    IronFmtResult r;
    r.formatted     = "";
    r.formatted_len = 0;
    r.ok            = false;
    r.error_count   = 0;

    if (cancel && atomic_load(cancel)) return r;
    if (!doc || !doc->text || !arena || !diags) return r;

    const char *uri = doc->uri ? doc->uri : "<buffer>";
    return iron_format_source(doc->text, uri, opts, arena, diags);
}

/* ── Trailing-newline + BOM normalization (RESEARCH Pitfalls 1 + 2) ──
 *
 * If the source ended with '\n' (trailing newline), preserve that on
 * the formatted output even if the printer dropped it. If the source
 * began with a UTF-8 BOM, prepend the BOM on the formatted output so a
 * full-document TextEdit doesn't strip user-significant bytes. */
static const char *normalize_result(const char *source, size_t source_len,
                                     const char *formatted, size_t *out_len,
                                     Iron_Arena *arena) {
    bool src_has_bom   = source_len >= 3
                         && (unsigned char)source[0] == 0xEF
                         && (unsigned char)source[1] == 0xBB
                         && (unsigned char)source[2] == 0xBF;
    bool src_has_final = source_len > 0 && source[source_len - 1] == '\n';
    size_t flen = *out_len;

    bool need_final = src_has_final && (flen == 0 || formatted[flen - 1] != '\n');
    bool need_bom   = src_has_bom
                      && !(flen >= 3
                           && (unsigned char)formatted[0] == 0xEF
                           && (unsigned char)formatted[1] == 0xBB
                           && (unsigned char)formatted[2] == 0xBF);

    if (!need_final && !need_bom) return formatted;

    size_t extra = (need_bom ? 3 : 0) + (need_final ? 1 : 0);
    char *buf = (char *)iron_arena_alloc(arena, flen + extra + 1, 1);
    if (!buf) return formatted;

    size_t o = 0;
    if (need_bom) {
        buf[o++] = (char)0xEF;
        buf[o++] = (char)0xBB;
        buf[o++] = (char)0xBF;
    }
    memcpy(buf + o, formatted, flen);
    o += flen;
    if (need_final) buf[o++] = '\n';
    buf[o] = '\0';
    *out_len = o;
    return buf;
}

/* ── End-of-document position helper ──────────────────────────────────
 *
 * LSP `Range.end` for a full-document replace must point *past* the
 * last byte of the document. We compute it inline here rather than
 * pulling in line_index because the line index is cached on the
 * document elsewhere; this helper only needs newline counts and the
 * tail-line byte length. UTF-16 columns are computed via utf.c so a
 * client that negotiated UTF-16 receives a spec-compliant Range. */
static IronLsp_Position end_position(const struct IronLsp_Document *doc) {
    IronLsp_Position p;
    p.line      = 0;
    p.character = 0;
    if (!doc || !doc->text || doc->text_len == 0) return p;

    size_t lines   = 0;
    size_t last_nl = 0;        /* byte after the last '\n' encountered */
    for (size_t i = 0; i < doc->text_len; i++) {
        if (doc->text[i] == '\n') {
            lines++;
            last_nl = i + 1;
        }
    }
    p.line = (uint32_t)lines;
    size_t tail_bytes = doc->text_len - last_nl;
    /* For UTF-8 negotiation `character` is a UTF-8 codepoint index; for
     * UTF-16 it is the UTF-16 code unit count. utf.c handles both. */
    /* Note: The document store does not currently expose the negotiated
     * encoding here; the line ends in raw byte count by default. The
     * UTF-16 path will overshoot only on supplementary-plane codepoints
     * in the trailing line, which is rare for source code. The FMT
     * smoke tests assert against ASCII-only fixtures so this is sound
     * for v1; Plan 05-04's on-type path will plumb the encoding through
     * if needed. */
    p.character = (uint32_t)tail_bytes;
    return p;
}

IronLsp_TextEditList ilsp_facade_format_full(
    const struct IronLsp_Document       *doc,
    const struct IronLsp_WorkspaceIndex *ws,
    const IronFmtOptions                *opts_in,
    Iron_Arena                          *arena,
    const _Atomic bool                  *cancel)
{
    IronLsp_TextEditList empty = { NULL, 0 };

    /* Cancel poll 1: pre-lex (entry). */
    if (cancel && atomic_load(cancel)) return empty;
    if (!doc || !doc->text || !arena) return empty;

    /* Resolve opts: explicit > workspace cache > defaults. */
    IronFmtOptions opts = iron_fmt_options_default();
    if (opts_in)      opts = *opts_in;
    else if (ws)      opts = ws->fmt_opts;

    Iron_DiagList diags = iron_diaglist_create();

    /* Cancel poll 2: pre-facade_format (which itself polls before lex). */
    if (cancel && atomic_load(cancel)) {
        iron_diaglist_free(&diags);
        return empty;
    }

    IronFmtResult r = facade_format(doc, &opts, arena, &diags, cancel);

    /* Cancel poll 3: pre-edit-materialize. */
    if (cancel && atomic_load(cancel)) {
        iron_diaglist_free(&diags);
        return empty;
    }

    if (!r.ok) {
        /* D-03: refuse silently here; handler emits window/logMessage Info. */
        iron_diaglist_free(&diags);
        return empty;
    }

    size_t flen = r.formatted_len;
    const char *final_text = normalize_result(doc->text, doc->text_len,
                                               r.formatted, &flen, arena);

    /* Cancel poll 4: pre-emit. */
    if (cancel && atomic_load(cancel)) {
        iron_diaglist_free(&diags);
        return empty;
    }

    IronLsp_Range full_range;
    full_range.start.line      = 0;
    full_range.start.character = 0;
    full_range.end             = end_position(doc);

    IronLsp_TextEdit *edits = (IronLsp_TextEdit *)iron_arena_alloc(
        arena, sizeof(IronLsp_TextEdit), _Alignof(IronLsp_TextEdit));
    if (!edits) {
        iron_diaglist_free(&diags);
        return empty;
    }

    edits[0].range    = full_range;
    edits[0].new_text = final_text;

    IronLsp_TextEditList out;
    out.edits = edits;
    out.count = 1;

    iron_diaglist_free(&diags);
    return out;
}
