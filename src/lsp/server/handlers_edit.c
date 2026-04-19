/* Phase 4 Plan 04-02 Task 03 -- EDIT handlers: textDocument/completion
 * + completionItem/resolve.
 *
 * Uses the same enqueue_json / enqueue_result / lookup_doc /
 * register_cancel helper shape as handlers_nav.c; helpers live in this
 * TU as private statics (duplication over cross-module shared state is
 * acceptable per PATTERNS.md §S2). */

#include "lsp/server/handlers_edit.h"
#include "lsp/server/server.h"
#include "lsp/server/cancel.h"
#include "lsp/store/document.h"
#include "lsp/facade/edit/complete/complete.h"
#include "lsp/facade/edit/complete/resolve.h"
#include "lsp/facade/edit/complete/buckets.h"
#include "lsp/facade/edit/codeaction/codeaction.h"
#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/span.h"
#include "lsp/facade/types.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/json.h"
#include "lsp/transport/types.h"
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Local helpers (duplicated from handlers_nav.c per PATTERNS.md) ── */

static void enqueue_json(IronLsp_Server *s, yyjson_mut_doc *d,
                          Iron_Arena *arena) {
    if (!s || !d || !arena || !s->writer) return;
    size_t len = 0;
    char *body = ilsp_json_write_mut(d, arena, &len);
    if (!body || len == 0) return;
    char *heap = (char *)malloc(len);
    if (!heap) return;
    memcpy(heap, body, len);
    ilsp_writer_enqueue(s->writer, ILSP_PRIO_RESPONSE, heap, len);
}

static void enqueue_result(IronLsp_Server *s, Iron_Arena *arena,
                            yyjson_val *request_id,
                            yyjson_mut_doc *d, yyjson_mut_val *result) {
    ilsp_nav_wrap_response(d, request_id, result);
    enqueue_json(s, d, arena);
}

static IronLsp_Document *lookup_doc(IronLsp_Server *s, const char *uri) {
    if (!s || !s->documents || !uri) return NULL;
    ptrdiff_t idx = shgeti(s->documents, uri);
    if (idx < 0) return NULL;
    return s->documents[idx].value;
}

static _Atomic bool *register_cancel(IronLsp_Server *s, yyjson_val *id_v) {
    if (!s || !s->cancels) return NULL;
    char *key = ilsp_nav_stringify_id(id_v);
    if (!key) return NULL;
    _Atomic bool *flag = ilsp_cancel_register(s->cancels, key);
    free(key);
    return flag;
}

/* ── sortText encoding ─────────────────────────────────────────────── */

/* Build "<bucket>-<99-score_bucket>-<label>" for stable LSP sort per
 * D-01. Fuzzy scores are positive doubles; we quantize to a 2-digit
 * 00-99 field (higher score => smaller sort bucket). */
static const char *make_sort_text(int bucket, double score,
                                    const char *label, Iron_Arena *arena) {
    /* Clamp score to a reasonable range for quantization. */
    int score_q = (int)(score * 10.0);
    if (score_q < 0)   score_q = 0;
    if (score_q > 99)  score_q = 99;
    int inv = 99 - score_q;
    const char *l = label ? label : "";
    size_t need = strlen(l) + 32;
    char *buf = (char *)iron_arena_alloc(arena, need, 1);
    if (!buf) return "";
    snprintf(buf, need, "%d-%02d-%s", bucket, inv, l);
    return buf;
}

/* ── textDocument/completion ──────────────────────────────────────── */

static yyjson_mut_val *candidate_to_json(yyjson_mut_doc *rd,
                                           const IronLsp_CompletionCandidate *c,
                                           Iron_Arena *arena) {
    yyjson_mut_val *o = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, o, "label", c->label ? c->label : "");
    yyjson_mut_obj_add_int   (rd, o, "kind",  c->kind);
    yyjson_mut_obj_add_strcpy(rd, o, "sortText",
        make_sort_text(c->bucket, c->fuzzy_score, c->label, arena));
    yyjson_mut_obj_add_strcpy(rd, o, "filterText", c->label ? c->label : "");
    yyjson_mut_obj_add_strcpy(rd, o, "insertText",
        c->insert_text ? c->insert_text : (c->label ? c->label : ""));
    /* Phase 4 Plan 04-03 Task 03: insertTextFormat comes from the
     * candidate. The orchestrator sets 2 (Snippet) only when the
     * client advertised snippetSupport AND the candidate mapped to
     * one of the D-15 template shapes; 1 (PlainText) otherwise. */
    int itf = c->insert_text_format > 0 ? c->insert_text_format : 1;
    yyjson_mut_obj_add_int   (rd, o, "insertTextFormat", itf);
    if (c->detail && *c->detail) {
        yyjson_mut_obj_add_strcpy(rd, o, "detail", c->detail);
    }
    /* Phase 4 Plan 04-03 Task 03: additionalTextEdits carries a
     * single zero-width-range insertion TextEdit when the candidate
     * needs a new `import <mod>` line injected at the top of the
     * file (D-02 EDIT-05 auto-import). When empty we still emit `[]`
     * per LSP 3.17 spec -- clients can rely on the array field
     * existing. Dedup race (PITFALL E): if two completions accept
     * quickly, the client may linearize two identical import
     * insertions; organizeImports (Plan 04-05) cleans up. */
    yyjson_mut_val *edits = yyjson_mut_arr(rd);
    if (c->additional_text_edit && c->additional_text_edit->new_text) {
        const IronLsp_AutoImportEdit *te = c->additional_text_edit;
        yyjson_mut_val *edit = yyjson_mut_obj(rd);
        yyjson_mut_val *range = yyjson_mut_obj(rd);
        yyjson_mut_val *start = yyjson_mut_obj(rd);
        yyjson_mut_val *end   = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, start, "line",      te->line);
        yyjson_mut_obj_add_uint(rd, start, "character", te->character);
        yyjson_mut_obj_add_uint(rd, end,   "line",      te->line);
        yyjson_mut_obj_add_uint(rd, end,   "character", te->character);
        yyjson_mut_obj_add_val (rd, range, "start", start);
        yyjson_mut_obj_add_val (rd, range, "end",   end);
        yyjson_mut_obj_add_val (rd, edit,  "range",   range);
        yyjson_mut_obj_add_strcpy(rd, edit, "newText", te->new_text);
        yyjson_mut_arr_append(edits, edit);
    }
    yyjson_mut_obj_add_val(rd, o, "additionalTextEdits", edits);

    /* data: opaque round-trip handle consumed by completionItem/resolve. */
    yyjson_mut_val *data = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, data, "canonical_path",
        c->canonical_path ? c->canonical_path : "");
    yyjson_mut_obj_add_strcpy(rd, data, "name_path",
        c->name_path ? c->name_path : "");
    yyjson_mut_obj_add_int (rd, data, "bucket", c->bucket);
    yyjson_mut_obj_add_uint(rd, data, "content_hash", c->content_hash);
    yyjson_mut_obj_add_val (rd, o, "data", data);

    return o;
}

void ilsp_handle_text_document_completion(IronLsp_Server *s,
                                            struct yyjson_doc *doc,
                                            Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    const char *uri = ilsp_nav_parse_uri(params);
    IronLsp_Position pos = {0};
    bool have_pos = ilsp_nav_parse_position(params, &pos);

    Iron_Arena    body_arena = iron_arena_create(8 * 1024);
    yyjson_alc    alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd        = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    IronLsp_Document *d = lookup_doc(s, uri);
    if (!d || !have_pos) {
        /* Empty CompletionList. */
        yyjson_mut_val *empty = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_bool(rd, empty, "isIncomplete", false);
        yyjson_mut_obj_add_val (rd, empty, "items", yyjson_mut_arr(rd));
        enqueue_result(s, &body_arena, id_v, rd, empty);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    _Atomic bool *cancel = register_cancel(s, id_v);

    Iron_Arena work_arena = iron_arena_create(128 * 1024);
    IronLsp_CompletionCandidate *cands = NULL;
    size_t n = 0;
    bool is_incomplete = false;
    ilsp_facade_complete(s, d, pos, cancel, &work_arena,
                           &cands, &n, &is_incomplete);

    if (cancel && atomic_load(cancel)) {
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    yyjson_mut_val *result = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_bool(rd, result, "isIncomplete", is_incomplete);
    yyjson_mut_val *items = yyjson_mut_arr(rd);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *o = candidate_to_json(rd, &cands[i], &body_arena);
        yyjson_mut_arr_append(items, o);
    }
    yyjson_mut_obj_add_val(rd, result, "items", items);
    enqueue_result(s, &body_arena, id_v, rd, result);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}

/* ── completionItem/resolve ───────────────────────────────────────── */

/* Clone a JSON value by streaming it through a write/parse cycle into
 * the response doc. Simpler than deep-walking heterogeneous yyjson
 * trees and sufficient for the item echo we need. */
static yyjson_mut_val *clone_json_val(yyjson_mut_doc *rd, yyjson_val *src,
                                        Iron_Arena *arena) {
    if (!src) return yyjson_mut_null(rd);
    size_t len = 0;
    yyjson_write_err err;
    memset(&err, 0, sizeof(err));
    char *s = yyjson_val_write(src, 0, &len);
    if (!s) return yyjson_mut_null(rd);
    yyjson_read_err rerr;
    memset(&rerr, 0, sizeof(rerr));
    yyjson_doc *tmp = yyjson_read(s, len, 0);
    free(s);
    if (!tmp) return yyjson_mut_null(rd);
    yyjson_mut_val *clone = yyjson_val_mut_copy(rd, yyjson_doc_get_root(tmp));
    yyjson_doc_free(tmp);
    (void)arena;
    return clone ? clone : yyjson_mut_null(rd);
}

void ilsp_handle_completion_item_resolve(IronLsp_Server *s,
                                           struct yyjson_doc *doc,
                                           Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    Iron_Arena    body_arena = iron_arena_create(8 * 1024);
    yyjson_alc    alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd        = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    _Atomic bool *cancel = register_cancel(s, id_v);

    /* Parse the `data` handle back into IronLsp_CompletionResolveData. */
    yyjson_val *data_v = yyjson_obj_get(params, "data");
    IronLsp_CompletionResolveData rd_in = {0};
    Iron_Arena work_arena = iron_arena_create(32 * 1024);
    if (data_v) {
        yyjson_val *cp_v = yyjson_obj_get(data_v, "canonical_path");
        yyjson_val *np_v = yyjson_obj_get(data_v, "name_path");
        yyjson_val *bk_v = yyjson_obj_get(data_v, "bucket");
        yyjson_val *ch_v = yyjson_obj_get(data_v, "content_hash");
        const char *cp = yyjson_get_str(cp_v);
        const char *np = yyjson_get_str(np_v);
        if (cp) rd_in.canonical_path = iron_arena_strdup(&work_arena, cp, strlen(cp));
        if (np) rd_in.name_path      = iron_arena_strdup(&work_arena, np, strlen(np));
        if (bk_v && (yyjson_is_int(bk_v) || yyjson_is_uint(bk_v) || yyjson_is_sint(bk_v))) {
            rd_in.bucket = yyjson_get_int(bk_v);
        }
        if (ch_v && (yyjson_is_uint(ch_v) || yyjson_is_int(ch_v) || yyjson_is_sint(ch_v))) {
            rd_in.content_hash = yyjson_get_uint(ch_v);
        }
    }

    const char *detail    = NULL;
    const char *doc_md    = NULL;
    ilsp_facade_completion_resolve(s, &rd_in, cancel, &work_arena,
                                      &detail, &doc_md);

    if (cancel && atomic_load(cancel)) {
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    /* Echo the incoming item back, upgraded with detail + documentation
     * when the facade produced them. */
    yyjson_mut_val *item = clone_json_val(rd, params, &body_arena);
    if (yyjson_mut_is_obj(item)) {
        if (detail) {
            yyjson_mut_val *d_v = yyjson_mut_obj_get(item, "detail");
            if (d_v) yyjson_mut_obj_remove_str(item, "detail");
            yyjson_mut_obj_add_strcpy(rd, item, "detail", detail);
        }
        if (doc_md) {
            yyjson_mut_val *old = yyjson_mut_obj_get(item, "documentation");
            if (old) yyjson_mut_obj_remove_str(item, "documentation");
            yyjson_mut_val *documentation = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_strcpy(rd, documentation, "kind",  "markdown");
            yyjson_mut_obj_add_strcpy(rd, documentation, "value", doc_md);
            yyjson_mut_obj_add_val(rd, item, "documentation", documentation);
        }
    }
    enqueue_result(s, &body_arena, id_v, rd, item);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}

/* ── textDocument/codeAction + codeAction/resolve (Plan 04-04) ───── */

/* Parse an LSP Range out of a params object; returns true on success. */
static bool parse_lsp_range(yyjson_val *params, IronLsp_Range *out) {
    if (!params || !out) return false;
    yyjson_val *range = yyjson_obj_get(params, "range");
    if (!range || !yyjson_is_obj(range)) return false;
    yyjson_val *s = yyjson_obj_get(range, "start");
    yyjson_val *e = yyjson_obj_get(range, "end");
    if (!s || !e) return false;
    yyjson_val *sl = yyjson_obj_get(s, "line");
    yyjson_val *sc = yyjson_obj_get(s, "character");
    yyjson_val *el = yyjson_obj_get(e, "line");
    yyjson_val *ec = yyjson_obj_get(e, "character");
    if (!sl || !sc || !el || !ec) return false;
    out->start.line      = (uint32_t)yyjson_get_uint(sl);
    out->start.character = (uint32_t)yyjson_get_uint(sc);
    out->end.line        = (uint32_t)yyjson_get_uint(el);
    out->end.character   = (uint32_t)yyjson_get_uint(ec);
    return true;
}

/* Pull context.only kinds (if present) into a small array. Strings
 * point into the parsed yyjson tree; valid for the duration of the
 * request body_arena. */
static size_t parse_only_kinds(yyjson_val *params, const char **out_kinds,
                                  size_t max) {
    if (!params) return 0;
    yyjson_val *ctx = yyjson_obj_get(params, "context");
    if (!ctx || !yyjson_is_obj(ctx)) return 0;
    yyjson_val *only = yyjson_obj_get(ctx, "only");
    if (!only || !yyjson_is_arr(only)) return 0;
    size_t n = 0;
    size_t idx, maxn;
    yyjson_val *item;
    yyjson_arr_foreach(only, idx, maxn, item) {
        if (n >= max) break;
        const char *s = yyjson_get_str(item);
        if (s) out_kinds[n++] = s;
    }
    return n;
}

/* Serialize a single IronLsp_CodeAction to a JSON object. When
 * `include_edit` is true and the action carries an edit, a
 * WorkspaceEdit is attached. When false (initial codeAction response,
 * D-07 lazy), no edit field is emitted — client must call
 * codeAction/resolve. */
static yyjson_mut_val *code_action_to_json(yyjson_mut_doc               *rd,
                                              const IronLsp_CodeAction     *act,
                                              const char                   *doc_uri,
                                              int32_t                       doc_version,
                                              bool                          client_supports_doc_changes,
                                              bool                          include_edit) {
    yyjson_mut_val *obj = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, obj, "title",
                                 act->title ? act->title : "");
    yyjson_mut_obj_add_strcpy(rd, obj, "kind",
                                 act->kind ? act->kind : "quickfix");
    yyjson_mut_obj_add_bool(rd, obj, "isPreferred", act->is_preferred);

    /* diagnostics[] — attach the originating diagnostic so clients can
     * correlate the action with the squiggle. We emit a minimal
     * Diagnostic shape (range + severity + code + source + message);
     * publishDiagnostics' richer relatedInformation is not required by
     * the LSP CodeAction spec. */
    yyjson_mut_val *diag_arr = yyjson_mut_arr(rd);
    const Iron_Diagnostic *d = act->originating_diag;
    if (d) {
        yyjson_mut_val *dj = yyjson_mut_obj(rd);
        yyjson_mut_val *range = yyjson_mut_obj(rd);
        yyjson_mut_val *start = yyjson_mut_obj(rd);
        yyjson_mut_val *end   = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, start, "line",
            d->span.line > 0 ? d->span.line - 1 : 0);
        yyjson_mut_obj_add_uint(rd, start, "character",
            d->span.col  > 0 ? d->span.col  - 1 : 0);
        yyjson_mut_obj_add_uint(rd, end, "line",
            d->span.end_line > 0 ? d->span.end_line - 1 : 0);
        yyjson_mut_obj_add_uint(rd, end, "character",
            d->span.end_col  > 0 ? d->span.end_col  - 1 : 0);
        yyjson_mut_obj_add_val(rd, range, "start", start);
        yyjson_mut_obj_add_val(rd, range, "end",   end);
        yyjson_mut_obj_add_val(rd, dj, "range", range);
        int severity = 1;
        switch (d->level) {
            case IRON_DIAG_ERROR:   severity = 1; break;
            case IRON_DIAG_WARNING: severity = 2; break;
            case IRON_DIAG_NOTE:    severity = 3; break;
        }
        yyjson_mut_obj_add_int(rd, dj, "severity", severity);
        char codebuf[16];
        snprintf(codebuf, sizeof(codebuf), "E%04d", d->code);
        yyjson_mut_obj_add_strcpy(rd, dj, "code", codebuf);
        yyjson_mut_obj_add_strcpy(rd, dj, "source", "iron");
        yyjson_mut_obj_add_strcpy(rd, dj, "message",
                                     d->message ? d->message : "");
        yyjson_mut_arr_append(diag_arr, dj);
    }
    yyjson_mut_obj_add_val(rd, obj, "diagnostics", diag_arr);

    /* data: opaque round-trip payload for codeAction/resolve. */
    yyjson_mut_val *data = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_int(rd, data, "file_version",   act->data_file_version);
    yyjson_mut_obj_add_int(rd, data, "code",           act->data_code);
    yyjson_mut_obj_add_int(rd, data, "diagnostic_idx", act->data_diagnostic_idx);
    yyjson_mut_obj_add_val(rd, obj, "data", data);

    /* Optional edit — only on resolve. */
    if (include_edit && act->edit_new_text) {
        yyjson_mut_val *edit = yyjson_mut_obj(rd);
        /* TextEdit shape used for either documentChanges or changes. */
        yyjson_mut_val *te = yyjson_mut_obj(rd);
        yyjson_mut_val *range = yyjson_mut_obj(rd);
        yyjson_mut_val *start = yyjson_mut_obj(rd);
        yyjson_mut_val *end   = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, start, "line",      act->edit_start_line);
        yyjson_mut_obj_add_uint(rd, start, "character", act->edit_start_char);
        yyjson_mut_obj_add_uint(rd, end,   "line",      act->edit_end_line);
        yyjson_mut_obj_add_uint(rd, end,   "character", act->edit_end_char);
        yyjson_mut_obj_add_val(rd, range, "start", start);
        yyjson_mut_obj_add_val(rd, range, "end",   end);
        yyjson_mut_obj_add_val(rd, te, "range", range);
        yyjson_mut_obj_add_strcpy(rd, te, "newText", act->edit_new_text);

        if (client_supports_doc_changes) {
            /* documentChanges: [ { textDocument:{uri,version}, edits:[te] } ] */
            yyjson_mut_val *dc_arr = yyjson_mut_arr(rd);
            yyjson_mut_val *dc     = yyjson_mut_obj(rd);
            yyjson_mut_val *td     = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_strcpy(rd, td, "uri",
                                         doc_uri ? doc_uri : "");
            yyjson_mut_obj_add_int   (rd, td, "version", doc_version);
            yyjson_mut_obj_add_val   (rd, dc, "textDocument", td);
            yyjson_mut_val *edits = yyjson_mut_arr(rd);
            yyjson_mut_arr_append(edits, te);
            yyjson_mut_obj_add_val(rd, dc, "edits", edits);
            yyjson_mut_arr_append(dc_arr, dc);
            yyjson_mut_obj_add_val(rd, edit, "documentChanges", dc_arr);
        } else {
            /* changes: { uri: [te] } */
            yyjson_mut_val *changes = yyjson_mut_obj(rd);
            yyjson_mut_val *te_arr  = yyjson_mut_arr(rd);
            yyjson_mut_arr_append(te_arr, te);
            yyjson_mut_obj_add_val(rd, changes,
                                     doc_uri ? doc_uri : "", te_arr);
            yyjson_mut_obj_add_val(rd, edit, "changes", changes);
        }
        yyjson_mut_obj_add_val(rd, obj, "edit", edit);
    }

    return obj;
}

void ilsp_handle_text_document_code_action(IronLsp_Server    *s,
                                             struct yyjson_doc *doc,
                                             Iron_Arena        *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    const char *uri = ilsp_nav_parse_uri(params);
    IronLsp_Range range = {0};
    bool have_range = parse_lsp_range(params, &range);

    Iron_Arena    body_arena = iron_arena_create(8 * 1024);
    yyjson_alc    alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd       = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    IronLsp_Document *d = lookup_doc(s, uri);
    if (!d || !have_range) {
        yyjson_mut_val *empty = yyjson_mut_arr(rd);
        enqueue_result(s, &body_arena, id_v, rd, empty);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    /* Pull context.only kinds (up to 8; more is overkill for M3). */
    const char *kinds[8];
    size_t kind_n = parse_only_kinds(params, kinds, 8);
    IronLsp_CodeActionOnly only = { .kinds = kind_n ? kinds : NULL,
                                      .kind_n = kind_n };

    _Atomic bool *cancel = register_cancel(s, id_v);

    Iron_Arena work_arena = iron_arena_create(64 * 1024);
    IronLsp_CodeAction *actions = NULL;
    size_t n = 0;
    ilsp_facade_code_action(s, d, range, &only, cancel, &work_arena,
                               &actions, &n);

    if (cancel && atomic_load(cancel)) {
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *o = code_action_to_json(rd, &actions[i],
                                                    d->uri, d->version,
                                                    s->client_supports_document_changes,
                                                    /* include_edit = */ false);
        yyjson_mut_arr_append(arr, o);
    }
    enqueue_result(s, &body_arena, id_v, rd, arr);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}

void ilsp_handle_code_action_resolve(IronLsp_Server    *s,
                                       struct yyjson_doc *doc,
                                       Iron_Arena        *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    Iron_Arena    body_arena = iron_arena_create(8 * 1024);
    yyjson_alc    alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd       = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    /* Decode data.{file_version, code, diagnostic_idx}. Missing fields
     * are treated as "stale" — we echo the action back without an edit
     * rather than erroring. T-4-3 mitigation. */
    int fv = -1, code = -1, didx = -1;
    yyjson_val *data = yyjson_obj_get(params, "data");
    if (data && yyjson_is_obj(data)) {
        yyjson_val *fv_v = yyjson_obj_get(data, "file_version");
        yyjson_val *cd_v = yyjson_obj_get(data, "code");
        yyjson_val *id2  = yyjson_obj_get(data, "diagnostic_idx");
        if (fv_v && (yyjson_is_int(fv_v) || yyjson_is_sint(fv_v) || yyjson_is_uint(fv_v)))
            fv   = yyjson_get_int(fv_v);
        if (cd_v && (yyjson_is_int(cd_v) || yyjson_is_sint(cd_v) || yyjson_is_uint(cd_v)))
            code = yyjson_get_int(cd_v);
        if (id2  && (yyjson_is_int(id2)  || yyjson_is_sint(id2)  || yyjson_is_uint(id2)))
            didx = yyjson_get_int(id2);
    }

    /* Echo the original params object back — we merge the edit field
     * into it when the handler produces one. */
    yyjson_mut_val *item = clone_json_val(rd, params, &body_arena);
    if (!yyjson_mut_is_obj(item)) item = yyjson_mut_obj(rd);

    /* Locate the owning document for version comparison + facade call.
     * The LSP spec does not require the client to send textDocument
     * inside codeAction/resolve params; we try to recover it from the
     * originating diagnostics[0].<relatedInformation>.location.uri or
     * from a synthetic textDocument field if present. When absent we
     * search every open document for one whose version matches fv —
     * the only legitimate resolve target. */
    IronLsp_Document *d = NULL;
    yyjson_val *td = yyjson_obj_get(params, "textDocument");
    if (td) {
        yyjson_val *uri_v = yyjson_obj_get(td, "uri");
        const char *uri = yyjson_get_str(uri_v);
        if (uri) d = lookup_doc(s, uri);
    }
    if (!d) {
        /* Walk diagnostics[0].<location?> — not in LSP 3.17 CodeAction
         * shape, but our serializer never emitted a URI there; skip. */
    }
    if (!d && s->documents) {
        /* Fallback: a unique open doc whose version matches fv. */
        for (ptrdiff_t i = 0, m = shlen(s->documents); i < m; i++) {
            IronLsp_Document *cd = s->documents[i].value;
            if (cd && cd->version == fv) { d = cd; break; }
        }
    }

    _Atomic bool *cancel = register_cancel(s, id_v);

    /* Call the facade. Even when d is NULL we ship an echoed item
     * without edit so the client gracefully drops it. */
    Iron_Arena work_arena = iron_arena_create(32 * 1024);
    IronLsp_CodeAction out;
    memset(&out, 0, sizeof(out));
    if (d) {
        ilsp_facade_code_action_resolve(s, d, fv, code, didx, cancel,
                                           &work_arena, &out);
    }

    if (cancel && atomic_load(cancel)) {
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    /* Merge the edit into the echoed item. Remove any pre-existing
     * edit field first. */
    if (yyjson_mut_is_obj(item)) {
        yyjson_mut_val *old = yyjson_mut_obj_get(item, "edit");
        if (old) yyjson_mut_obj_remove_str(item, "edit");

        if (out.edit_new_text) {
            /* Build just the `edit` value via the shared serializer and
             * splice it in. code_action_to_json returns the full
             * action; we need only the `edit` it emitted. Re-render
             * separately so we don't overwrite the client's other
             * fields (title / kind / diagnostics from the input). */
            yyjson_mut_val *edit = yyjson_mut_obj(rd);
            yyjson_mut_val *te = yyjson_mut_obj(rd);
            yyjson_mut_val *range = yyjson_mut_obj(rd);
            yyjson_mut_val *start = yyjson_mut_obj(rd);
            yyjson_mut_val *end   = yyjson_mut_obj(rd);
            yyjson_mut_obj_add_uint(rd, start, "line",      out.edit_start_line);
            yyjson_mut_obj_add_uint(rd, start, "character", out.edit_start_char);
            yyjson_mut_obj_add_uint(rd, end,   "line",      out.edit_end_line);
            yyjson_mut_obj_add_uint(rd, end,   "character", out.edit_end_char);
            yyjson_mut_obj_add_val(rd, range, "start", start);
            yyjson_mut_obj_add_val(rd, range, "end",   end);
            yyjson_mut_obj_add_val(rd, te, "range", range);
            yyjson_mut_obj_add_strcpy(rd, te, "newText", out.edit_new_text);

            if (s->client_supports_document_changes) {
                yyjson_mut_val *dc_arr = yyjson_mut_arr(rd);
                yyjson_mut_val *dc     = yyjson_mut_obj(rd);
                yyjson_mut_val *td2    = yyjson_mut_obj(rd);
                yyjson_mut_obj_add_strcpy(rd, td2, "uri",
                                             d && d->uri ? d->uri : "");
                yyjson_mut_obj_add_int   (rd, td2, "version",
                                             d ? d->version : 0);
                yyjson_mut_obj_add_val   (rd, dc, "textDocument", td2);
                yyjson_mut_val *edits = yyjson_mut_arr(rd);
                yyjson_mut_arr_append(edits, te);
                yyjson_mut_obj_add_val(rd, dc, "edits", edits);
                yyjson_mut_arr_append(dc_arr, dc);
                yyjson_mut_obj_add_val(rd, edit, "documentChanges", dc_arr);
            } else {
                yyjson_mut_val *changes = yyjson_mut_obj(rd);
                yyjson_mut_val *te_arr  = yyjson_mut_arr(rd);
                yyjson_mut_arr_append(te_arr, te);
                yyjson_mut_obj_add_val(rd, changes,
                                         d && d->uri ? d->uri : "", te_arr);
                yyjson_mut_obj_add_val(rd, edit, "changes", changes);
            }
            yyjson_mut_obj_add_val(rd, item, "edit", edit);
        } else {
            /* Stale or unresolvable: emit `edit: null` so the client
             * drops the action cleanly (D-07). */
            yyjson_mut_obj_add_val(rd, item, "edit", yyjson_mut_null(rd));
        }
    }
    enqueue_result(s, &body_arena, id_v, rd, item);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}
