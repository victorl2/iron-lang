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
#include "lsp/facade/nav/nav_common.h"
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
    yyjson_mut_obj_add_int   (rd, o, "insertTextFormat", 1 /* PlainText */);
    if (c->detail && *c->detail) {
        yyjson_mut_obj_add_strcpy(rd, o, "detail", c->detail);
    }
    /* additionalTextEdits: empty array in Plan 04-02 (auto-import is
     * deferred to Plan 04-03). */
    yyjson_mut_val *empty_edits = yyjson_mut_arr(rd);
    yyjson_mut_obj_add_val(rd, o, "additionalTextEdits", empty_edits);

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
