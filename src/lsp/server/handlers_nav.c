/* Phase 3 Plan 03 Task 02/03 -- NAV request dispatchers.
 *
 * 5 handlers live in this TU:
 *   - textDocument/declaration      (Task 02, NAV-03)
 *   - textDocument/definition       (Task 02, NAV-02)
 *   - textDocument/typeDefinition   (Task 02, NAV-04)
 *   - textDocument/documentSymbol   (Task 03, NAV-07)
 *   - workspace/symbol              (Task 03, NAV-08)
 *
 * Each follows the 7-step pattern demonstrated by
 * ilsp_handle_text_document_diagnostic in handlers_document.c:
 *   1. Parse id_v, params, uri, position.
 *   2. Register cancel flag.
 *   3. Look up the open document (NULL for workspace/symbol).
 *   4. Delegate to the facade.
 *   5. If cancelled, drop the response.
 *   6. Build yyjson response with result = array / null.
 *   7. Enqueue at ILSP_PRIO_RESPONSE.
 *
 * Plan 03 does not detect `definitionLinkSupport` client capability;
 * we default to emitting LocationLink[] which every major editor (VSCode,
 * Neovim, Zed) accepts. Plan 04 will add a client-capability cache on
 * IronLsp_Server if necessary. */

#include "lsp/server/handlers_nav.h"
#include "lsp/server/server.h"
#include "lsp/server/cancel.h"
#include "lsp/store/document.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/nav_core.h"
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

/* Build and enqueue a response body. Takes arena-owned body bytes and
 * copies them to a malloc'd buffer that ownership transfers to the
 * writer. */
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

/* Wrap `result` in { jsonrpc, id, result } and enqueue. */
static void enqueue_result(IronLsp_Server *s, Iron_Arena *arena,
                            yyjson_val *request_id,
                            yyjson_mut_doc *d, yyjson_mut_val *result) {
    ilsp_nav_wrap_response(d, request_id, result);
    enqueue_json(s, d, arena);
}

/* Look up an open document. Returns NULL when unknown. */
static IronLsp_Document *lookup_doc(IronLsp_Server *s, const char *uri) {
    if (!s || !s->documents || !uri) return NULL;
    ptrdiff_t idx = shgeti(s->documents, uri);
    if (idx < 0) return NULL;
    return s->documents[idx].value;
}

/* Shared cancel-flag registration helper. Mirror of the pull-diagnostic
 * path: re-register per-request so the registry owns the pointer. */
static _Atomic bool *register_cancel(IronLsp_Server *s, yyjson_val *id_v) {
    if (!s || !s->cancels) return NULL;
    char *key = ilsp_nav_stringify_id(id_v);
    if (!key) return NULL;
    _Atomic bool *flag = ilsp_cancel_register(s->cancels, key);
    free(key);
    return flag;
}

/* ── LocationLink-returning handlers ──────────────────────────────── */

typedef void (*nav_resolver_t)(IronLsp_Server *,
                                IronLsp_Document *,
                                IronLsp_Position,
                                _Atomic bool *,
                                Iron_Arena *,
                                IronLsp_LocationLink **,
                                size_t *);

/* Shared dispatcher body for definition / declaration / typeDefinition. */
static void handle_locationlink_request(IronLsp_Server *s,
                                          struct yyjson_doc *doc,
                                          Iron_Arena *arena,
                                          nav_resolver_t resolver) {
    (void)arena;  /* per-request arena owned by the dispatcher; we use our own. */
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;

    const char *uri = ilsp_nav_parse_uri(params);
    IronLsp_Position pos = {0};
    bool have_pos = ilsp_nav_parse_position(params, &pos);

    /* Build the response doc upfront so failure paths can emit a
     * well-formed empty-array result rather than leaving the client
     * hanging. */
    Iron_Arena    body_arena = iron_arena_create(4 * 1024);
    yyjson_alc    alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd        = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    IronLsp_Document *d = lookup_doc(s, uri);
    if (!d || !have_pos) {
        /* No document or malformed position -> empty array result. */
        yyjson_mut_val *empty = yyjson_mut_arr(rd);
        enqueue_result(s, &body_arena, id_v, rd, empty);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    _Atomic bool *cancel = register_cancel(s, id_v);

    Iron_Arena            work_arena = iron_arena_create(64 * 1024);
    IronLsp_LocationLink *links = NULL;
    size_t                n     = 0;
    resolver(s, d, pos, cancel, &work_arena, &links, &n);

    if (cancel && atomic_load(cancel)) {
        /* Drop silently. */
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    yyjson_mut_val *arr = ilsp_nav_build_location_link_array(
        rd, links, n, /*client_supports_link=*/true);
    enqueue_result(s, &body_arena, id_v, rd, arr);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}

void ilsp_handle_text_document_definition(IronLsp_Server *s,
                                            struct yyjson_doc *doc,
                                            Iron_Arena *arena) {
    (void)arena;
    handle_locationlink_request(s, doc, arena,
                                 ilsp_facade_nav_definition);
}

void ilsp_handle_text_document_declaration(IronLsp_Server *s,
                                             struct yyjson_doc *doc,
                                             Iron_Arena *arena) {
    (void)arena;
    handle_locationlink_request(s, doc, arena,
                                 ilsp_facade_nav_declaration);
}

void ilsp_handle_text_document_type_definition(IronLsp_Server *s,
                                                 struct yyjson_doc *doc,
                                                 Iron_Arena *arena) {
    (void)arena;
    handle_locationlink_request(s, doc, arena,
                                 ilsp_facade_nav_type_definition);
}

/* ── DocumentSymbol ───────────────────────────────────────────────── */

/* Build a JSON object for a single DocSymbol tree node (recursive). */
static yyjson_mut_val *doc_sym_to_json(yyjson_mut_doc *rd,
                                         const IronLsp_DocSymbol *s) {
    yyjson_mut_val *o = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, o, "name", s->name ? s->name : "");
    if (s->detail) {
        yyjson_mut_obj_add_strcpy(rd, o, "detail", s->detail);
    }
    yyjson_mut_obj_add_int(rd, o, "kind", s->kind);

    /* range */
    yyjson_mut_val *range = yyjson_mut_obj(rd);
    yyjson_mut_val *rs    = yyjson_mut_obj(rd);
    yyjson_mut_val *re    = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, rs, "line",      s->range.start.line);
    yyjson_mut_obj_add_uint(rd, rs, "character", s->range.start.character);
    yyjson_mut_obj_add_uint(rd, re, "line",      s->range.end.line);
    yyjson_mut_obj_add_uint(rd, re, "character", s->range.end.character);
    yyjson_mut_obj_add_val(rd, range, "start", rs);
    yyjson_mut_obj_add_val(rd, range, "end",   re);
    yyjson_mut_obj_add_val(rd, o, "range", range);

    /* selectionRange */
    yyjson_mut_val *srange = yyjson_mut_obj(rd);
    yyjson_mut_val *ss     = yyjson_mut_obj(rd);
    yyjson_mut_val *se     = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, ss, "line",      s->selection_range.start.line);
    yyjson_mut_obj_add_uint(rd, ss, "character", s->selection_range.start.character);
    yyjson_mut_obj_add_uint(rd, se, "line",      s->selection_range.end.line);
    yyjson_mut_obj_add_uint(rd, se, "character", s->selection_range.end.character);
    yyjson_mut_obj_add_val(rd, srange, "start", ss);
    yyjson_mut_obj_add_val(rd, srange, "end",   se);
    yyjson_mut_obj_add_val(rd, o, "selectionRange", srange);

    /* children */
    if (s->child_count > 0) {
        yyjson_mut_val *arr = yyjson_mut_arr(rd);
        for (size_t i = 0; i < s->child_count; i++) {
            yyjson_mut_val *co = doc_sym_to_json(rd, &s->children[i]);
            yyjson_mut_arr_append(arr, co);
        }
        yyjson_mut_obj_add_val(rd, o, "children", arr);
    }
    return o;
}

void ilsp_handle_text_document_document_symbol(IronLsp_Server *s,
                                                 struct yyjson_doc *doc,
                                                 Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;
    const char *uri = ilsp_nav_parse_uri(params);

    Iron_Arena    body_arena = iron_arena_create(4 * 1024);
    yyjson_alc    alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd        = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    IronLsp_Document *d = lookup_doc(s, uri);
    if (!d) {
        yyjson_mut_val *empty = yyjson_mut_arr(rd);
        enqueue_result(s, &body_arena, id_v, rd, empty);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    _Atomic bool *cancel = register_cancel(s, id_v);

    Iron_Arena           work_arena = iron_arena_create(64 * 1024);
    IronLsp_DocSymbol   *syms = NULL;
    size_t               n    = 0;
    ilsp_facade_nav_document_symbol(s, d, cancel, &work_arena,
                                     &syms, &n, /*hierarchical=*/true);

    if (cancel && atomic_load(cancel)) {
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *o = doc_sym_to_json(rd, &syms[i]);
        yyjson_mut_arr_append(arr, o);
    }
    enqueue_result(s, &body_arena, id_v, rd, arr);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}

/* ── workspace/symbol ─────────────────────────────────────────────── */

static yyjson_mut_val *ws_sym_to_json(yyjson_mut_doc *rd,
                                        const IronLsp_WorkspaceSymbol *ws) {
    yyjson_mut_val *o = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, o, "name", ws->name ? ws->name : "");
    yyjson_mut_obj_add_int   (rd, o, "kind", ws->kind);
    if (ws->container_name && *ws->container_name) {
        yyjson_mut_obj_add_strcpy(rd, o, "containerName", ws->container_name);
    }

    /* location: { uri, range } */
    yyjson_mut_val *loc = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, loc, "uri", ws->uri ? ws->uri : "");

    yyjson_mut_val *range = yyjson_mut_obj(rd);
    yyjson_mut_val *rs    = yyjson_mut_obj(rd);
    yyjson_mut_val *re    = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, rs, "line",      ws->range.start.line);
    yyjson_mut_obj_add_uint(rd, rs, "character", ws->range.start.character);
    yyjson_mut_obj_add_uint(rd, re, "line",      ws->range.end.line);
    yyjson_mut_obj_add_uint(rd, re, "character", ws->range.end.character);
    yyjson_mut_obj_add_val (rd, range, "start", rs);
    yyjson_mut_obj_add_val (rd, range, "end",   re);
    yyjson_mut_obj_add_val (rd, loc, "range", range);

    yyjson_mut_obj_add_val(rd, o, "location", loc);
    return o;
}

void ilsp_handle_workspace_symbol(IronLsp_Server *s,
                                    struct yyjson_doc *doc,
                                    Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;
    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");

    const char *query = "";
    if (params) {
        yyjson_val *q = yyjson_obj_get(params, "query");
        const char *qs = yyjson_get_str(q);
        if (qs) query = qs;
    }

    Iron_Arena    body_arena = iron_arena_create(8 * 1024);
    yyjson_alc    alc        = ilsp_json_alc(&body_arena);
    yyjson_mut_doc *rd        = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&body_arena); return; }

    _Atomic bool *cancel = register_cancel(s, id_v);

    Iron_Arena                work_arena = iron_arena_create(64 * 1024);
    IronLsp_WorkspaceSymbol  *syms = NULL;
    size_t                    n    = 0;
    ilsp_facade_nav_workspace_symbol(s, query, cancel, &work_arena,
                                       &syms, &n);

    if (cancel && atomic_load(cancel)) {
        iron_arena_free(&work_arena);
        yyjson_mut_doc_free(rd);
        iron_arena_free(&body_arena);
        return;
    }

    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < n; i++) {
        yyjson_mut_val *o = ws_sym_to_json(rd, &syms[i]);
        yyjson_mut_arr_append(arr, o);
    }
    enqueue_result(s, &body_arena, id_v, rd, arr);

    yyjson_mut_doc_free(rd);
    iron_arena_free(&body_arena);
    iron_arena_free(&work_arena);
}
