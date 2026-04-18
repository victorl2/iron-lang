#ifndef IRON_LSP_OBS_LOG_H
#define IRON_LSP_OBS_LOG_H

/* Phase 2 Plan 06 Task 01 (CORE-21) -- Structured JSON-line log sink.
 *
 * `ilsp_log_open(override_dir)` resolves the state directory using the
 * XDG Base Directory Spec v0.8 order and opens
 * `<dir>/iron-lsp/server-<pid>.log` (or `<override_dir>/server-<pid>.log`
 * when the caller wants to pin the path). Every call emits one JSON
 * object per line; keys are kept short (`ts`, `pid`, `lvl`, `event`,
 * `msg`) to keep the log grep-friendly.
 *
 * Resolution order (matches RESEARCH.md §Logging, Example 3):
 *   1. $XDG_STATE_HOME (if set and non-empty)  -> $XDG_STATE_HOME/iron-lsp
 *   2. $HOME/.local/state                       -> $HOME/.local/state/iron-lsp
 *   3. /tmp/iron-lsp                             (last-ditch; warn on stderr)
 *
 * Level filtering is driven by the `$IRONLS_LOG` env var (ERROR|WARN|
 * INFO|DEBUG; default WARN). `ilsp_log_set_level()` lets `--log-level=`
 * argv override the env after `ilsp_log_open`.
 *
 * Thread-safety: a single module-level mutex wraps every fwrite + flush
 * so concurrent writes from reader / writer / worker threads cannot
 * garble a JSON line. Rotation triggers at 100 MB by renaming the
 * current file to `<path>.1` and opening a fresh one.
 *
 * The sink is NULL-safe -- ilsp_log() before ilsp_log_open() is a no-op
 * rather than a crash, so tests can call the loggers without booting
 * the full stack. */

#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IronLsp_LogLevel {
    ILSP_LOG_ERROR = 0,
    ILSP_LOG_WARN  = 1,
    ILSP_LOG_INFO  = 2,
    ILSP_LOG_DEBUG = 3
} IronLsp_LogLevel;

/* Open the JSON-line log sink.
 *   - override_dir != NULL:     <override_dir>/server-<pid>.log
 *   - override_dir == NULL:     resolve XDG; <dir>/iron-lsp/server-<pid>.log
 * Returns 0 on success, -1 on failure. On failure the global sink is
 * unchanged (log calls remain no-ops). */
int  ilsp_log_open(const char *override_dir);

/* Flush + close + reset global pointer. Safe to call before open. */
void ilsp_log_close(void);

/* printf-style log emission. `level` is compared against the current
 * level (default WARN; overridable via $IRONLS_LOG or
 * ilsp_log_set_level). Lines below threshold are dropped. */
void ilsp_log(IronLsp_LogLevel level, const char *event,
              const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* Current threshold and override. */
IronLsp_LogLevel ilsp_log_level(void);
void             ilsp_log_set_level(IronLsp_LogLevel lvl);

/* Introspection for tests. Returns NULL if no sink is open. */
const char *ilsp_log_path(void);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_OBS_LOG_H */
