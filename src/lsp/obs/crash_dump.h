#ifndef IRON_LSP_OBS_CRASH_DUMP_H
#define IRON_LSP_OBS_CRASH_DUMP_H

/* Phase 7 Plan 07-01 Task 01 (HARD-14, D-02) -- Crash dump pipeline.
 *
 * Installs SIGABRT + SIGSEGV handlers that, on delivery, write a 3-section
 * plain-text dump to $XDG_STATE_HOME/iron-lsp/crashes/<ISO8601>-<pid>.dmp.
 * The dump sections are:
 *
 *   === BACKTRACE ===          (backtrace_symbols_fd raw addresses)
 *   === IN-FLIGHT REQUESTS === (last 16 method/id pairs from ring buffer)
 *   === DOCUMENT STATE ===     (IRON_VERSION_FULL + uname + workspace root)
 *
 * The ring buffer is maintained by the dispatcher: ilsp_crash_ring_push
 * is called at handler entry (method dispatched) and ilsp_crash_ring_pop
 * at handler return. Both are wait-free atomic operations; the signal
 * handler reads the latest 16 entries (tolerating torn reads per the
 * D-02 + T-07-01-02 accepted risk).
 *
 * Signal-handler discipline (T-07-01-01 mitigation):
 *   - SA_RESETHAND on SIGSEGV so a recursive SEGV in the handler itself
 *     kills the process cleanly rather than looping.
 *   - SA_ONSTACK + 64KB alternate signal stack (sigaltstack) so stack
 *     overflow SIGSEGVs can still run the handler.
 *   - Every call in the handler is POSIX async-signal-safe:
 *       write(2), open(2), close(2), backtrace(3) (glibc + Apple libc),
 *       backtrace_symbols_fd(3), clock_gettime(CLOCK_REALTIME), raise(3),
 *       signal(3). NO printf, NO malloc, NO backtrace_symbols.
 *
 * SIGABRT continues to invoke Phase 2 sigsetjmp per-document quarantine
 * via src/lsp/obs/abort_handler.c -- this handler RUNS FIRST (installed
 * via sigaction under the same SIGABRT slot) to write the dump, then
 * delegates to the Phase 2 handler by chaining. See crash_dump.c for
 * the chain mechanism.
 *
 * SIGSEGV writes the dump then raises SIGSEGV with SIG_DFL (correct exit
 * code propagation per D-02). */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install SIGABRT + SIGSEGV handlers and prepare alternate signal stack +
 * pre-computed crash-directory path. Idempotent across repeated calls.
 * Must be called once at server startup, AFTER ilsp_log_open (shares XDG
 * resolution semantics). */
void ilsp_crash_install_handlers(void);

/* Wait-free push of (request_id, method) into the 16-slot ring buffer.
 * Called from the dispatcher at handler entry. `method` is copied into
 * a fixed 48-byte field; oversize method strings are truncated. */
void ilsp_crash_ring_push(uint64_t request_id, const char *method);

/* Wait-free pop. Clears the slot matching `request_id` if present,
 * otherwise no-op. Called from the dispatcher at handler return. */
void ilsp_crash_ring_pop(uint64_t request_id);

/* Inform the crash-dump module of the currently-active workspace root
 * so it can be included in the DOCUMENT STATE section. Called by
 * handlers_lifecycle when `initialize` resolves the root. Safe to call
 * before install; passing NULL clears the stored value. */
void ilsp_crash_set_workspace_root(const char *root);

/* Introspection (tests only): return the absolute path to the crashes/
 * directory that ilsp_crash_install_handlers resolved at startup, or
 * NULL if install was never called. Pointer is module-scope stable. */
const char *ilsp_crash_dir_path(void);

/* Introspection (tests only): reset the module to its pre-install state.
 * Drops handlers + clears the ring buffer. Useful for isolating unit
 * tests that each need a fresh install/teardown cycle. */
void ilsp_crash_reset_for_testing(void);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_OBS_CRASH_DUMP_H */
