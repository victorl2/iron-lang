/* Phase 3 Plan 06 (NAV-12, NAV-13, D-12) -- workspace/diagnostic +
 * workspace/diagnostic/refresh.
 *
 * Pull handler delegates to ilsp_facade_workspace_diagnostic. The
 * facade returns an arena-owned array of IronLsp_WsDiagFileReport;
 * this handler wraps them in a JSON-RPC response shaped per LSP 3.17
 * WorkspaceDiagnosticReport:
 *
 *   { "result": {
 *       "items": [
 *         { "kind":"full", "uri":"...", "version":N, "resultId":"...",
 *           "items":[Diagnostic...] },
 *         { "kind":"unchanged", "uri":"...", "resultId":"..." }
 *       ]
 *   } }
 *
 * Refresh emitter mirrors src/lsp/server/notifications.c
 * `ilsp_send_window_showmessage`: arena-scoped yyjson build + heap copy
 * + ilsp_writer_enqueue at ILSP_PRIO_NOTIFICATION.
 */

#include "lsp/server/handlers_workspace_diag.h"

#include "lsp/facade/workspace_diagnostic.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/server/cancel.h"
#include "lsp/server/server.h"
#include "lsp/transport/json.h"
#include "lsp/transport/types.h"
#include "lsp/transport/writer.h"
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ── Shared helper: enqueue an arena-built yyjson body. ───────────── */

static void enqueue_body(IronLsp_Writer *w, IronLsp_Priority prio,
                          const char *body, size_t len) {
    if (!w || !body || len == 0) return;
    char *heap = (char *)malloc(len);
    if (!heap) return;
    memcpy(heap, body, len);
    ilsp_writer_enqueue(w, prio, heap, len);
}

/* ── Pull handler: workspace/diagnostic ─────────────────────────── */

void ilsp_handle_workspace_diagnostic(struct IronLsp_Server *s,
                                        struct yyjson_doc     *doc,
                                        Iron_Arena            *arena) {
    (void)arena;  /* we use our own per-call arena for the body. */
    if (!s || !doc) return;

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");

    /* Optional params.previousResultId (from a prior resultId). */
    const char *prev_result_id = NULL;
    if (params) {
        yyjson_val *pr = yyjson_obj_get(params, "previousResultId");
        if (pr && yyjson_is_str(pr)) prev_result_id = yyjson_get_str(pr);
    }

    /* Register a cancel flag keyed on the stringified id so the
     * facade polls it between files (D-16). */
    _Atomic bool *cancel = NULL;
    if (s->cancels) {
        char *key = ilsp_nav_stringify_id(id_v);
        if (key) {
            cancel = ilsp_cancel_register(s->cancels, key);
            free(key);
        }
    }

    /* Build response doc + facade work arena. */
    Iron_Arena      body_arena = iron_arena_create(8 * 1024);
    yyjson_alc      alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd         = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    yyjson_mut_val *root_obj = yyjson_mut_obj(rd);
    yyjson_mut_doc_set_root(rd, root_obj);
    yyjson_mut_obj_add_strcpy(rd, root_obj, "jsonrpc", "2.0");

    /* Clone the id. */
    if (id_v && !yyjson_is_null(id_v)) {
        if (yyjson_is_int(id_v) || yyjson_is_sint(id_v))
            yyjson_mut_obj_add_sint(rd, root_obj, "id", yyjson_get_sint(id_v));
        else if (yyjson_is_uint(id_v))
            yyjson_mut_obj_add_uint(rd, root_obj, "id", yyjson_get_uint(id_v));
        else if (yyjson_is_str(id_v))
            yyjson_mut_obj_add_strcpy(rd, root_obj, "id", yyjson_get_str(id_v));
        else
            yyjson_mut_obj_add_null(rd, root_obj, "id");
    } else {
        yyjson_mut_obj_add_null(rd, root_obj, "id");
    }

    /* Call the facade. */
    Iron_Arena fac_arena = iron_arena_create(32 * 1024);
    IronLsp_WsDiagFileReport *reports = NULL;
    size_t n_reports = 0;
    ilsp_facade_workspace_diagnostic(s, prev_result_id, cancel, rd,
                                      &fac_arena, &reports, &n_reports);

    /* If cancelled, drop silently (HARD-05). */
    if (cancel && atomic_load(cancel)) {
        iron_arena_free(&fac_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    /* result: { items: [ {kind, uri, version?, resultId, items?}, ... ] } */
    yyjson_mut_val *result = yyjson_mut_obj(rd);
    yyjson_mut_val *items_arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < n_reports; i++) {
        const IronLsp_WsDiagFileReport *rep = &reports[i];
        yyjson_mut_val *rep_obj = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_strcpy(rd, rep_obj, "kind",
                                    rep->kind ? rep->kind : "unchanged");
        yyjson_mut_obj_add_strcpy(rd, rep_obj, "uri",
                                    rep->uri ? rep->uri : "");
        if (rep->version >= 0) {
            yyjson_mut_obj_add_int(rd, rep_obj, "version", rep->version);
        }
        yyjson_mut_obj_add_strcpy(rd, rep_obj, "resultId",
                                    rep->result_id ? rep->result_id : "");
        if (rep->kind && strcmp(rep->kind, "full") == 0 && rep->items_json) {
            yyjson_mut_obj_add_val(rd, rep_obj, "items", rep->items_json);
        }
        yyjson_mut_arr_append(items_arr, rep_obj);
    }
    yyjson_mut_obj_add_val(rd, result, "items", items_arr);
    yyjson_mut_obj_add_val(rd, root_obj, "result", result);

    /* Serialize + enqueue. */
    size_t body_len = 0;
    char *body = ilsp_json_write_mut(rd, &body_arena, &body_len);
    if (body && body_len > 0) {
        enqueue_body(s->writer, ILSP_PRIO_RESPONSE, body, body_len);
    }

    iron_arena_free(&fac_arena);
    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
}

/* ── Push emitter: workspace/diagnostic/refresh ─────────────────── */

void ilsp_send_workspace_diagnostic_refresh(struct IronLsp_Server *s) {
    if (!s || !s->writer) return;

    /* LSP 3.17 classifies `workspace/diagnostic/refresh` as a
     * server-initiated REQUEST (not a pure notification): the server
     * assigns an id and the client acknowledges with a null response.
     * pygls/lsprotocol will reject a body that lacks an "id" field
     * (see JsonRpcInvalidParams on DiagnosticRefreshRequest).  We
     * allocate an id from server->next_request_id so the shape is
     * spec-compliant; the client's response is ignored (we don't
     * correlate it to anything). */
    Iron_Arena arena = iron_arena_create(2 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
    if (!doc) { iron_arena_free(&arena); return; }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "jsonrpc", "2.0");
    uint64_t req_id = atomic_fetch_add(&s->next_request_id, 1);
    yyjson_mut_obj_add_uint  (doc, root, "id",      (uint64_t)req_id);
    yyjson_mut_obj_add_strcpy(doc, root, "method",
                                "workspace/diagnostic/refresh");
    /* Per LSP 3.17 there are no params. */

    size_t body_len = 0;
    char *body = ilsp_json_write_mut(doc, &arena, &body_len);
    if (body && body_len > 0) {
        enqueue_body(s->writer, ILSP_PRIO_NOTIFICATION, body, body_len);
    }

    yyjson_mut_doc_free(doc);
    iron_arena_free(&arena);
}
