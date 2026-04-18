/* iron-lsp entry point -- Phase 2 Plan 06 Task 02 (CORE-19, CORE-21).
 *
 * Full wiring: argv -> signals -> log -> server singleton -> reader +
 * writer threads -> main-thread reader.join() -> clean shutdown.
 *
 * Startup ordering (important -- see RESEARCH.md §Process Lifetime):
 *   1. argv parse. `--version` returns immediately (unit tests rely on
 *      this). `--log-dir=<p>` + `--log-level=<L>` capture into locals
 *      used once the log sink is open.
 *   2. signal(SIGPIPE, SIG_IGN) BEFORE any I/O so write(2) can return
 *      EPIPE instead of killing us (pattern reused from
 *      src/runtime/iron_net_init.c:95-108).
 *   3. ilsp_install_abort_handler() -- Plan 05's SIGABRT boundary. Runs
 *      before we spawn worker threads so the sigaction is inherited by
 *      every pthread_create child.
 *   4. ilsp_log_open() -- XDG-resolved JSON-line sink; --log-dir wins.
 *      Log the startup banner so operators can trace process starts.
 *   5. IronLsp_Server singleton field initialization. The struct body
 *      is defined in lsp/server/server.h (Plan 03 Task 03); this TU
 *      does NOT re-declare it (acceptance invariant).
 *   6. ilsp_writer_start() spawns the writer thread.
 *   7. ilsp_reader_start() spawns the reader thread and binds
 *      on_message() as the per-frame dispatch callback.
 *   8. Main thread blocks in ilsp_reader_join() until stdin EOF or
 *      explicit reader shutdown (e.g. parent editor quit / exit-queued).
 *
 * Teardown ordering (must match steps 5-7 in reverse):
 *   a. Iterate server.documents and ilsp_ast_worker_shutdown_and_join
 *      each -- joins the per-doc worker thread + frees its mailbox.
 *   b. ilsp_document_destroy + shdel for each map entry.
 *   c. shfree the documents map itself.
 *   d. ilsp_writer_shutdown + ilsp_writer_join + ilsp_writer_destroy.
 *   e. ilsp_reader_destroy (already joined in step 8).
 *   f. Destroy cancels + dyn_reg.
 *   g. ilsp_trace_dump to stderr (operator visibility of timings).
 *   h. Log + close the log sink.
 *   i. Exit code 0 if lifecycle reached EXIT_QUEUED (client drove
 *      shutdown + exit), else 1 (editor crashed or we died early).
 *
 * Singleton discipline: `g_server` is mutated ONLY before the reader
 * thread starts (step 5) and AFTER the reader thread joins (step a).
 * Between those two points it is const-after-init from main.c's
 * perspective; handler code mutates state via their respective
 * subsystem mutexes (cancels lock, mailbox lock, log lock, etc.). This
 * is the documented CLAUDE.md "no static mutable state" exception; no
 * other TU references g_server directly. */

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util/arena.h"
#include "lsp/server/server.h"     /* IronLsp_Server body -- DO NOT re-declare. */
#include "lsp/server/dispatch.h"
#include "lsp/server/lifecycle.h"
#include "lsp/server/cancel.h"
#include "lsp/server/dyn_register.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h" /* Phase 3 Plan 02: workspace index teardown */
#include "lsp/facade/workspace_diagnostic.h" /* Phase 3 Plan 06: ws_diag cache */
#include "lsp/workers/ast_worker.h"
#include "lsp/transport/reader.h"
#include "lsp/transport/writer.h"
#include "lsp/facade/types.h"
#include "lsp/obs/log.h"           /* Plan 06 Task 01: JSON-line log sink. */
#include "lsp/obs/trace.h"         /* Plan 06 Task 01: shutdown histogram. */
#include "lsp/obs/abort_handler.h" /* Plan 05: SIGABRT boundary. */

#include "vendor/stb_ds.h"

#ifndef IRON_VERSION_STRING
#define IRON_VERSION_STRING "0.0.0"
#endif
#ifndef IRON_GIT_HASH
#define IRON_GIT_HASH "unknown"
#endif
#ifndef IRON_BUILD_DATE
#define IRON_BUILD_DATE "unknown"
#endif
#ifndef IRON_BINARY_NAME
#define IRON_BINARY_NAME "ironls"
#endif

/* dyn_register symbols ship without a public .h (Plan 03 leaves them
 * extern-only). Forward-declare matches src/lsp/server/dyn_register.c. */
extern IronLsp_DynRegister *ilsp_dyn_register_create(void);
extern void                 ilsp_dyn_register_destroy(IronLsp_DynRegister *r);

/* Singleton -- see file header for mutation discipline. */
static IronLsp_Server g_server = {0};

/* Reader callback. Invoked on the reader thread for each COMPLETE frame.
 * Allocates a per-request arena, routes via ilsp_dispatch_route, frees
 * the arena. This is the Plan 05 HARD-06 per-request-arena discipline
 * propagated from the facade up to the dispatcher entry. */
static void on_message(void *ctx, const char *body, size_t len) {
    IronLsp_Server *s = (IronLsp_Server *)ctx;
    IronLsp_TraceToken tok = ilsp_trace_begin("dispatch-frame");
    Iron_Arena arena = iron_arena_create(32 * 1024);
    ilsp_dispatch_route(s, body, len, &arena);
    iron_arena_free(&arena);
    ilsp_trace_end(tok);
}

/* Parse --log-level=<L> values. Unknown strings are silently ignored so
 * invocations keep the process alive rather than failing at startup. */
static IronLsp_LogLevel parse_log_level_arg(const char *s,
                                            IronLsp_LogLevel fallback) {
    if (!s || !*s) return fallback;
    if (strcmp(s, "ERROR") == 0 || strcmp(s, "error") == 0) return ILSP_LOG_ERROR;
    if (strcmp(s, "WARN")  == 0 || strcmp(s, "warn")  == 0) return ILSP_LOG_WARN;
    if (strcmp(s, "INFO")  == 0 || strcmp(s, "info")  == 0) return ILSP_LOG_INFO;
    if (strcmp(s, "DEBUG") == 0 || strcmp(s, "debug") == 0) return ILSP_LOG_DEBUG;
    return fallback;
}

int main(int argc, char **argv) {
    /* ── 1. argv parse ───────────────────────────────────────────────── */
    const char      *override_log_dir = NULL;
    bool             have_level       = false;
    IronLsp_LogLevel level_arg        = ILSP_LOG_WARN;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 ||
            strcmp(argv[i], "-v")        == 0) {
            printf("%s %s (%s, %s)\n",
                   IRON_BINARY_NAME,
                   IRON_VERSION_STRING,
                   IRON_GIT_HASH,
                   IRON_BUILD_DATE);
            return 0;
        }
        if (strncmp(argv[i], "--log-dir=", 10) == 0) {
            override_log_dir = argv[i] + 10;
            continue;
        }
        if (strncmp(argv[i], "--log-level=", 12) == 0) {
            level_arg  = parse_log_level_arg(argv[i] + 12, ILSP_LOG_WARN);
            have_level = true;
            continue;
        }
        /* Unknown flag -- ignore; do NOT error (editors may pass harmless
         * flags in future). */
    }

    /* ── 2. SIGPIPE SIG_IGN ─────────────────────────────────────────────
     * Follow the pattern documented in src/runtime/iron_net_init.c:
     * signal() is sufficient here because we only install SIG_IGN (no
     * handler state) and the effect is idempotent. MUST happen before
     * ANY I/O call so the first write to a broken pipe returns EPIPE
     * instead of terminating the process. */
    signal(SIGPIPE, SIG_IGN);

    /* ── 3. SIGABRT boundary (Plan 05) ──────────────────────────────────
     * Install BEFORE spawning worker threads so the sigaction is
     * inherited. The handler siglongjmp's into the per-doc jmp_buf via
     * the _Thread_local ilsp_current_doc_tls; outside a compile it
     * _exit(134)s. */
    ilsp_install_abort_handler();

    /* ── 4. Log sink ────────────────────────────────────────────────────
     * Open BEFORE starting threads so every subsequent log() call
     * (including the startup banner) lands in the file. */
    if (ilsp_log_open(override_log_dir) != 0) {
        fprintf(stderr, "ironls: could not open log sink; continuing without file log\n");
    }
    if (have_level) ilsp_log_set_level(level_arg);
    ilsp_log(ILSP_LOG_INFO, "startup",
             "ironls %s (%s, %s) pid=%d",
             IRON_VERSION_STRING, IRON_GIT_HASH, IRON_BUILD_DATE,
             (int)getpid());

    /* ── 5. Server singleton init ───────────────────────────────────── */
    g_server.lifecycle         = ILSP_LIFECYCLE_UNINIT;
    g_server.writer            = ilsp_writer_create(stdout);
    g_server.reader            = NULL;   /* populated below */
    g_server.cancels           = ilsp_cancel_registry_create();
    g_server.dyn_reg           = ilsp_dyn_register_create();
    g_server.position_encoding = ILSP_ENC_UTF16;   /* re-negotiated by initialize */
    g_server.documents         = NULL;              /* lazy sh_new_strdup on didOpen */
    g_server.workspace_root    = NULL;
    g_server.workers           = NULL;
    g_server.workspace_index   = NULL;              /* created in `initialize` handler */
    /* Plan 06 (NAV-12, D-12): workspace/diagnostic per-file cache.
     * Created eagerly so the handler's first call finds a non-NULL cache
     * even when no workspace_index exists (pull gracefully returns empty). */
    g_server.ws_diag_cache     = ilsp_ws_diag_cache_create();
    atomic_store(&g_server.next_request_id, 1);

    if (!g_server.writer || !g_server.cancels || !g_server.dyn_reg) {
        ilsp_log(ILSP_LOG_ERROR, "startup-failure",
                 "server subsystem allocation failed; exiting");
        fprintf(stderr, "ironls: server subsystem allocation failed\n");
        /* Best-effort cleanup of any partial allocations. */
        if (g_server.writer)  ilsp_writer_destroy(g_server.writer);
        if (g_server.cancels) ilsp_cancel_registry_destroy(g_server.cancels);
        if (g_server.dyn_reg) ilsp_dyn_register_destroy(g_server.dyn_reg);
        ilsp_log_close();
        return 1;
    }

    /* ── 6. Writer thread ─────────────────────────────────────────────── */
    ilsp_writer_start(g_server.writer);

    /* ── 7. Reader thread ─────────────────────────────────────────────── */
    g_server.reader = ilsp_reader_create(stdin, on_message, &g_server);
    if (!g_server.reader) {
        ilsp_log(ILSP_LOG_ERROR, "startup-failure",
                 "reader allocation failed; exiting");
        ilsp_writer_shutdown(g_server.writer);
        ilsp_writer_join(g_server.writer);
        ilsp_writer_destroy(g_server.writer);
        ilsp_cancel_registry_destroy(g_server.cancels);
        ilsp_dyn_register_destroy(g_server.dyn_reg);
        ilsp_log_close();
        return 1;
    }
    ilsp_reader_start(g_server.reader);

    /* ── 8. Block on reader lifetime ──────────────────────────────────── */
    ilsp_log(ILSP_LOG_INFO, "running",
             "reader+writer threads started; entering main loop");
    ilsp_reader_join(g_server.reader);

    /* ── a. Per-doc worker teardown ───────────────────────────────────── */
    ilsp_log(ILSP_LOG_INFO, "shutdown-docs",
             "reader exited; joining per-document workers");
    if (g_server.documents) {
        for (ptrdiff_t i = 0; i < shlen(g_server.documents); i++) {
            IronLsp_Document *d = g_server.documents[i].value;
            if (!d) continue;
            ilsp_ast_worker_shutdown_and_join(d);
            ilsp_document_destroy(d);
        }
        shfree(g_server.documents);
        g_server.documents = NULL;
    }

    /* ── d. Writer teardown ───────────────────────────────────────────── */
    ilsp_log(ILSP_LOG_INFO, "shutdown-writer", "draining + joining writer");
    ilsp_writer_shutdown(g_server.writer);
    ilsp_writer_join(g_server.writer);
    ilsp_writer_destroy(g_server.writer);
    g_server.writer = NULL;

    /* ── e. Reader destroy (already joined) ───────────────────────────── */
    ilsp_reader_destroy(g_server.reader);
    g_server.reader = NULL;

    /* ── f. Cancels + dyn_reg ─────────────────────────────────────────── */
    ilsp_cancel_registry_destroy(g_server.cancels);
    g_server.cancels = NULL;
    ilsp_dyn_register_destroy(g_server.dyn_reg);
    g_server.dyn_reg = NULL;
    if (g_server.workspace_root) {
        free(g_server.workspace_root);
        g_server.workspace_root = NULL;
    }
    if (g_server.workspace_index) {
        ilsp_workspace_index_destroy(g_server.workspace_index);
        g_server.workspace_index = NULL;
    }
    if (g_server.ws_diag_cache) {
        ilsp_ws_diag_cache_destroy(g_server.ws_diag_cache);
        g_server.ws_diag_cache = NULL;
    }

    /* ── g. Trace dump ────────────────────────────────────────────────── */
    ilsp_trace_dump(stderr);

    /* ── h/i. Exit code + log close ───────────────────────────────────── */
    int exit_code = (g_server.lifecycle == ILSP_LIFECYCLE_EXIT_QUEUED) ? 0 : 1;
    ilsp_log(ILSP_LOG_INFO, "exit",
             "ironls exiting (code=%d, lifecycle=%s)",
             exit_code, ilsp_lifecycle_state_name(g_server.lifecycle));
    ilsp_log_close();

    return exit_code;
}
