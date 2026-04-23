#ifndef IRON_LSP_CLI_ARGS_H
#define IRON_LSP_CLI_ARGS_H

/* Phase 7 Plan 07-01 Task 02 -- argv parser for ironls.
 *
 * Centralises the command-line argument parsing that used to live inline
 * in main.c so the supervisor + worker modes can share the same schema.
 *
 * Modes:
 *   - NORMAL:              no --supervised, no --__worker. Single-process
 *                          LSP (the default pre-Phase-7 behaviour).
 *   - SUPERVISED_PARENT:   --supervised. Fork a worker child via
 *                          ilsp_supervisor_run() and proxy stdio.
 *   - SUPERVISED_WORKER:   --__worker (INTERNAL flag, not documented in
 *                          --help output). Skips the supervisor path and
 *                          runs the normal server loop. Only the
 *                          supervisor parent sets this on its fork'd
 *                          child.
 *
 * Other flags preserved:
 *   --version / -v                 (existing)
 *   --log-dir=<path>               (existing)
 *   --log-level=<ERROR|WARN|INFO|DEBUG>  (existing)
 *   --rss-cap=<bytes>              (Phase 7 new; consumed by Plan 07-02 --
 *                                  parsed here so the CLI surface is
 *                                  stable; 0 disables the cap).
 *
 * Unknown flags are ignored (preserves pre-Phase-7 tolerance). */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum IlspMode {
    ILSP_MODE_NORMAL             = 0,
    ILSP_MODE_SUPERVISED_PARENT  = 1,
    ILSP_MODE_SUPERVISED_WORKER  = 2
} IlspMode;

typedef enum IlspArgsLogLevel {
    ILSP_ARGS_LOG_UNSET = -1,
    ILSP_ARGS_LOG_ERROR = 0,
    ILSP_ARGS_LOG_WARN  = 1,
    ILSP_ARGS_LOG_INFO  = 2,
    ILSP_ARGS_LOG_DEBUG = 3
} IlspArgsLogLevel;

typedef struct IlspArgs {
    IlspMode           mode;
    const char        *log_dir;          /* NULL unless --log-dir= given */
    IlspArgsLogLevel   log_level;        /* ILSP_ARGS_LOG_UNSET = default */
    uint64_t           rss_cap_bytes;    /* 0 = use default cap / disabled */
    bool               want_version;     /* --version / -v */
    bool               rss_cap_explicit; /* set when --rss-cap= was given */
} IlspArgs;

/* Parse argv. Never fails: unknown flags are ignored. */
IlspArgs ilsp_args_parse(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_CLI_ARGS_H */
