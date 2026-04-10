/* iron_net_init.c — Network runtime init hooks for Phase 59 P01c.
 *
 * Responsibilities:
 *   - Iron_net_wsa_startup_once / Iron_net_wsa_cleanup_once
 *     Refcounted WSAStartup wrapper (Windows only). Repeated calls from
 *     per-test iron_runtime_init harnesses are safe — only the first call
 *     actually hits WSAStartup, and only the last cleanup hits WSACleanup.
 *     On POSIX these are no-ops.
 *
 *   - iron_net_install_sigpipe_ignore
 *     Installs SIG_IGN for SIGPIPE on POSIX so writes to closed pipes/sockets
 *     return -1 with errno==EPIPE instead of killing the process. Windows
 *     has no SIGPIPE so it's a no-op there.
 *
 * These hooks are called from iron_runtime_init/shutdown in iron_string.c
 * (which owns the runtime lifecycle entry points).
 *
 * Implementation note: the refcount mutex is wrapped in a lock-init guard so
 * the very first call can initialise the mutex without a double-init race.
 * An atomic "initialised" flag would also work but would pull in <stdatomic.h>
 * on POSIX only; using an INIT_ONCE / pthread_once would be overkill for a
 * single-mutex lazy-init. We use a simple bool + compare — the first-call
 * race is only possible if two threads call iron_runtime_init concurrently,
 * which the existing runtime already does not support (see iron_string.c
 * iron_runtime_init — it assumes serial init).
 */

#include "runtime/iron_runtime.h"

#include <signal.h>

#ifdef _WIN32

/* winsock2.h must be included BEFORE windows.h; iron_runtime.h pulls in
 * windows.h on Windows, so we rely on the include order: iron_runtime.h
 * already includes windows.h, so here we include winsock2.h after it which
 * can be problematic. The cleanest fix is to define WIN32_LEAN_AND_MEAN
 * and include winsock2.h explicitly before any windows.h inclusion. Since
 * iron_runtime.h does that already only via #include <windows.h>, we
 * include winsock2.h with WIN32_LEAN_AND_MEAN semantics here — winsock2.h
 * pulls in enough of windows.h for our needs. */
#include <winsock2.h>

static iron_mutex_t s_wsa_lock;
static int          s_wsa_refcount = 0;
static bool         s_wsa_lock_init = false;

static void iron_net_ensure_wsa_lock(void) {
    if (!s_wsa_lock_init) {
        IRON_MUTEX_INIT(s_wsa_lock);
        s_wsa_lock_init = true;
    }
}

int Iron_net_wsa_startup_once(void) {
    iron_net_ensure_wsa_lock();
    IRON_MUTEX_LOCK(s_wsa_lock);
    int rc = 0;
    if (s_wsa_refcount == 0) {
        WSADATA wsa;
        rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    if (rc == 0) {
        s_wsa_refcount++;
    }
    IRON_MUTEX_UNLOCK(s_wsa_lock);
    return rc;
}

void Iron_net_wsa_cleanup_once(void) {
    if (!s_wsa_lock_init) return;
    IRON_MUTEX_LOCK(s_wsa_lock);
    if (s_wsa_refcount > 0) {
        s_wsa_refcount--;
        if (s_wsa_refcount == 0) {
            WSACleanup();
        }
    }
    IRON_MUTEX_UNLOCK(s_wsa_lock);
}

#else  /* !_WIN32 */

int Iron_net_wsa_startup_once(void) {
    /* POSIX has no WinSock init — return success unconditionally. */
    return 0;
}

void Iron_net_wsa_cleanup_once(void) {
    /* No-op on POSIX. */
}

#endif /* _WIN32 */

void iron_net_install_sigpipe_ignore(void) {
#ifndef _WIN32
    /* Install SIG_IGN so write(2) to a closed pipe / socket returns -1
     * with errno == EPIPE instead of delivering SIGPIPE (which by default
     * terminates the process).
     *
     * signal() is permitted here instead of sigaction() because we only
     * set SIG_IGN (no handler state, no restart semantics to configure)
     * and Iron programs don't rely on the default SIGPIPE behaviour.
     * Repeated calls are idempotent — the second installation writes the
     * same SIG_IGN value. */
    signal(SIGPIPE, SIG_IGN);
#endif
}
