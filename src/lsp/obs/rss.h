#ifndef IRON_LSP_OBS_RSS_H
#define IRON_LSP_OBS_RSS_H

/* Phase 7 Plan 07-02 Task 01 (HARD-15, D-03) -- RSS measurement + cap.
 *
 * Portable resident-set-size abstraction and background sampler that
 * trips a self-restart (`_exit(42)`) when the cap is exceeded.
 *
 * Platforms:
 *   - Linux:  read "VmRSS" from /proc/self/status (KiB) × 1024 = bytes
 *   - macOS:  task_info(mach_task_self(), TASK_BASIC_INFO, ...) ->
 *             info.resident_size (bytes, passthrough)
 *   - Other:  unsupported; ilsp_rss_current_bytes() returns 0 so the
 *             cap never trips (documented fallback per D-03).
 *
 * Cap enforcement:
 *   - Default cap: 1 073 741 824 bytes (1 GiB = 1024³). Matches
 *     rust-analyzer's default; sized for v1 Iron workspaces up to
 *     ~100 kLoC with 10× headroom.
 *   - Override order (highest -> lowest priority):
 *       1. CLI flag --rss-cap=<bytes>   (parsed by src/lsp/cli/args.c)
 *       2. Env var IRON_LSP_RSS_CAP_BYTES
 *       3. Compiled-in default (1 GiB)
 *   - Setting IRON_LSP_RSS_CAP_BYTES=0 disables the cap entirely
 *     (documented developer escape hatch, T-07-02-06 accepted risk).
 *
 * Sampling:
 *   - Background pthread started by ilsp_rss_cap_init; samples every
 *     5 seconds via ilsp_rss_current_bytes(). No global mutable state
 *     beyond an atomic install latch + the cap value.
 *
 * Exit-42 discipline (T-07-02-05):
 *   - When sampled RSS > cap, the sampler thread does, in order:
 *       1. Emit a framed window/logMessage MessageType=Error directly
 *          to stdout (synchronous write -- the async writer queue
 *          cannot be trusted once we're about to _exit). The message
 *          body follows UI-SPEC S1/S5:
 *            "RSS cap exceeded: current=<X>MiB cap=<Y>MiB. Restarting."
 *       2. Write a plain-text marker dump to
 *          $XDG_STATE_HOME/iron-lsp/rss-restart-<ISO8601>.log containing
 *          a /proc/self/status snapshot (Linux) OR the task_info fields
 *          (macOS) plus the last 16 in-flight requests from the 07-01
 *          crash ring buffer.
 *       3. Call _exit(42) -- NOT exit(). We deliberately skip atexit
 *          handlers because they may re-enter the failing path (e.g.
 *          writer-thread drain allocating more memory).
 *
 * Exit code 42 is the documented RSS-cap-restart sentinel, distinct
 * from 0 (normal shutdown) and 1 (early-failure crash). The supervisor
 * (Plan 07-01 Task 02) and editor clients (Phase 6) both treat it as
 * an expected-restart signal and reconnect. */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return the process' current resident-set size in bytes, or 0 on
 * unsupported platforms / failure. Thread-safe. May be called at any
 * time (before, during, or after ilsp_rss_cap_init). */
uint64_t ilsp_rss_current_bytes(void);

/* Start the background 5-second sampler that enforces `cap_bytes`.
 * - cap_bytes == 0  : cap disabled; no thread is spawned; returns 0.
 * - cap_bytes  > 0  : spawn a daemon pthread; returns 0 on success,
 *                     -1 if pthread_create failed.
 * Idempotent: second and subsequent calls return 0 without re-spawning. */
int ilsp_rss_cap_init(uint64_t cap_bytes);

/* Introspection (tests only): true iff ilsp_rss_cap_init was called
 * with a non-zero cap and the sampler thread is live. */
bool ilsp_rss_cap_installed(void);

/* Introspection (tests only): return the cap the sampler is enforcing,
 * or 0 if disabled / not yet installed. */
uint64_t ilsp_rss_cap_bytes(void);

/* Testing hook: reset the install latch so a subsequent cap_init()
 * re-spawns the thread. DOES NOT join the existing thread (unit tests
 * exercise the logic via the synchronous check helper below; the
 * production integration is exercised by the end-to-end shell
 * invariant). */
void ilsp_rss_reset_for_testing(void);

/* Testing hook: inject a synthetic RSS reading. Subsequent calls to
 * ilsp_rss_current_bytes() return `bytes` instead of probing the OS.
 * Pass 0 to clear the override and resume OS probing. Used by the
 * Unity tests to exercise the cap-trip branch without actually
 * allocating gigabytes. */
void ilsp_rss_set_override_for_testing(uint64_t bytes);

/* Testing hook: perform one synchronous sample + cap check on the
 * calling thread (bypassing the sleep loop). Returns the action taken:
 *   0 = no action (cap not exceeded OR cap disabled OR RSS unknown)
 *   1 = would-trip (test mode: the marker file is still written but
 *       _exit(42) is replaced with `*out_tripped = true` so the test
 *       can assert without exiting the harness)
 * If `out_tripped` is NULL and the cap trips, this function calls
 * _exit(42) as in production. */
int ilsp_rss_sample_and_check_for_testing(bool *out_tripped);

/* Testing hook: override the /proc/self/status parse path. Pass a
 * NUL-terminated buffer containing procfs-style status text; the
 * parser will extract the VmRSS line from it instead of reading
 * /proc/self/status. Pass NULL to resume real procfs reads. Linux-
 * path coverage for the unit test. */
void ilsp_rss_set_procfs_override_for_testing(const char *status_text);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_OBS_RSS_H */
