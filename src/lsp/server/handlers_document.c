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
#include "lsp/store/document.h"
#include "lsp/store/workspace.h"
#include "lsp/store/sha256.h"
#include "lsp/facade/types.h"

#include "vendor/yyjson/yyjson.h"
#include "vendor/stb_ds.h"
#include "util/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations -- referenced from dispatch.c handler table. */
void ilsp_handle_didOpen               (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didChange             (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didClose              (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didSave               (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);
void ilsp_handle_didChangeWatchedFiles (IronLsp_Server *s, struct yyjson_doc *doc, Iron_Arena *arena);

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

    /* Idempotent: a duplicate didOpen replaces the existing document. */
    ptrdiff_t idx = shgeti(s->documents, uri);
    if (idx >= 0) {
        IronLsp_Document *prev = s->documents[idx].value;
        ilsp_document_destroy(prev);
        shdel(s->documents, uri);
    }

    IronLsp_Document *nd = ilsp_document_create(uri, text, strlen(text), version);
    if (!nd) return;
    shput(s->documents, uri, nd);
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
            case ILSP_WATCHED_SOURCE:
                if (open_doc) {
                    fprintf(stderr,
                            "ironls: watched-files source stale-on-disk %s\n",
                            uri);
                } else {
                    fprintf(stderr,
                            "ironls: did-change-watched noop (source not open) %s\n",
                            uri);
                }
                break;
            case ILSP_WATCHED_MANIFEST:
                fprintf(stderr,
                        "ironls: workspace-reindex-pending (iron.toml) %s\n",
                        uri);
                break;
            case ILSP_WATCHED_LOCKFILE:
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
