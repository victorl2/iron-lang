/* Phase 4 Plan 04-02 Task 02 (EDIT-01, D-01, D-17) -- completion facade
 * orchestrator implementation.
 *
 * Routes through ilsp_facade_compile_for_nav so the single
 * iron_analyze_buffer call site invariant (CORE-22) stays intact; no
 * new analyzer call sites are introduced. On cold-start (program
 * returned NULL) we fall back to parser-only identifier extraction +
 * keyword bucket so the user still gets a useful list during the
 * initial warm-up window.
 */

#include "lsp/facade/edit/complete/complete.h"
#include "lsp/facade/edit/complete/buckets.h"
#include "lsp/facade/edit/complete/context_classify.h"
#include "lsp/facade/compile.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/utf.h"
#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ── Byte math ────────────────────────────────────────────────────── */

static size_t pos_to_byte(const struct IronLsp_Document *doc,
                            IronLsp_Position pos,
                            IronLsp_PositionEncoding enc) {
    if (!doc || !doc->text) return 0;
    size_t line_start = ilsp_byte_of_line(&doc->line_idx, pos.line);
    if (line_start > doc->text_len) return doc->text_len;
    size_t line_end = line_start;
    while (line_end < doc->text_len && doc->text[line_end] != '\n') line_end++;
    const char *line = doc->text + line_start;
    size_t line_len = line_end - line_start;
    size_t byte;
    if (enc == ILSP_ENC_UTF16) {
        byte = ilsp_utf16_column_to_utf8_byte(line, line_len, pos.character);
    } else {
        byte = ilsp_utf8_column_to_utf8_byte(line, line_len, pos.character);
    }
    if (byte > line_len) byte = line_len;
    return line_start + byte;
}

/* Walk backward over identifier characters to extract the query prefix
 * the user is actively typing. Returns an arena-owned NUL-terminated
 * string (empty string if cursor is not inside an ident). */
static const char *extract_query_prefix(const struct IronLsp_Document *doc,
                                          size_t byte_off,
                                          Iron_Arena *arena) {
    if (!doc || !doc->text || byte_off > doc->text_len) return "";
    size_t end = byte_off;
    size_t start = end;
    while (start > 0) {
        unsigned char c = (unsigned char)doc->text[start - 1];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_') { start--; continue; }
        break;
    }
    if (start == end) return "";
    return iron_arena_strdup(arena, doc->text + start, end - start);
}

/* ── qsort comparator ─────────────────────────────────────────────── */

static int candidate_cmp(const void *pa, const void *pb) {
    const IronLsp_CompletionCandidate *a = (const IronLsp_CompletionCandidate *)pa;
    const IronLsp_CompletionCandidate *b = (const IronLsp_CompletionCandidate *)pb;
    if (a->bucket != b->bucket) return a->bucket - b->bucket;
    if (a->fuzzy_score != b->fuzzy_score)
        return (b->fuzzy_score > a->fuzzy_score) ? 1 : -1;
    const char *la = a->label ? a->label : "";
    const char *lb = b->label ? b->label : "";
    return strcmp(la, lb);
}

/* ── Public API ───────────────────────────────────────────────────── */

#define ILSP_COMPLETION_MAX_RESULTS 128

void ilsp_facade_complete(struct IronLsp_Server          *server,
                            struct IronLsp_Document        *doc,
                            IronLsp_Position                pos,
                            _Atomic bool                   *cancel,
                            Iron_Arena                     *arena,
                            IronLsp_CompletionCandidate   **out,
                            size_t                         *out_n,
                            bool                           *out_is_incomplete) {
    if (out)               *out = NULL;
    if (out_n)             *out_n = 0;
    if (out_is_incomplete) *out_is_incomplete = false;
    if (!server || !doc || !arena || !out || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags      = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &diags);
    /* program may be NULL on cold-start / parse-fatal; the bucket
     * builder accepts NULL program and simply emits empty top-level
     * + empty local + keyword bucket (for EXPR_HEAD / STATEMENT_HEAD). */
    if (cancel && atomic_load(cancel)) goto done;

    size_t cursor_byte = pos_to_byte(doc, pos, enc);
    const char *qp = extract_query_prefix(doc, cursor_byte, arena);
    IronLsp_CompletionContext ctx =
        ilsp_completion_context_classify(doc, cursor_byte);

    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    ilsp_complete_buckets_build(server, doc, program, cursor_byte,
                                  ctx, qp, cancel, arena, &cands, &n);
    if (cancel && atomic_load(cancel)) goto done;
    if (n == 0) goto done;

    qsort(cands, n, sizeof(*cands), candidate_cmp);

    if (n > ILSP_COMPLETION_MAX_RESULTS) {
        n = ILSP_COMPLETION_MAX_RESULTS;
    }

    *out = cands;
    *out_n = n;

done:
    iron_diaglist_free(&diags);
    iron_arena_free(&walk_arena);
}
