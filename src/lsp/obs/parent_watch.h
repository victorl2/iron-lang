#ifndef IRON_LSP_OBS_PARENT_WATCH_H
#define IRON_LSP_OBS_PARENT_WATCH_H

/* Phase 7 Plan 07-01 Task 02 (HARD-20, D-08) -- Parent-death detection.
 *
 * ilsp_parent_watch_init installs a platform-appropriate watcher that
 * delivers SIGTERM to the current process when the parent exits. The
 * server's reader thread observes SIGTERM via the existing Phase 2
 * SIGPIPE/SIGTERM path and begins graceful shutdown.
 *
 * Platforms:
 *   - Linux: prctl(PR_SET_PDEATHSIG, SIGTERM) -- kernel-managed; no
 *     thread required; triggers IMMEDIATELY on parent death. 6-line
 *     install.
 *   - macOS: kqueue + EVFILT_PROC + NOTE_EXIT watching getppid(); a
 *     background pthread polls kevent() every 1s and self-SIGTERMs on
 *     NOTE_EXIT delivery.
 *   - Other BSDs / exotic POSIX: fallback poll getppid() != 1 every 5s
 *     (if PPID becomes 1, init/launchd adopted us -> parent died).
 *
 * This is a one-shot install called from main() after log init and
 * before any worker threads spawn. Idempotent across repeated calls
 * (second+ calls are no-ops). */

#ifdef __cplusplus
extern "C" {
#endif

/* Install the parent-death watcher. Returns 0 on success, -1 on failure
 * (platform-dependent; on Linux the only possible failure is if prctl
 * itself returns -1, which on a sane kernel is impossible for this
 * call). Idempotent. */
int ilsp_parent_watch_init(void);

/* Introspection (tests only): returns non-zero if the parent-watch
 * subsystem has been installed, 0 otherwise. */
int ilsp_parent_watch_installed(void);

/* Testing hook: reset the idempotency latch so the next init call
 * actually re-installs. Used by unit tests that fork children which
 * need their own install (fork() inherits the parent's `s_installed`
 * static). NOT for production code. */
void ilsp_parent_watch_reset_for_testing(void);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_OBS_PARENT_WATCH_H */
