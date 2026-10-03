/* Phase 7 Plan 07-01 Task 02 (HARD-20, D-08) -- Parent-death detection.
 *
 * On Linux we use prctl(PR_SET_PDEATHSIG, SIGTERM) -- kernel delivers
 * SIGTERM to us the instant our parent exits. This is the canonical
 * TLPI pattern (Kerrisk §27.5) and requires no threads, no polling,
 * no kqueue-equivalent abstraction.
 *
 * Per T-07-01 threat-model note: prctl resets its PR_SET_PDEATHSIG
 * disposition on any setuid/setgid-induced privilege change. Our LSP
 * never calls setuid/setgid post-init (we are a user-land-only tool),
 * but we install AFTER log_open (which might create files under HOME)
 * just in case a future feature does.
 *
 * On macOS we use kqueue(2) + EVFILT_PROC + NOTE_EXIT. Apple's man
 * page guarantees EV_ADD with an already-exited PID triggers NOTE_EXIT
 * immediately (T-07-01-07 mitigated race). A background pthread spins
 * on kevent() with a 1s timeout and self-SIGTERMs on NOTE_EXIT.
 *
 * On other POSIX (no prctl, no kqueue) we poll getppid() != 1 every 5s
 * from a background thread. PPID==1 after having had a non-init parent
 * means init/launchd reparented us -- parent died.
 *
 * The SIGTERM routing is intentional: Phase 2's transport layer already
 * treats SIGTERM as the graceful-shutdown signal (see
 * src/lsp/transport/reader.c SIGPIPE/SIGTERM handling). Parent-death
 * therefore folds into the existing shutdown path with zero new wiring.
 */

#include "lsp/obs/parent_watch.h"
#include "lsp/obs/log.h"

#include <errno.h>
#include "util/pthread_compat.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "util/os.h"

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#endif

static _Atomic int s_installed = 0;

int ilsp_parent_watch_installed(void) {
    return atomic_load(&s_installed);
}

void ilsp_parent_watch_reset_for_testing(void) {
    atomic_store(&s_installed, 0);
}

#if defined(__APPLE__)

static void *macos_kqueue_watcher(void *arg) {
    int kq = (int)(intptr_t)arg;
    struct kevent ev;
    while (1) {
        struct timespec t = { .tv_sec = 1, .tv_nsec = 0 };
        int n = kevent(kq, NULL, 0, &ev, 1, &t);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;  /* kqueue broke; let the fallback in main() handle it */
        }
        if (n == 0) continue;  /* timeout, re-poll */
        if (ev.filter == EVFILT_PROC && (ev.fflags & NOTE_EXIT)) {
            ilsp_log(ILSP_LOG_INFO, "parent-death",
                     "kqueue reported parent NOTE_EXIT; self-SIGTERM");
            kill(getpid(), SIGTERM);
            break;
        }
    }
    close(kq);
    return NULL;
}

#endif  /* __APPLE__ */

#if defined(_WIN32)

/* Windows: the parent's process id comes from the process snapshot; a
 * handle on it with SYNCHRONIZE lets a thread wait for its exit. */
#include <tlhelp32.h>
static DWORD win_parent_pid(void) {
    DWORD me = GetCurrentProcessId(), parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof pe;
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == me) { parent = pe.th32ParentProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return parent;
}
static void *win_parent_watcher(void *arg) {
    HANDLE parent = (HANDLE)arg;
    WaitForSingleObject(parent, INFINITE);
    ilsp_log(ILSP_LOG_INFO, "parent-death", "parent process exited; exiting");
    CloseHandle(parent);
    _exit(0);
    return NULL;
}

#elif !defined(__linux__) && !defined(__APPLE__)

/* Fallback polling thread for exotic BSDs. */
static void *ppid_polling_watcher(void *unused) {
    (void)unused;
    pid_t initial_parent = getppid();
    while (1) {
        sleep(5);
        pid_t now = getppid();
        if (now == 1 && initial_parent != 1) {
            ilsp_log(ILSP_LOG_INFO, "parent-death",
                     "PPID=1 (was %d); self-SIGTERM", (int)initial_parent);
            kill(getpid(), SIGTERM);
            break;
        }
    }
    return NULL;
}

#endif  /* fallback */

int ilsp_parent_watch_init(void) {
    /* Idempotent. */
    int expected = 0;
    if (!atomic_compare_exchange_strong(&s_installed, &expected, 1)) {
        return 0;
    }

#if defined(__linux__)
    /* Linux: kernel-managed, zero threads. */
    pid_t parent_before_install = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) != 0) {
        int saved = errno;
        ilsp_log(ILSP_LOG_WARN, "parent-watch",
                 "prctl(PR_SET_PDEATHSIG) failed errno=%d", saved);
        atomic_store(&s_installed, 0);
        return -1;
    }
    /* Race guard: if the parent died between our snapshot and prctl, the
     * kernel could not deliver the newly configured signal. Compare PIDs
     * rather than treating PPID 1 as inherently orphaned: inside a container,
     * a healthy launcher or editor can legitimately be PID 1. */
    if (getppid() != parent_before_install) {
        ilsp_log(ILSP_LOG_INFO, "parent-watch",
                 "parent changed during watcher install; self-SIGTERM");
        kill(getpid(), SIGTERM);
    }
    return 0;

#elif defined(__APPLE__)
    int kq = kqueue();
    if (kq < 0) {
        ilsp_log(ILSP_LOG_WARN, "parent-watch",
                 "kqueue() failed errno=%d", errno);
        atomic_store(&s_installed, 0);
        return -1;
    }
    struct kevent kev;
    EV_SET(&kev, getppid(), EVFILT_PROC, EV_ADD | EV_ENABLE,
           NOTE_EXIT, 0, 0);
    if (kevent(kq, &kev, 1, NULL, 0, NULL) < 0) {
        int saved = errno;
        close(kq);
        ilsp_log(ILSP_LOG_WARN, "parent-watch",
                 "kevent(EV_ADD) failed errno=%d", saved);
        atomic_store(&s_installed, 0);
        return -1;
    }
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, macos_kqueue_watcher,
                       (void *)(intptr_t)kq) != 0) {
        pthread_attr_destroy(&attr);
        close(kq);
        ilsp_log(ILSP_LOG_WARN, "parent-watch",
                 "pthread_create failed for kqueue watcher");
        atomic_store(&s_installed, 0);
        return -1;
    }
    pthread_attr_destroy(&attr);
    return 0;

#elif defined(_WIN32)
    DWORD ppid = win_parent_pid();
    HANDLE parent = ppid ? OpenProcess(SYNCHRONIZE, FALSE, ppid) : NULL;
    if (!parent) {
        ilsp_log(ILSP_LOG_WARN, "parent-watch", "cannot open the parent process");
        atomic_store(&s_installed, 0);
        return -1;
    }
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, win_parent_watcher, parent);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        CloseHandle(parent);
        ilsp_log(ILSP_LOG_WARN, "parent-watch", "pthread_create failed for the parent watcher");
        atomic_store(&s_installed, 0);
        return -1;
    }
    return 0;
#else
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &attr, ppid_polling_watcher, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        ilsp_log(ILSP_LOG_WARN, "parent-watch",
                 "pthread_create failed for PPID poller");
        atomic_store(&s_installed, 0);
        return -1;
    }
    return 0;
#endif
}
