/* Phase 2 Plan 04 Task 02 -- Document-sync notification handlers.
 *
 * Five handlers feed into dispatch.c's ilsp_handler_table, each keyed by
 * LSP method name:
 *   - textDocument/didOpen        notification
 *   - textDocument/didChange      notification
 *   - textDocument/didClose       notification
 *   - textDocument/didSave        notification
 *   - workspace/didChangeWatchedFiles  notification
 *
 * All five manipulate server->documents (stb_ds sh_new_strdup map: URI
 * string key -> IronLsp_Document *value). The map is lazy-initialized
 * on the first didOpen via sh_new_strdup. Plan 05 will extend each
 * document with a per-doc mailbox + worker thread; Plan 04 keeps the
 * data side self-contained.
 *
 * Handler signature (see dispatch.h):
 *   void handler(IronLsp_Server *s, yyjson_doc *doc, Iron_Arena *arena);
 *
 * The per-request arena is owned by the dispatcher; our handlers
 * consume the parsed JSON via yyjson_doc_get_root + yyjson_obj_get. No
 * response is enqueued (all five methods are notifications, not
 * requests). On parse failure we log to stderr and drop the event -- no
 * error response (per JSON-RPC 2.0 §5).
 *
 * Thread discipline: all five handlers run on the main dispatcher
 * thread. The documents map is mutated only from here. */

#include "lsp/server/server.h"
#include "lsp/server/dispatch.h"
#include "lsp/server/cancel.h"
#include "lsp/server/handlers_workspace_diag.h"  /* Plan 06: refresh emitter */
#include "lsp/store/document.h"
#include "lsp/store/workspace.h"
#include "lsp/store/workspace_index.h" /* Phase 3 Plan 02: invalidation */
#include "lsp/store/sha256.h"
#include "lsp/facade/types.h"
#include "lsp/facade/workspace_diagnostic.h"      /* Plan 06: cache evict */
#include "lsp/workers/ast_worker.h"    /* Plan 05: start/shutdown worker */
#include "lsp/workers/mailbox.h"       /* Plan 05: post COMPILE / PULL */
#include "lsp/facade/compile.h"         /* empty pull reports */

#include "vendor/yyjson/yyjson.h"
#include "vendor/stb_ds.h"
#include "util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations -- referenced from dispatch.c handler table. */
void ilsp_handle_didOpen                 (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didChange               (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didClose                (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didSave                 (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didChangeWatchedFiles   (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);
/* Plan 05: pull diagnostic handler. */
void ilsp_handle_text_document_diagnostic(IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);

/* ── Plan 05 helpers ─────────────────────────────────────────────── */

/* Build a stringified request-id key from a yyjson id value. Caller
 * owns the returned malloc'd string (or NULL). */
static char *stringify_request_id(yyjson_val *id) {
    if (!id || yyjson_is_null(id)) return NULL;
    if (yyjson_is_int(id) || yyjson_is_sint(id)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld",
                  (long long)yyjson_get_sint(id));
        return strdup(buf);
    }
    if (yyjson_is_uint(id)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%llu",
                  (unsigned long long)yyjson_get_uint(id));
        return strdup(buf);
    }
    if (yyjson_is_str(id)) return strdup(yyjson_get_str(id));
    return NULL;
}

/* ── Helpers ───────────────────────────────────────────────────────── */

/* A pull diagnostic request still waiting in a document's mailbox when the
 * document closes (or is reopened) gets an empty report: every request is
 * answered. */
static void answer_pending_pull(IronLsp_Server *s, struct IronLsp_Mailbox *m) {
    char *id = ilsp_mailbox_take_pull(m);
    if (id) {
        ilsp_facade_pull_diagnostic_empty(s, id);
        free(id);
    }
}

/* Lazily initialize the documents map on first use. Idempotent. */
static void ensure_documents_map(IronLsp_Server *s) {
    if (!s) return;
    if (s->documents == NULL) {
        sh_new_strdup(s->documents);
    }
}

/* Parse an LSP Range from a JSON object:
 *   { "start": { "line": N, "character": N }, "end": { ... } }
 * Returns true on success; false on malformed input. */
static bool parse_range(yyjson_val *range_v, IronLsp_Range *out) {
    if (!range_v || !yyjson_is_obj(range_v)) return false;
    yyjson_val *start_v = yyjson_obj_get(range_v, "start");
    yyjson_val *end_v   = yyjson_obj_get(range_v, "end");
    if (!start_v || !end_v) return false;

    yyjson_val *sl = yyjson_obj_get(start_v, "line");
    yyjson_val *sc = yyjson_obj_get(start_v, "character");
    yyjson_val *el = yyjson_obj_get(end_v,   "line");
    yyjson_val *ec = yyjson_obj_get(end_v,   "character");
    if (!sl || !sc || !el || !ec) return false;

    out->start.line      = (uint32_t)yyjson_get_uint(sl);
    out->start.character = (uint32_t)yyjson_get_uint(sc);
    out->end.line        = (uint32_t)yyjson_get_uint(el);
    out->end.character   = (uint32_t)yyjson_get_uint(ec);
    return true;
}

/* Compare a client-supplied hex hash against a digest. Returns true iff
 * they match (case-insensitive). hint_hex must be 64 chars. */
static bool sha256_matches_hint(const uint8_t digest[32], const char *hint_hex) {
    char got[65];
    ilsp_sha256_hex(digest, got);
    for (int i = 0; i < 64; i++) {
        char a = got[i];
        char b = hint_hex[i];
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return hint_hex[64] == '\0' || hint_hex[64] == '\n' || hint_hex[64] == '\r';
}

/* ── textDocument/didOpen ──────────────────────────────────────────── */

void ilsp_handle_didOpen(IronLsp_Server *s, struct yyjson_doc *doc,
                          Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;
    yyjson_val *td     = yyjson_obj_get(params, "textDocument");
    if (!td) return;

    const char *uri     = yyjson_get_str(yyjson_obj_get(td, "uri"));
    const char *text    = yyjson_get_str(yyjson_obj_get(td, "text"));
    int32_t     version = (int32_t)yyjson_get_int(yyjson_obj_get(td, "version"));
    if (!uri || !text) return;

    ensure_documents_map(s);

    /* Idempotent: a duplicate didOpen replaces the existing document.
     * Plan 05: stop any existing ASTWorker before destroying the doc. */
    ptrdiff_t idx = shgeti(s->documents, uri);
    if (idx >= 0) {
        IronLsp_Document *prev = s->documents[idx].value;
        ilsp_ast_worker_shutdown_and_join(prev);
        if (prev->mailbox) {
            answer_pending_pull(s, prev->mailbox);
            ilsp_mailbox_destroy(prev->mailbox);
            prev->mailbox = NULL;
        }
        ilsp_document_destroy(prev);
        shdel(s->documents, uri);
    }

    IronLsp_Document *nd = ilsp_document_create(uri, text, strlen(text), version);
    if (!nd) return;
    shput(s->documents, uri, nd);
    if (s->workspace_index) {
        char *path = ilsp_workspace_path_from_uri(uri);
        if (path) {
            ilsp_workspace_index_update_buffer(s->workspace_index, path,
                                                nd->text, nd->text_len);
            free(path);
        }
    }

    /* Plan 05 CORE-14: start the per-doc ASTWorker and schedule the
     * initial compile for this document. Failure (e.g. pthread_create
     * returned EAGAIN) is non-fatal: we log and leave the doc around
     * so subsequent edits still update the buffer; the client just
     * won't see diagnostics. */
    if (!ilsp_ast_worker_start(s, nd)) {
        fprintf(stderr,
                "ironls: failed to start ASTWorker for %s -- diagnostics disabled\n",
                uri);
        return;
    }
    ilsp_mailbox_post_compile(nd->mailbox, nd->version, NULL);
}

/* ── textDocument/didChange ────────────────────────────────────────── */

void ilsp_handle_didChange(IronLsp_Server *s, struct yyjson_doc *doc,
                            Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;
    ensure_documents_map(s);

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;
    yyjson_val *td      = yyjson_obj_get(params, "textDocument");
    yyjson_val *changes = yyjson_obj_get(params, "contentChanges");
    if (!td || !changes || !yyjson_is_arr(changes)) return;

    const char *uri = yyjson_get_str(yyjson_obj_get(td, "uri"));
    int32_t new_ver = (int32_t)yyjson_get_int(yyjson_obj_get(td, "version"));
    if (!uri) return;

    ptrdiff_t idx = shgeti(s->documents, uri);
    if (idx < 0) {
        fprintf(stderr,
                "ironls: didChange for unknown URI %s -- dropping\n", uri);
        return;
    }
    IronLsp_Document *d = s->documents[idx].value;

    IronLsp_PositionEncoding enc = s->position_encoding;

    /* Apply each TextDocumentContentChangeEvent in order. Each event's
     * `range` (optional) decides full-replace vs. incremental. The events
     * of one notification together make version `new_ver`: the document
     * apply functions insist on a rising version per edit, so the events
     * step through base+1, base+2, ... and the document then takes the
     * client's version. (It used to keep the last step's number, so
     * after a notification with several events every later diagnostic
     * carried a version the client never sent.) */
    size_t count = yyjson_arr_size(changes);
    bool applied = false;
    /* The worker snapshots under this lock: it sees the text before or
     * after the whole notification, with the matching version. */
    ilsp_document_lock(d);
    for (size_t i = 0; i < count; i++) {
        yyjson_val *ch    = yyjson_arr_get(changes, i);
        yyjson_val *range = yyjson_obj_get(ch, "range");
        yyjson_val *txt   = yyjson_obj_get(ch, "text");
        const char *ntxt  = yyjson_get_str(txt);
        size_t      nlen  = ntxt ? strlen(ntxt) : 0;
        int32_t step_ver  = d->version + 1;

        if (range) {
            IronLsp_Range r = {0};
            if (!parse_range(range, &r)) continue;
            if (!ilsp_document_apply_incremental(d, r, ntxt, nlen, enc,
                                                  step_ver)) {
                /* Edit rejected; stop applying further changes in this
                 * batch to preserve invariants. */
                break;
            }
        } else {
            if (!ilsp_document_apply_full_replace(d, ntxt, nlen, step_ver)) {
                break;
            }
        }
        applied = true;
    }
    if (applied) d->version = new_ver;
    ilsp_document_unlock(d);

    if (applied && s->workspace_index) {
        char *path = ilsp_workspace_path_from_uri(d->uri);
        if (path) {
            ilsp_workspace_index_update_buffer(s->workspace_index, path,
                                                d->text, d->text_len);
            free(path);
        }
    }

    /* Drift detection: if the client included a non-standard
     * "contentHash" string, compare against our SHA-256. Log WARN on
     * mismatch but do not roll back (the client is trusted to resolve
     * via didClose+didOpen). */
    yyjson_val *hint = yyjson_obj_get(params, "contentHash");
    const char *hex  = yyjson_get_str(hint);
    if (hex && strlen(hex) >= 64) {
        if (!sha256_matches_hint(d->sha256, hex)) {
            fprintf(stderr,
                    "ironls: content-hash drift on %s (client hint != "
                    "server hash) -- client should reload\n", uri);
        }
    }

    /* Plan 05 CORE-14/CORE-15: post a COMPILE to the ASTWorker. Worker
     * debounces 150ms + coalesces rapid didChange floods into a single
     * compile of the newest version. (A per-version cancel flag was
     * registered here, never signalled and never released: one allocation
     * leaked per edit.) */
    if (d->mailbox && d->worker_started) {
        ilsp_mailbox_post_compile(d->mailbox, d->version, NULL);
    }
}

/* ── textDocument/didClose ─────────────────────────────────────────── */

void ilsp_handle_didClose(IronLsp_Server *s, struct yyjson_doc *doc,
                           Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;
    yyjson_val *td     = yyjson_obj_get(params, "textDocument");
    if (!td) return;
    const char *uri = yyjson_get_str(yyjson_obj_get(td, "uri"));
    if (!uri) return;

    if (!s->documents) return;
    ptrdiff_t idx = shgeti(s->documents, uri);
    if (idx < 0) return;   /* idempotent: closing unknown URI is a no-op. */
    IronLsp_Document *d = s->documents[idx].value;

    /* Plan 05: stop the ASTWorker before freeing document memory. The
     * worker may still be mid-compile; ilsp_ast_worker_shutdown_and_join
     * posts SHUTDOWN to the mailbox and pthread_join's the worker
     * thread. After join, it's safe to destroy the mailbox and the
     * document body. */
    ilsp_ast_worker_shutdown_and_join(d);
    if (d->mailbox) {
        answer_pending_pull(s, d->mailbox);
        ilsp_mailbox_destroy(d->mailbox);
        d->mailbox = NULL;
    }
    /* The worker is stopped: clear what the closed file showed, after
     * anything it published. */
    ilsp_facade_publish_cleared(s, d->uri);
    if (s->workspace_index) {
        char *path = ilsp_workspace_path_from_uri(d->uri);
        if (path) {
            ilsp_workspace_index_refresh_path(s->workspace_index, path);
            free(path);
        }
    }
    ilsp_document_destroy(d);
    shdel(s->documents, uri);
}

/* A source file changed on disk. The open documents of its package are
 * analyzed together with it (the saved contents of the package's other
 * files), so their diagnostics may have changed: analyze them again, and
 * ask a client that pulls diagnostics to pull again. */
static void reanalyze_open_documents(IronLsp_Server *s, const char *changed_uri) {
    if (!s || !s->documents) return;
    bool any = false;
    for (ptrdiff_t i = 0; i < shlen(s->documents); i++) {
        IronLsp_Document *d = s->documents[i].value;
        if (!d || !d->uri) continue;
        if (changed_uri && strcmp(d->uri, changed_uri) == 0) continue;
        any = true;
        if (d->mailbox && d->worker_started) {
            ilsp_mailbox_post_compile(d->mailbox, d->version, NULL);
        }
    }
    if (any && s->client_pulls_diagnostics) ilsp_send_workspace_diagnostic_refresh(s);
}

/* ── textDocument/didSave ──────────────────────────────────────────── */

void ilsp_handle_didSave(IronLsp_Server *s, struct yyjson_doc *doc,
                          Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;
    yyjson_val *td     = yyjson_obj_get(params, "textDocument");
    if (!td) return;
    const char *uri = yyjson_get_str(yyjson_obj_get(td, "uri"));
    if (!uri) return;

    /* If the save notification carries text (the client negotiated
     * includeText: true at registration), treat it as a full-replace to
     * sync server buffer with on-disk content. Otherwise this is a
     * signal-only event and we just log. */
    yyjson_val *text_v = yyjson_obj_get(params, "text");
    const char *ntxt   = yyjson_get_str(text_v);
    if (ntxt && s->documents) {
        ptrdiff_t idx = shgeti(s->documents, uri);
        if (idx >= 0) {
            IronLsp_Document *d = s->documents[idx].value;
            /* didSave doesn't carry a version: the text is the client's
             * current version, which the document keeps (the apply
             * function wants a rising number, so step and restore). */
            ilsp_document_lock(d);
            int32_t keep = d->version;
            if (ilsp_document_apply_full_replace(d, ntxt, strlen(ntxt),
                                                 keep + 1))
                d->version = keep;
            ilsp_document_unlock(d);
        }
    }
    /* else: signal-only save -- no state change needed. */
    reanalyze_open_documents(s, uri);
}

/* ── workspace/didChangeWatchedFiles ───────────────────────────────── */

void ilsp_handle_didChangeWatchedFiles(IronLsp_Server *s,
                                        struct yyjson_doc *doc,
                                        Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;
    yyjson_val *changes = yyjson_obj_get(params, "changes");
    if (!changes || !yyjson_is_arr(changes)) return;

    size_t n = yyjson_arr_size(changes);
    for (size_t i = 0; i < n; i++) {
        yyjson_val *ev  = yyjson_arr_get(changes, i);
        const char *uri = yyjson_get_str(yyjson_obj_get(ev, "uri"));
        if (!uri) continue;

        IronLsp_WatchedKind kind = ilsp_workspace_classify(uri);

        /* If it's an open doc, note staleness. */
        bool open_doc = false;
        if (s->documents) {
            if (shgeti(s->documents, uri) >= 0) open_doc = true;
        }

        switch (kind) {
            case ILSP_WATCHED_SOURCE: {
                /* Phase 3 Plan 02: invalidate the matching workspace-index
                 * entry so the next nav request reparses the fresh bytes. */
                if (s->workspace_index) {
                    char *canon = ilsp_workspace_path_from_uri(uri);
                    const char *evict_key = canon ? canon : uri;
                    ilsp_workspace_index_invalidate_path(
                        s->workspace_index, evict_key);
                    /* Plan 06 (NAV-12, D-12): drop the workspace/diagnostic
                     * cache slot so the next pull re-analyzes. */
                    if (s->ws_diag_cache) {
                        ilsp_ws_diag_cache_evict_path(
                            s->ws_diag_cache, evict_key);
                    }
                    if (canon) free(canon);
                }
                reanalyze_open_documents(s, uri);
                if (open_doc) {
                    fprintf(stderr,
                            "ironls: watched-files source stale-on-disk %s\n",
                            uri);
                } else {
                    fprintf(stderr,
                            "ironls: did-change-watched source invalidated %s\n",
                            uri);
                    /* Plan 06 (NAV-13, D-12): emit workspace/diagnostic/refresh
                     * when a NON-OPEN file is invalidated. Open docs already
                     * get publishDiagnostics push; only non-open files need
                     * the workspace-pull re-trigger. */
                    ilsp_send_workspace_diagnostic_refresh(s);
                }
                break; }
            case ILSP_WATCHED_MANIFEST:
                /* Phase 3 Plan 02: invalidate ALL user-workspace entries. */
                if (s->workspace_index) {
                    ilsp_workspace_index_invalidate_dep(
                        s->workspace_index, NULL);
                    /* Phase 5 Plan 05-02 (D-13): iron.toml change ->
                     * reload cached [fmt] options. Subsequent
                     * textDocument/formatting picks up the new options. */
                    ilsp_workspace_fmt_opts_reload(s->workspace_index);
                }
                fprintf(stderr,
                        "ironls: workspace-reindex-pending (iron.toml) %s\n",
                        uri);
                /* Manifest rewrite invalidates every file's diagnostics --
                 * push refresh so the client re-pulls the whole workspace. */
                ilsp_send_workspace_diagnostic_refresh(s);
                break;
            case ILSP_WATCHED_UNKNOWN:
                /* Silent no-op. */
                break;
        }
    }
}

/* ── textDocument/diagnostic (pull) ──────────────────────────────────
 * Plan 05 CORE-17. Synchronous pull: routes a PULL_DIAGNOSTIC message
 * to the doc's ASTWorker with the request id. The worker runs the
 * facade pull path, which builds a DocumentDiagnosticReport and
 * enqueues it at ILSP_PRIO_RESPONSE bound to the original id. If the
 * document is unknown, emit an empty report so the client isn't left
 * hanging. */

void ilsp_handle_text_document_diagnostic(IronLsp_Server *s,
                                           struct yyjson_doc *doc,
                                           Iron_Arena *arena) {
    (void)arena;
    if (!s || !doc) return;

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");
    if (!params) return;
    yyjson_val *td = yyjson_obj_get(params, "textDocument");
    if (!td) return;
    const char *uri = yyjson_get_str(yyjson_obj_get(td, "uri"));
    if (!uri) return;

    char *request_id = stringify_request_id(id_v);

    if (!s->documents) goto no_doc;
    ptrdiff_t idx = shgeti(s->documents, uri);
    if (idx < 0) goto no_doc;

    IronLsp_Document *d = s->documents[idx].value;
    if (!d->mailbox || !d->worker_started) goto no_doc;

    /* Post PULL_DIAGNOSTIC to the worker; the worker handles response
     * enqueue via ilsp_facade_pull_diagnostic. The mailbox duplicates
     * the request_id string internally; we free our copy after. */
    char *displaced = ilsp_mailbox_post_pull(d->mailbox, d->version,
                                             request_id ? request_id : "");
    if (displaced) {
        /* A newer pull for the same document replaced it before the
         * worker got to it: the older request still needs a response. */
        ilsp_facade_pull_diagnostic_empty(s, displaced);
        free(displaced);
    }
    free(request_id);
    return;

no_doc:
    /* Unknown document: reply with an empty full report so the client
     * doesn't wait for a response that never comes. */
    ilsp_facade_pull_diagnostic_empty(s, request_id);
    free(request_id);
}
