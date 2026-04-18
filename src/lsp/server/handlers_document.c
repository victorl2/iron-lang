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
#include "lsp/store/document.h"
#include "lsp/store/workspace.h"
#include "lsp/store/workspace_index.h" /* Phase 3 Plan 02: invalidation */
#include "lsp/store/sha256.h"
#include "lsp/facade/types.h"
#include "lsp/workers/ast_worker.h"    /* Plan 05: start/shutdown worker */
#include "lsp/workers/mailbox.h"       /* Plan 05: post COMPILE / PULL */

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
            ilsp_mailbox_destroy(prev->mailbox);
            prev->mailbox = NULL;
        }
        ilsp_document_destroy(prev);
        shdel(s->documents, uri);
    }

    IronLsp_Document *nd = ilsp_document_create(uri, text, strlen(text), version);
    if (!nd) return;
    shput(s->documents, uri, nd);

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
     * `range` (optional) decides full-replace vs. incremental. The
     * first event with a present range bumps to new_ver; subsequent
     * events in the same didChange batch bump by one each so the
     * monotonicity guard doesn't reject mid-batch edits. */
    size_t count = yyjson_arr_size(changes);
    int32_t working_ver = d->version;
    for (size_t i = 0; i < count; i++) {
        yyjson_val *ch    = yyjson_arr_get(changes, i);
        yyjson_val *range = yyjson_obj_get(ch, "range");
        yyjson_val *txt   = yyjson_obj_get(ch, "text");
        const char *ntxt  = yyjson_get_str(txt);
        size_t      nlen  = ntxt ? strlen(ntxt) : 0;

        /* The final event targets new_ver; intermediate events step by 1. */
        int32_t step_ver = (i + 1 == count)
                           ? new_ver
                           : (working_ver + 1);
        if (step_ver <= working_ver) step_ver = working_ver + 1;

        if (range) {
            IronLsp_Range r = {0};
            if (!parse_range(range, &r)) continue;
            if (!ilsp_document_apply_incremental(d, r, ntxt, nlen, enc,
                                                  step_ver)) {
                /* Edit rejected; stop applying further changes in this
                 * batch to preserve invariants. */
                return;
            }
        } else {
            if (!ilsp_document_apply_full_replace(d, ntxt, nlen, step_ver)) {
                return;
            }
        }
        working_ver = d->version;
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
     * debounces 250ms + coalesces rapid didChange floods into a single
     * compile of the newest version. Register a per-version cancel flag
     * so a subsequent edit can race-cancel the in-flight analyze. The
     * cancel-flag key is "<uri>#v<version>" -- unique per edit, so
     * cancel_signal can flip the right flag without cross-doc confusion. */
    if (d->mailbox && d->worker_started) {
        char cancel_key[1024];
        snprintf(cancel_key, sizeof(cancel_key),
                  "%s#v%d", uri, d->version);
        _Atomic bool *flag = s->cancels
                               ? ilsp_cancel_register(s->cancels, cancel_key)
                               : NULL;
        ilsp_mailbox_post_compile(d->mailbox, d->version, flag);
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
        ilsp_mailbox_destroy(d->mailbox);
        d->mailbox = NULL;
    }
    ilsp_document_destroy(d);
    shdel(s->documents, uri);
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
            /* didSave doesn't carry a version; bump by 1 to satisfy the
             * monotonicity guard. */
            (void)ilsp_document_apply_full_replace(d, ntxt, strlen(ntxt),
                                                    d->version + 1);
        }
    }
    /* else: signal-only save -- no state change needed. */
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
                    ilsp_workspace_index_invalidate_path(
                        s->workspace_index, canon ? canon : uri);
                    if (canon) free(canon);
                }
                if (open_doc) {
                    fprintf(stderr,
                            "ironls: watched-files source stale-on-disk %s\n",
                            uri);
                } else {
                    fprintf(stderr,
                            "ironls: did-change-watched source invalidated %s\n",
                            uri);
                }
                break; }
            case ILSP_WATCHED_MANIFEST:
                /* Phase 3 Plan 02: invalidate ALL user-workspace entries +
                 * drop the dep map (cascades to dep_map in Plan 02-03). */
                if (s->workspace_index) {
                    ilsp_workspace_index_invalidate_dep(
                        s->workspace_index, NULL);
                }
                fprintf(stderr,
                        "ironls: workspace-reindex-pending (iron.toml) %s\n",
                        uri);
                break;
            case ILSP_WATCHED_LOCKFILE:
                if (s->workspace_index) {
                    ilsp_workspace_index_invalidate_dep(
                        s->workspace_index, NULL);
                }
                fprintf(stderr,
                        "ironls: workspace-reindex-pending (iron.lock) %s\n",
                        uri);
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
    ilsp_mailbox_post_pull(d->mailbox, d->version,
                            request_id ? request_id : "");
    free(request_id);
    return;

no_doc:
    /* Unknown document: reply with an empty full report so the client
     * doesn't time out waiting for a response. */
    free(request_id);
    /* Note: actually emitting the response here would require
     * duplicating the facade's response-building logic. For Plan 05,
     * log and let the client's timeout handle it. A follow-up can
     * synthesize an empty DocumentDiagnosticReport. */
    fprintf(stderr,
            "ironls: textDocument/diagnostic for unknown URI %s\n", uri);
}
