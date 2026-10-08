/* Phase 2 Plan 05 Task 02 -- Compilation facade.
 *
 * This file is the SINGLE call site for iron_analyze_buffer from
 * anywhere under src/lsp. Every hover / goto / diagnostic path in the
 * LSP must flow through one of the three entry points below. The
 * CORE-22 parity harness enforces this structurally (grep check).
 *
 *   - ilsp_facade_compile_pure  : analyzer-only; caller owns arena + diags.
 *   - ilsp_facade_compile       : + translate Iron_Diagnostic[] -> LSP
 *                                  Diagnostic[] + enqueue publishDiagnostics.
 *   - ilsp_facade_pull_diagnostic : synchronous pull; builds
 *                                   DocumentDiagnosticReport response
 *                                   and enqueues at ILSP_PRIO_RESPONSE.
 *
 * Arena discipline (HARD-06): the facade owns a per-request 64 KB arena
 * that is created + freed within each call. No arena outlives a single
 * compile. */

#include "lsp/facade/compile.h"
#include "lsp/facade/diagnostics.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/types.h"
#include "lsp/transport/json.h"
#include "analyzer/analyzer.h"
#include "analyzer/stdlib_prepend.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"
#include "vendor/stb_ds.h"

#include "util/pthread_compat.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Internal helper: enqueue a heap-copied JSON body onto the writer. */
static void enqueue_body(IronLsp_Writer *w, IronLsp_Priority prio,
                          const char *arena_body, size_t body_len) {
    if (!w || !arena_body || body_len == 0) return;
    char *heap = (char *)malloc(body_len);
    if (!heap) return;
    memcpy(heap, arena_body, body_len);
    ilsp_writer_enqueue(w, prio, heap, body_len);
    /* Ownership transferred; do not touch `heap` after this. */
}

/* Keep only the diagnostics of the edited document: the stdlib prelude's
 * carry their own file names. (The analyzer orders the program's decls the
 * same way; see Iron_Program.prelude_decl_count.) */
static void keep_document_diags(Iron_DiagList *diags, const char *doc_file) {
    int n = 0, errors = 0, warnings = 0;
    for (int i = 0; i < diags->count; i++) {
        Iron_Diagnostic *dg = &diags->items[i];
        if (dg->span.filename && strcmp(dg->span.filename, doc_file) != 0) continue;
        if (dg->level == IRON_DIAG_ERROR) errors++;
        else if (dg->level == IRON_DIAG_WARNING) warnings++;
        diags->items[n++] = *dg;
    }
    if (diags->items) arrsetlen(diags->items, n);
    diags->count = n;
    diags->error_count = errors;
    diags->warning_count = warnings;
}

/* Where the stdlib is, resolved once per process: every document worker
 * analyzes with it, and a failed lookup should be reported once, not on
 * every keystroke. NULL when there is none; the buffer is then analyzed
 * alone. */
static pthread_once_t g_lib_dir_once = PTHREAD_ONCE_INIT;
static char          *g_lib_dir;

static void resolve_lib_dir(void) {
    g_lib_dir = iron_stdlib_lib_dir();
}

/* Shared analyze primitive -- the SINGLE iron_analyze_buffer call site
 * for the entire src/lsp tree. Both ilsp_facade_compile_pure (discards
 * the program pointer) and ilsp_facade_compile_for_nav (returns it)
 * route through this helper so the CORE-22 grep check stays at 1 hit.
 *
 * The buffer is analyzed with the same stdlib prelude `ironc check` uses
 * (iron_stdlib_prepend), so Map, String methods and imported modules
 * resolve in the editor as they do in the compiler. */
static Iron_Program *facade_analyze(struct IronLsp_Document      *doc,
                                      const IronLsp_CompileRequest *req,
                                      Iron_Arena                   *arena,
                                      Iron_DiagList                *diags) {
    if (!doc || !arena || !diags) return NULL;
    const _Atomic bool *cancel = req ? req->cancel_flag : NULL;
    const char *doc_file = doc->uri ? doc->uri : "<buffer>";

    char *source = NULL;
    int prepended = 0;
    pthread_once(&g_lib_dir_once, resolve_lib_dir);
    if (g_lib_dir) {
        size_t len = doc->text ? doc->text_len : 0;
        source = (char *)malloc(len + 1);
        if (source) {
            if (len) memcpy(source, doc->text, len);
            source[len] = '\0';
            prepended = iron_stdlib_prepend(&source, doc_file, g_lib_dir);
        }
    }

    /* With no stdlib found, the buffer is analyzed alone. */
    bool with_prelude = source && prepended > 0;
    const char *text = with_prelude ? source : (doc->text ? doc->text : "");
    size_t      text_len = with_prelude ? strlen(source) : doc->text_len;
    Iron_AnalyzeResult r = iron_analyze_buffer(
        text, text_len, doc_file, IRON_ANALYSIS_MODE_LSP, arena, diags, cancel,
        with_prelude ? prepended + 1 : 0);
    /* The arena keeps no pointers into the source text (tokens and the AST
     * copy what they keep), so the prelude buffer can go now. */
    free(source);
    keep_document_diags(diags, doc_file);
    return r.program;
}

void ilsp_facade_compile_pure(struct IronLsp_Document      *doc,
                               const IronLsp_CompileRequest *req,
                               Iron_Arena                   *arena,
                               Iron_DiagList                *diags) {
    (void)facade_analyze(doc, req, arena, diags);
}

Iron_Program *ilsp_facade_compile_for_nav(struct IronLsp_Document      *doc,
                                            const IronLsp_CompileRequest *req,
                                            Iron_Arena                   *arena,
                                            Iron_DiagList                *diags) {
    return facade_analyze(doc, req, arena, diags);
}

void ilsp_facade_compile(struct IronLsp_Server   *server,
                          struct IronLsp_Document *doc,
                          const IronLsp_CompileRequest *req) {
    if (!server || !doc) return;

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    ilsp_facade_compile_pure(doc, req, &arena, &diags);

    /* Cancellation check: if the atomic was tripped (HARD-05), drop the
     * payload silently. The client's next edit will schedule a fresh
     * compile. */
    bool cancelled = (req && req->cancel_flag
                       && atomic_load(req->cancel_flag));
    if (cancelled) {
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return;
    }

    /* Build + enqueue textDocument/publishDiagnostics. */
    Iron_Arena build_arena = iron_arena_create(8 * 1024);
    yyjson_mut_doc *mdoc = ilsp_build_publish_diagnostics(
        &diags, doc, server->position_encoding, &build_arena);
    if (mdoc) {
        size_t body_len = 0;
        char *body = ilsp_json_write_mut(mdoc, &build_arena, &body_len);
        enqueue_body(server->writer, ILSP_PRIO_NOTIFICATION, body, body_len);
        yyjson_mut_doc_free(mdoc);
    }

    iron_arena_free(&build_arena);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

void ilsp_facade_pull_diagnostic(struct IronLsp_Server   *server,
                                  struct IronLsp_Document *doc,
                                  const char              *request_id) {
    if (!server || !doc || !request_id) return;

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    /* Quarantined doc: return an empty-items report with the current
     * version's resultId. */
    if (!atomic_load(&doc->quarantined)) {
        IronLsp_CompileRequest req = { .version = doc->version,
                                        .cancel_flag = NULL };
        ilsp_facade_compile_pure(doc, &req, &arena, &diags);
    }

    Iron_Arena build_arena = iron_arena_create(8 * 1024);
    yyjson_mut_doc *mdoc = ilsp_build_pull_diagnostic_report(
        &diags, doc, server->position_encoding, request_id, &build_arena);
    if (mdoc) {
        size_t body_len = 0;
        char *body = ilsp_json_write_mut(mdoc, &build_arena, &body_len);
        enqueue_body(server->writer, ILSP_PRIO_RESPONSE, body, body_len);
        yyjson_mut_doc_free(mdoc);
    }

    iron_arena_free(&build_arena);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}
