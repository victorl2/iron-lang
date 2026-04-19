/* Phase 2 Plan 03 -- Lifecycle + cancel handlers.
 *
 * Implements the five handlers referenced by dispatch.c's compile-time
 * handler table:
 *   - ilsp_handle_initialize   (req)  builds ServerCapabilities, enqueues response
 *   - ilsp_handle_initialized  (notif) fires post-init dynamic registration
 *   - ilsp_handle_shutdown     (req)  enqueues null result; state advances
 *   - ilsp_handle_exit         (notif) calls ilsp_exit_fn(code)
 *   - ilsp_handle_cancel       (notif) flips the registry flag for the id
 *
 * Exit-syscall seam (testability): the file-scope function pointer
 * `ilsp_exit_fn` is the indirection point for the process-terminating
 * exit. Production default is `_exit` (async-signal-safe). Unity tests
 * extern-override it with a capture hook so the test binary doesn't
 * actually terminate when exercising the exit path.
 *
 * Thread discipline: all five handlers run on the main dispatcher
 * thread; no mutation of shared state outside the writer queue (thread-
 * safe) and the cancel registry (thread-safe). */

#include "lsp/server/server.h"
#include "lsp/server/dispatch.h"
#include "lsp/server/lifecycle.h"
#include "lsp/server/capabilities.h"
#include "lsp/server/cancel.h"
#include "lsp/server/dyn_register.h"
#include "lsp/store/workspace.h"          /* Phase 5 Plan 05-02 (D-13): URI->path */
#include "lsp/store/workspace_index.h"   /* Phase 5 Plan 05-02 (D-13): fmt_opts load */
#include "lsp/transport/json.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/types.h"
#include "vendor/yyjson/yyjson.h"
#include "util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* _exit */

/* ── Exit syscall seam ──────────────────────────────────────────────────── */
/* Non-static by design: Unity tests extern-override this to capture the
 * exit code without terminating. Production default is async-signal-safe
 * _exit (flushes nothing; that's fine, the writer thread has already
 * drained by the time exit is invoked in our path). */
void (*ilsp_exit_fn)(int) = _exit;

/* ── Response helpers ───────────────────────────────────────────────────── */

/* Clone the LSP request `id` from the inbound doc into the response doc.
 * id may be integer, string, or null; preserve shape. Returns a mut val
 * owned by `resp_doc` -- caller yyjson_mut_obj_add_val's it. */
static yyjson_mut_val *clone_request_id(yyjson_mut_doc *resp_doc,
                                        yyjson_val *id) {
    if (!id || yyjson_is_null(id)) {
        return yyjson_mut_null(resp_doc);
    }
    if (yyjson_is_int(id) || yyjson_is_sint(id)) {
        return yyjson_mut_sint(resp_doc, yyjson_get_sint(id));
    }
    if (yyjson_is_uint(id)) {
        return yyjson_mut_uint(resp_doc, yyjson_get_uint(id));
    }
    if (yyjson_is_str(id)) {
        return yyjson_mut_strcpy(resp_doc, yyjson_get_str(id));
    }
    return yyjson_mut_null(resp_doc);
}

/* Enqueue a JSON-RPC 2.0 response (result, not error). Takes the result
 * mut-val (already attached to resp_doc) as the payload. */
static void enqueue_response(IronLsp_Server *s,
                             yyjson_mut_doc *resp_doc,
                             Iron_Arena     *arena) {
    size_t len = 0;
    char *body = ilsp_json_write_mut(resp_doc, arena, &len);
    if (!body) return;

    /* The writer takes ownership of a malloc'd buffer. Our body lives in
     * the arena (freed when the arena is freed); copy it to a malloc'd
     * buffer for the writer's ownership discipline. */
    char *heap_body = (char *)malloc(len);
    if (!heap_body) return;
    memcpy(heap_body, body, len);
    ilsp_writer_enqueue(s->writer, ILSP_PRIO_RESPONSE, heap_body, len);
}

/* ── initialize (request) ───────────────────────────────────────────────── */

void ilsp_handle_initialize(IronLsp_Server    *s,
                            struct yyjson_doc *doc,
                            Iron_Arena        *arena) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *id   = yyjson_obj_get(root, "id");
    yyjson_val *params      = yyjson_obj_get(root, "params");
    yyjson_val *client_caps = params ? yyjson_obj_get(params, "capabilities") : NULL;

    /* Negotiate position encoding; stash on server. */
    IronLsp_PositionEncoding enc =
        ilsp_capabilities_negotiate_position_encoding(client_caps);
    s->position_encoding = enc;

    /* Phase 4 Plan 04-03 Task 03: sniff two client capabilities used
     * by the completion orchestrator + WorkspaceEdit emission.
     *   textDocument.completion.completionItem.snippetSupport
     *   workspace.workspaceEdit.documentChanges
     * Both default false; only set when present AND true in the
     * client-supplied JSON. */
    s->client_supports_snippet = false;
    s->client_supports_document_changes = false;
    if (client_caps && yyjson_is_obj(client_caps)) {
        yyjson_val *td = yyjson_obj_get(client_caps, "textDocument");
        if (td && yyjson_is_obj(td)) {
            yyjson_val *comp = yyjson_obj_get(td, "completion");
            if (comp && yyjson_is_obj(comp)) {
                yyjson_val *ci = yyjson_obj_get(comp, "completionItem");
                if (ci && yyjson_is_obj(ci)) {
                    yyjson_val *snip = yyjson_obj_get(ci, "snippetSupport");
                    if (snip && yyjson_is_bool(snip) && yyjson_get_bool(snip)) {
                        s->client_supports_snippet = true;
                    }
                }
            }
        }
        yyjson_val *ws = yyjson_obj_get(client_caps, "workspace");
        if (ws && yyjson_is_obj(ws)) {
            yyjson_val *we = yyjson_obj_get(ws, "workspaceEdit");
            if (we && yyjson_is_obj(we)) {
                yyjson_val *dc = yyjson_obj_get(we, "documentChanges");
                if (dc && yyjson_is_bool(dc) && yyjson_get_bool(dc)) {
                    s->client_supports_document_changes = true;
                }
            }
        }
    }

    /* Phase 5 Plan 05-02 (D-13): resolve workspace root from initialize
     * params (workspaceFolders[0].uri preferred; rootUri fallback) and
     * stand up the workspace index so [fmt] options can be cached for
     * the formatting endpoints. The index is created here even when a
     * prior phase did not populate it; existing handlers (handlers_document
     * iron.toml watcher, nav handlers) defensively guard against NULL,
     * so creating it strictly improves their fidelity.  */
    if (params && !s->workspace_root) {
        const char *root_uri = NULL;
        yyjson_val *folders = yyjson_obj_get(params, "workspaceFolders");
        if (folders && yyjson_is_arr(folders) && yyjson_arr_size(folders) > 0) {
            yyjson_val *first = yyjson_arr_get(folders, 0);
            if (first && yyjson_is_obj(first)) {
                root_uri = yyjson_get_str(yyjson_obj_get(first, "uri"));
            }
        }
        if (!root_uri) {
            root_uri = yyjson_get_str(yyjson_obj_get(params, "rootUri"));
        }
        if (root_uri) {
            char *path = ilsp_workspace_path_from_uri(root_uri);
            if (path) {
                s->workspace_root = path;   /* takes ownership */
                if (!s->workspace_index) {
                    s->workspace_index =
                        ilsp_workspace_index_create(s->workspace_root);
                }
            }
        }
    }
    /* Phase 5 Plan 05-02 (D-13): load [fmt] from workspace iron.toml.
     * No-op when workspace_index is NULL (no rootUri / no workspace
     * folder) -- fmt facade falls back to defaults in that case. */
    if (s->workspace_index) {
        ilsp_workspace_fmt_opts_load(s->workspace_index,
                                      s->workspace_root);
    }

    /* Build response doc: { jsonrpc: "2.0", id, result: { capabilities, serverInfo } }. */
    yyjson_alc      alc = ilsp_json_alc(arena);
    yyjson_mut_doc *rd  = yyjson_mut_doc_new(&alc);
    if (!rd) return;
    yyjson_mut_val *rroot = yyjson_mut_obj(rd);
    yyjson_mut_doc_set_root(rd, rroot);

    yyjson_mut_obj_add_strcpy(rd, rroot, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val  (rd, rroot, "id", clone_request_id(rd, id));

    yyjson_mut_val *result = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_val(rd, rroot, "result", result);

    /* capabilities derived from the compile-time handler registry. */
    yyjson_mut_val *caps = ilsp_capabilities_build(rd, enc);
    yyjson_mut_obj_add_val(rd, result, "capabilities", caps);

    /* serverInfo -- optional but useful for client logs. */
    yyjson_mut_val *info = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_strcpy(rd, info, "name",    "ironls");
    yyjson_mut_obj_add_strcpy(rd, info, "version", "1.2.0-alpha");
    yyjson_mut_obj_add_val   (rd, result, "serverInfo", info);

    enqueue_response(s, rd, arena);
}

/* ── initialized (notification) ─────────────────────────────────────────── */

void ilsp_handle_initialized(IronLsp_Server    *s,
                             struct yyjson_doc *doc,
                             Iron_Arena        *arena) {
    (void)doc;
    (void)arena;

    /* Post-initialized: dynamically register for watched-files notifications.
     * This is fire-and-forget; the client's response (if any) is a
     * plain id-match that we currently ignore. Plan 04 will wire the
     * didChangeWatchedFiles handler to consume the events. */
    if (s && s->dyn_reg) {
        ilsp_dyn_register_watched_files(s);
    }
}

/* ── shutdown (request) ─────────────────────────────────────────────────── */

void ilsp_handle_shutdown(IronLsp_Server    *s,
                          struct yyjson_doc *doc,
                          Iron_Arena        *arena) {
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *id   = yyjson_obj_get(root, "id");

    /* Spec: shutdown result is null. */
    yyjson_alc      alc = ilsp_json_alc(arena);
    yyjson_mut_doc *rd  = yyjson_mut_doc_new(&alc);
    if (!rd) return;
    yyjson_mut_val *rroot = yyjson_mut_obj(rd);
    yyjson_mut_doc_set_root(rd, rroot);

    yyjson_mut_obj_add_strcpy(rd, rroot, "jsonrpc", "2.0");
    yyjson_mut_obj_add_val  (rd, rroot, "id", clone_request_id(rd, id));
    yyjson_mut_obj_add_val  (rd, rroot, "result", yyjson_mut_null(rd));

    enqueue_response(s, rd, arena);
}

/* ── exit (notification) ────────────────────────────────────────────────── */

void ilsp_handle_exit(IronLsp_Server    *s,
                      struct yyjson_doc *doc,
                      Iron_Arena        *arena) {
    (void)doc;
    (void)arena;

    /* Per LSP 3.17 §exit:
     *  - If the server was previously shutdown: exit code 0.
     *  - Otherwise (exit without prior shutdown): exit code 1. */
    int code = (s && s->lifecycle == ILSP_LIFECYCLE_SHUTDOWN) ? 0 : 1;
    ilsp_exit_fn(code);
}

/* ── $/cancelRequest (notification) ─────────────────────────────────────── */

void ilsp_handle_cancel(IronLsp_Server    *s,
                        struct yyjson_doc *doc,
                        Iron_Arena        *arena) {
    (void)arena;
    if (!s || !s->cancels || !doc) return;

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;
    yyjson_val *id     = yyjson_obj_get(params, "id");
    if (!id) return;

    char key[64];
    if (yyjson_is_int(id) || yyjson_is_sint(id)) {
        long long v = (long long)yyjson_get_sint(id);
        snprintf(key, sizeof(key), "%lld", v);
    } else if (yyjson_is_uint(id)) {
        unsigned long long v = (unsigned long long)yyjson_get_uint(id);
        snprintf(key, sizeof(key), "%llu", v);
    } else if (yyjson_is_str(id)) {
        const char *s_id = yyjson_get_str(id);
        if (!s_id) return;
        snprintf(key, sizeof(key), "%s", s_id);
    } else {
        return;   /* malformed -- ignore */
    }

    ilsp_cancel_signal(s->cancels, key);
}
