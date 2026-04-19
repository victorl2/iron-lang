/* Phase 5 Plan 05-02 (FMT-02, D-03, D-14) -- formatting request handlers.
 *
 * Three dispatch handlers route into the fmt facade:
 *
 *   - ilsp_handle_text_document_formatting        (FMT-02, LIVE)
 *   - ilsp_handle_text_document_range_formatting  (FMT-03, STUB -> Plan 05-03)
 *   - ilsp_handle_text_document_on_type_formatting(FMT-04, STUB -> Plan 05-04)
 *
 * The local enqueue_json / enqueue_result / lookup_doc / register_cancel
 * helpers are DUPLICATED from handlers_edit.c per PATTERNS.md §S2 (the
 * "duplication over cross-module shared state" rule Phase 3 and 4 both
 * used). They are file-static so there is no ODR collision.
 *
 * D-12: per-request 64 KB work arena (HARD-06). D-03: empty TextEdit[]
 * on refusal + one-line window/logMessage Info. D-10: the sole in-TU
 * call that reaches the compiler-side iron_format_source lives inside
 * src/lsp/facade/fmt/format.c -- this TU calls ilsp_facade_format_full
 * only, preserving the single-call-site invariant. */

#include "lsp/server/handlers_fmt.h"

#include "lsp/server/server.h"
#include "lsp/server/cancel.h"
#include "lsp/server/notifications.h"
#include "lsp/facade/fmt/format.h"
#include "lsp/facade/nav/nav_common.h"   /* wrap_response, parse_uri, stringify_id */
#include "lsp/facade/types.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/json.h"
#include "lsp/transport/types.h"
#include "vendor/yyjson/yyjson.h"
#include "vendor/stb_ds.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Local helpers (duplicated from handlers_edit.c per PATTERNS.md §S2) ── */

static void fmt_enqueue_json(IronLsp_Server *s, yyjson_mut_doc *d,
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

static void fmt_enqueue_result(IronLsp_Server *s, Iron_Arena *arena,
                                yyjson_val *request_id,
                                yyjson_mut_doc *d, yyjson_mut_val *result) {
    ilsp_nav_wrap_response(d, request_id, result);
    fmt_enqueue_json(s, d, arena);
}

static IronLsp_Document *fmt_lookup_doc(IronLsp_Server *s, const char *uri) {
    if (!s || !s->documents || !uri) return NULL;
    ptrdiff_t idx = shgeti(s->documents, uri);
    if (idx < 0) return NULL;
    return s->documents[idx].value;
}

static _Atomic bool *fmt_register_cancel(IronLsp_Server *s, yyjson_val *id_v) {
    if (!s || !s->cancels) return NULL;
    char *key = ilsp_nav_stringify_id(id_v);
    if (!key) return NULL;
    _Atomic bool *flag = ilsp_cancel_register(s->cancels, key);
    free(key);
    return flag;
}

/* ── TextEdit[] serializer ───────────────────────────────────────────── */

static yyjson_mut_val *serialize_text_edits(yyjson_mut_doc             *rd,
                                             const IronLsp_TextEditList *list) {
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    if (!list || list->count == 0) return arr;

    for (size_t i = 0; i < list->count; i++) {
        yyjson_mut_val *obj = yyjson_mut_obj(rd);

        yyjson_mut_val *range = yyjson_mut_obj(rd);
        yyjson_mut_val *start = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, start, "line",
            list->edits[i].range.start.line);
        yyjson_mut_obj_add_uint(rd, start, "character",
            list->edits[i].range.start.character);
        yyjson_mut_val *end = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, end, "line",
            list->edits[i].range.end.line);
        yyjson_mut_obj_add_uint(rd, end, "character",
            list->edits[i].range.end.character);
        yyjson_mut_obj_add_val(rd, range, "start", start);
        yyjson_mut_obj_add_val(rd, range, "end",   end);
        yyjson_mut_obj_add_val(rd, obj,   "range", range);

        yyjson_mut_obj_add_strcpy(rd, obj, "newText",
            list->edits[i].new_text ? list->edits[i].new_text : "");

        yyjson_mut_arr_append(arr, obj);
    }
    return arr;
}

/* ── textDocument/formatting (FMT-02, LIVE) ─────────────────────────── */

void ilsp_handle_text_document_formatting(IronLsp_Server    *s,
                                            struct yyjson_doc *doc,
                                            Iron_Arena        *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    const char *uri = ilsp_nav_parse_uri(params);

    /* Response-doc arena: short-lived yyjson build. */
    Iron_Arena      body_arena = iron_arena_create(8 * 1024);
    yyjson_alc      alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd         = yyjson_mut_doc_new(&alc);
    if (!rd) {
        iron_arena_free(&body_arena);
        return;
    }

    IronLsp_Document *d = fmt_lookup_doc(s, uri);
    if (!d) {
        /* Unknown document: empty TextEdit[] (spec-compliant). */
        fmt_enqueue_result(s, &body_arena, id_v, rd, yyjson_mut_arr(rd));
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    _Atomic bool *cancel = fmt_register_cancel(s, id_v);

    /* Per-request 64 KB work arena (D-12, HARD-06) holds the facade's
     * formatted bytes + TextEdit array until we serialize them into rd. */
    Iron_Arena work_arena = iron_arena_create(64 * 1024);

    IronLsp_TextEditList result = ilsp_facade_format_full(
        d, s->workspace_index, /* opts */ NULL, &work_arena, cancel);

    if (cancel && atomic_load(cancel)) {
        /* Cancelled: drop response per Phase 2 policy. */
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    /* D-03: empty result == refusal. Emit window/logMessage Info once. */
    if (result.count == 0) {
        ilsp_send_window_logmessage(s, uri, ILSP_MESSAGE_TYPE_INFO,
            "iron-lsp: formatter refused due to parse errors");
    }

    yyjson_mut_val *arr = serialize_text_edits(rd, &result);
    fmt_enqueue_result(s, &body_arena, id_v, rd, arr);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}

/* ── textDocument/rangeFormatting (FMT-03, Plan 05-03, D-04, D-06) ─── */

void ilsp_handle_text_document_range_formatting(IronLsp_Server    *s,
                                                  struct yyjson_doc *doc,
                                                  Iron_Arena        *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    const char *uri = ilsp_nav_parse_uri(params);

    Iron_Arena      body_arena = iron_arena_create(8 * 1024);
    yyjson_alc      alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd         = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    /* Parse params.range -- { start:{line,character}, end:{...} }.
     * A missing range is a spec-violation on the client side; we emit
     * empty TextEdit[] rather than MethodNotFound. */
    yyjson_val *range_v = yyjson_obj_get(params, "range");
    if (!range_v) {
        fmt_enqueue_result(s, &body_arena, id_v, rd, yyjson_mut_arr(rd));
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }
    yyjson_val *start_v = yyjson_obj_get(range_v, "start");
    yyjson_val *end_v   = yyjson_obj_get(range_v, "end");
    if (!start_v || !end_v) {
        fmt_enqueue_result(s, &body_arena, id_v, rd, yyjson_mut_arr(rd));
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    IronLsp_Range req_range;
    req_range.start.line      =
        (uint32_t)yyjson_get_uint(yyjson_obj_get(start_v, "line"));
    req_range.start.character =
        (uint32_t)yyjson_get_uint(yyjson_obj_get(start_v, "character"));
    req_range.end.line        =
        (uint32_t)yyjson_get_uint(yyjson_obj_get(end_v,   "line"));
    req_range.end.character   =
        (uint32_t)yyjson_get_uint(yyjson_obj_get(end_v,   "character"));

    /* T-05-03-01 mitigation: reject inverted ranges (start > end). */
    if (req_range.end.line < req_range.start.line ||
        (req_range.end.line == req_range.start.line &&
         req_range.end.character < req_range.start.character)) {
        fmt_enqueue_result(s, &body_arena, id_v, rd, yyjson_mut_arr(rd));
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    IronLsp_Document *d = fmt_lookup_doc(s, uri);
    if (!d) {
        fmt_enqueue_result(s, &body_arena, id_v, rd, yyjson_mut_arr(rd));
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    _Atomic bool *cancel = fmt_register_cancel(s, id_v);

    /* Per-request 64 KB work arena (D-12, HARD-06). */
    Iron_Arena work_arena = iron_arena_create(64 * 1024);

    IronLsp_TextEditList result = ilsp_facade_format_range(
        d, s->workspace_index, req_range, /* opts */ NULL,
        &work_arena, cancel);

    if (cancel && atomic_load(cancel)) {
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    yyjson_mut_val *arr = serialize_text_edits(rd, &result);
    fmt_enqueue_result(s, &body_arena, id_v, rd, arr);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}

/* ── textDocument/onTypeFormatting (FMT-04, Plan 05-04, D-05, D-14) ── */

void ilsp_handle_text_document_on_type_formatting(IronLsp_Server    *s,
                                                    struct yyjson_doc *doc,
                                                    Iron_Arena        *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    const char *uri = ilsp_nav_parse_uri(params);

    Iron_Arena      body_arena = iron_arena_create(8 * 1024);
    yyjson_alc      alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd         = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    /* Parse params.position + params.ch. Missing fields = empty edits
     * (spec-violation on the client side; we emit empty TextEdit[]
     * rather than MethodNotFound). */
    yyjson_val *pos_v = yyjson_obj_get(params, "position");
    yyjson_val *ch_v  = yyjson_obj_get(params, "ch");
    if (!pos_v || !ch_v) {
        fmt_enqueue_result(s, &body_arena, id_v, rd, yyjson_mut_arr(rd));
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    IronLsp_Position position;
    position.line      =
        (uint32_t)yyjson_get_uint(yyjson_obj_get(pos_v, "line"));
    position.character =
        (uint32_t)yyjson_get_uint(yyjson_obj_get(pos_v, "character"));

    const char *ch_str = yyjson_get_str(ch_v);
    char trigger = (ch_str && ch_str[0]) ? ch_str[0] : '\0';

    IronLsp_Document *d = fmt_lookup_doc(s, uri);
    if (!d) {
        fmt_enqueue_result(s, &body_arena, id_v, rd, yyjson_mut_arr(rd));
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    _Atomic bool *cancel = fmt_register_cancel(s, id_v);

    /* Per-request 64 KB work arena (D-12, HARD-06). */
    Iron_Arena work_arena = iron_arena_create(64 * 1024);

    IronLsp_TextEditList result = ilsp_facade_format_on_type(
        d, s->workspace_index, position, trigger, /* opts */ NULL,
        &work_arena, cancel);

    if (cancel && atomic_load(cancel)) {
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    yyjson_mut_val *arr = serialize_text_edits(rd, &result);
    fmt_enqueue_result(s, &body_arena, id_v, rd, arr);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}
