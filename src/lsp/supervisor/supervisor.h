#ifndef IRON_LSP_SUPERVISOR_SUPERVISOR_H
#define IRON_LSP_SUPERVISOR_SUPERVISOR_H

/* Phase 7 Plan 07-01 Task 02 (HARD-13, D-01) -- Fork/proxy supervisor.
 *
 * `ilsp_supervisor_run` is the entry point selected by main() when the
 * user passes `--supervised`. It:
 *
 *   1. Creates two pipe pairs (editor<->worker stdin; worker->editor stdout)
 *   2. fork()s a worker child, which closes the unused pipe ends, dup2s
 *      the pipe ends to stdin/stdout, and runs the normal LSP server
 *      entry point with --__worker added to its argv.
 *   3. In the parent, closes the unused ends, installs a SIGCHLD handler
 *      (self-pipe style so the main poll() loop sees worker death), and
 *      enters a poll(2) byte-forwarder that shuttles bytes between
 *      editor-stdin <-> worker-stdin AND worker-stdout <-> editor-stdout.
 *   4. On worker death: waitpid(WNOHANG), reap, inject a synthetic
 *      `window/showMessage` LSP notification to the editor, then either
 *      sleep(backoff) + respawn, OR (if 5 crashes happened inside 60s)
 *      exit with code 1 (bailout).
 *
 * Per D-01 decision:
 *   - Backoff sequence: {1, 2, 4, 8, 16} seconds, cap at 16
 *   - Bailout: 5 consecutive crashes within 60s wall-clock -> _exit(1)
 *   - Worker does NOT preserve document state across restarts -- the
 *     editor re-didOpens (all 3 target clients handle this)
 *   - Parent does NOT parse LSP frames -- it only forwards bytes
 *   - Parent injects one well-formed LSP notification frame on restart
 *
 * The supervisor is transport-hermetic: it never includes any Phase 2
 * transport module headers. The only serialisation it knows is the
 * on-wire Content-Length LSP frame format (for the synthetic
 * window/showMessage injection).
 *
 * For unit tests, the module exposes internal helpers gated on
 * ILSP_SUPERVISOR_TESTING so the backoff table, bailout window, and
 * the byte-forwarder can be exercised without actually forking. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>   /* ssize_t for the test-only forward helper */

#ifdef __cplusplus
extern "C" {
#endif

/* Fork-and-proxy supervisor entry. `argv` should be the same argv that
 * main() received; the supervisor will propagate every element to the
 * worker's argv EXCEPT --supervised (stripped) and with --__worker
 * appended. Returns an exit code suitable for main() to return.
 *
 * Only returns after: (a) bailout threshold hit, (b) the editor closed
 * stdin AND the current worker exited cleanly, or (c) an unrecoverable
 * setup failure (pipe/fork/etc.). */
int ilsp_supervisor_run(int argc, char **argv);

/* ── Test surface (only defined when ILSP_SUPERVISOR_TESTING is set) ──
 *
 * The backoff computation + crash-window tracker are exposed so unit
 * tests can feed synthetic timelines and assert the correct delays +
 * bailout behaviour without actually forking workers. */
#ifdef ILSP_SUPERVISOR_TESTING

/* Return the backoff-delay-in-seconds for a given consecutive-crash
 * count: {1, 2, 4, 8, 16, 16, 16, ...}. crash_count 0 returns 1. */
int ilsp_supervisor_backoff_secs_for_test(unsigned crash_count);

/* Track a crash with a synthetic timestamp and return 1 if the bailout
 * threshold (5 crashes in 60s) is now met, else 0. State is module-
 * scope; call ilsp_supervisor_reset_bailout_for_test between independent
 * test timelines. */
int ilsp_supervisor_record_crash_for_test(int64_t ts_secs);
void ilsp_supervisor_reset_bailout_for_test(void);

/* Build the synthetic window/showMessage LSP frame into `out` (cap
 * bytes). Returns the number of bytes written (INCLUDING the
 * Content-Length header + CRLFCRLF separator), or 0 on overflow. */
size_t ilsp_supervisor_build_showmessage_for_test(char *out, size_t cap,
                                                   const char *message);

/* Byte-forward a buffer of `n` bytes from `in_fd` to `out_fd`. Retries
 * on EINTR/EAGAIN. Returns total bytes forwarded, or -1 on EPIPE/EBADF. */
ssize_t ilsp_supervisor_forward_bytes_for_test(int in_fd, int out_fd,
                                                size_t n);

#endif  /* ILSP_SUPERVISOR_TESTING */

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_SUPERVISOR_SUPERVISOR_H */
