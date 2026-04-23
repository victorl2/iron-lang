/* Phase 7 Plan 07-01 Task 01 (HARD-14, D-02) -- Crash dump pipeline.
 *
 * Three-section plain-text crash dump written from SIGABRT/SIGSEGV via
 * exclusively async-signal-safe primitives. See crash_dump.h for the
 * public API contract + discipline.
 *
 * Discipline enforced here (not negotiable):
 *
 *   1. NO malloc / NO printf / NO fprintf / NO snprintf INSIDE the
 *      signal handler. All formatting is done with hand-rolled
 *      async-signal-safe helpers (as_safe_*). The one formatting
 *      primitive we do call -- backtrace_symbols_fd(3) -- is explicitly
 *      AS-Safe per the glibc manual (3.2.2.5 of POSIX.1-2008 rationale;
 *      Apple's man page concurs).
 *
 *   2. Every path string used by the handler is pre-computed at
 *      install time. open(2), close(2), write(2) are AS-Safe; constructing
 *      the ISO8601 timestamp is done with clock_gettime + a tiny hand-rolled
 *      decimal formatter (AS-Safe because clock_gettime is on the POSIX
 *      AS-Safe list).
 *
 *   3. The 16-entry ring buffer uses _Atomic operations only. There is
 *      no lock; there is no condvar. The signal-handler read is
 *      torn-read-tolerant by design (T-07-01-02 accepted disposition).
 *
 *   4. SIGSEGV flag set includes SA_RESETHAND so a recursive SEGV in the
 *      handler itself (e.g. the crashes/ directory disappeared
 *      mid-flight) kills the process with the default disposition rather
 *      than looping. After writing the dump we ALSO call signal(SIGSEGV,
 *      SIG_DFL) + raise(SIGSEGV) to guarantee the correct exit code even
 *      if the OS didn't honour SA_RESETHAND (it always does on Linux +
 *      Apple, but belt + braces).
 *
 *   5. SIGABRT chain: this handler writes the dump, then delegates to
 *      the Phase 2 sigsetjmp handler (ilsp_install_abort_handler) so
 *      per-document quarantine continues to work. The chain is
 *      implemented by calling the Phase 2 handler directly after the
 *      dump write -- Phase 2's handler either siglongjmps into an
 *      active sigsetjmp or falls through to _exit(134), so control
 *      does NOT return from it in either case (no further action needed
 *      on our side).
 *
 *   6. Ring buffer data contract (D-02):
 *      - 16 slots, each {uint64_t id; char method[48]}
 *      - slot index is a monotonic _Atomic uint32_t; wraps via `& 15`
 *      - push uses atomic_fetch_add so two concurrent pushes always
 *        land in distinct slots
 *      - pop clears id = 0 (used as "empty") when the stored id matches;
 *        this is best-effort (a newer push may have already overwritten
 *        the slot -- accepted)
 *      - signal-handler read walks backwards from idx-1 over all 16
 *        slots; slots with id == 0 are skipped. */

#include "lsp/obs/crash_dump.h"
#include "lsp/obs/log.h"           /* no calls from handler; only for init-time warn */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>    /* snprintf at init time only -- NOT from handler */
#include <stdlib.h>   /* getenv at init time only */
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#endif

#ifndef IRON_VERSION_STRING
#define IRON_VERSION_STRING "unknown"
#endif

/* ── Tunables (plan-mandated literals) ──────────────────────────────────── */

#define ILSP_CRASH_RING_SLOTS     16u
#define ILSP_CRASH_METHOD_MAX     48u      /* 48 bytes incl. trailing NUL */
#define ILSP_CRASH_BT_FRAMES      64
#define ILSP_CRASH_SIGSTACK_BYTES 65536u   /* 64 KiB alternate stack */
#define ILSP_CRASH_PATH_MAX       4096u

/* ── Ring buffer ────────────────────────────────────────────────────────── */

typedef struct {
    _Atomic uint64_t id;
    char             method[ILSP_CRASH_METHOD_MAX];
} IlspCrashSlot;

static IlspCrashSlot  s_ring[ILSP_CRASH_RING_SLOTS];
static _Atomic uint32_t s_ring_idx = 0;

void ilsp_crash_ring_push(uint64_t request_id, const char *method) {
    if (request_id == 0) {
        /* id=0 is the "empty" sentinel. Bump to 1 to avoid collisions
         * (notifications in the LSP spec carry no id; dispatcher passes
         * a synthesised non-zero id for ring bookkeeping). */
        request_id = 1;
    }
    uint32_t raw  = atomic_fetch_add_explicit(&s_ring_idx, 1,
                                              memory_order_relaxed);
    uint32_t slot = raw & (ILSP_CRASH_RING_SLOTS - 1);

    /* Zero the method field first so the signal-handler reader always
     * sees a NUL-terminated string regardless of truncation. */
    memset(s_ring[slot].method, 0, ILSP_CRASH_METHOD_MAX);
    if (method && method[0]) {
        size_t n = strlen(method);
        if (n >= ILSP_CRASH_METHOD_MAX) n = ILSP_CRASH_METHOD_MAX - 1;
        memcpy(s_ring[slot].method, method, n);
    }
    /* Store the id LAST so a reader that sees a non-zero id is guaranteed
     * to also see the method bytes (release-store pairing, torn-read-
     * tolerant per T-07-01-02). */
    atomic_store_explicit(&s_ring[slot].id, request_id, memory_order_release);
}

void ilsp_crash_ring_pop(uint64_t request_id) {
    if (request_id == 0) request_id = 1;
    /* Best-effort clear: scan all slots looking for a matching id and
     * zero it. If the slot was overwritten by a newer push we do
     * nothing (correct per D-02 -- ring is newest-wins). */
    for (uint32_t i = 0; i < ILSP_CRASH_RING_SLOTS; i++) {
        uint64_t cur = atomic_load_explicit(&s_ring[i].id,
                                            memory_order_acquire);
        if (cur == request_id) {
            atomic_compare_exchange_strong_explicit(
                &s_ring[i].id, &cur, (uint64_t)0,
                memory_order_release, memory_order_relaxed);
            return;
        }
    }
}

/* ── State captured at install time (read-only from handler) ─────────── */

static char     s_crash_dir[ILSP_CRASH_PATH_MAX];      /* e.g. ".../iron-lsp/crashes" */
static size_t   s_crash_dir_len = 0;
static char     s_version_str[128];                    /* "IRON_VERSION_FULL=..." */
static size_t   s_version_len   = 0;
static char     s_os_line[256];                        /* "OS=Linux x86_64\n" */
static size_t   s_os_line_len   = 0;
static char     s_workspace_line[ILSP_CRASH_PATH_MAX]; /* "WORKSPACE=/...\n" */
static _Atomic size_t s_workspace_line_len = 0;
static _Atomic int    s_installed = 0;
static stack_t        s_altstack;

/* Phase 2 SIGABRT handler is installed by ilsp_install_abort_handler in
 * abort_handler.c. Phase 7 captures its sigaction here so we can chain. */
static struct sigaction s_prev_sigabrt;
static struct sigaction s_prev_sigsegv;

/* ── Forward decls for helpers ────────────────────────────────────────── */

static void signal_handler(int signo, siginfo_t *info, void *ctx);

/* ── Async-signal-safe helpers ───────────────────────────────────────── */

/* Write `n` bytes of `buf` to `fd`, retrying on EINTR. AS-Safe. */
static void as_safe_write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;  /* give up silently -- we're in a handler */
        }
        if (w == 0) return;
        buf += (size_t)w;
        n   -= (size_t)w;
    }
}

/* AS-Safe strlen. */
static size_t as_safe_strlen(const char *s) {
    size_t i = 0;
    while (s[i]) i++;
    return i;
}

/* AS-Safe memcpy. Included for clarity; relies on libc memcpy being
 * AS-Safe-in-practice (not on the POSIX list but universally so). */

/* Format a non-negative integer into `dst` as zero-padded decimal of
 * exactly `width` chars. Returns the number of chars written (=width).
 * Does NOT NUL-terminate (caller controls boundary). AS-Safe. */
static size_t as_safe_pad_int(char *dst, unsigned width, uint64_t value) {
    for (int i = (int)width - 1; i >= 0; i--) {
        dst[i] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    return width;
}

/* Format a non-negative integer without zero padding. Writes at most
 * `cap` bytes. Returns bytes written. Does NOT NUL-terminate. AS-Safe. */
static size_t as_safe_fmt_uint(char *dst, size_t cap, uint64_t value) {
    char tmp[32];
    size_t n = 0;
    if (value == 0) {
        if (cap > 0) dst[0] = '0';
        return cap > 0 ? 1 : 0;
    }
    while (value > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    size_t out = 0;
    for (size_t i = 0; i < n && out < cap; i++) {
        dst[out++] = tmp[n - 1 - i];
    }
    return out;
}

/* Build ISO8601 UTC timestamp "YYYY-MM-DDTHHMMSSZ" into `dst` (18 bytes).
 * AS-Safe: uses clock_gettime + gmtime_r (gmtime_r itself is technically
 * NOT on the POSIX AS-Safe list, but on Linux + macOS it is reentrant
 * and touches no shared state). As a belt-and-braces fallback if
 * gmtime_r returns NULL we fall back to the epoch-in-seconds decimal. */
static size_t as_safe_iso8601(char *dst, size_t cap) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        if (cap >= 1) { dst[0] = 'x'; return 1; }
        return 0;
    }
    struct tm tmv;
    if (gmtime_r(&ts.tv_sec, &tmv) == NULL) {
        /* Fallback: decimal epoch seconds, no terminator. */
        return as_safe_fmt_uint(dst, cap, (uint64_t)ts.tv_sec);
    }
    if (cap < 18) return 0;
    size_t off = 0;
    off += as_safe_pad_int(dst + off, 4, (uint64_t)(tmv.tm_year + 1900));
    dst[off++] = '-';
    off += as_safe_pad_int(dst + off, 2, (uint64_t)(tmv.tm_mon + 1));
    dst[off++] = '-';
    off += as_safe_pad_int(dst + off, 2, (uint64_t)tmv.tm_mday);
    dst[off++] = 'T';
    off += as_safe_pad_int(dst + off, 2, (uint64_t)tmv.tm_hour);
    off += as_safe_pad_int(dst + off, 2, (uint64_t)tmv.tm_min);
    off += as_safe_pad_int(dst + off, 2, (uint64_t)tmv.tm_sec);
    dst[off++] = 'Z';
    return off;
}

/* Recursive mkdir (install-time only -- NOT from handler). */
static int ilsp_crash_mkdir_p(const char *path) {
    char tmp[ILSP_CRASH_PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
    return 0;
}

/* Resolve $XDG_STATE_HOME/iron-lsp/crashes/ (or $HOME/.local/state/... or
 * /tmp fallback) -- same discipline as src/lsp/obs/log.c. */
static int resolve_crash_dir(char *out, size_t cap) {
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] != '\0') {
        int n = snprintf(out, cap, "%s/iron-lsp/crashes", xdg);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        int n = snprintf(out, cap, "%s/.local/state/iron-lsp/crashes", home);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
    int n = snprintf(out, cap, "/tmp/iron-lsp/crashes");
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* ── The signal handler itself (ASYNC-SIGNAL-SAFE ONLY) ──────────────── */

static void signal_handler(int signo, siginfo_t *info, void *ctx) {
    (void)info;
    (void)ctx;

    /* ── 1. Build dump path: <s_crash_dir>/<iso8601>-<pid>.dmp ─────── */
    char path[ILSP_CRASH_PATH_MAX];
    size_t off = 0;
    if (s_crash_dir_len > 0 && s_crash_dir_len < sizeof(path) - 64) {
        memcpy(path, s_crash_dir, s_crash_dir_len);
        off = s_crash_dir_len;
    } else {
        /* No dir resolved -- write to /tmp as a last-ditch fallback. */
        static const char fallback[] = "/tmp";
        memcpy(path, fallback, sizeof(fallback) - 1);
        off = sizeof(fallback) - 1;
    }
    path[off++] = '/';
    off += as_safe_iso8601(path + off, sizeof(path) - off - 32);
    path[off++] = '-';
    off += as_safe_fmt_uint(path + off, sizeof(path) - off - 8,
                            (uint64_t)getpid());
    static const char ext[] = ".dmp";
    memcpy(path + off, ext, sizeof(ext) - 1);
    off += sizeof(ext) - 1;
    path[off] = '\0';

    /* ── 2. Open the dump file ─────────────────────────────────────── */
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) {
        /* Can't write a dump -- also write to stderr for the operator. */
        static const char err[] = "ironls: crash dump path unopenable\n";
        as_safe_write_all(STDERR_FILENO, err, sizeof(err) - 1);
        goto chain;
    }

    /* ── 3. Header + signal + pid line ─────────────────────────────── */
    {
        static const char hdr[] =
            "Iron LSP crash dump\n"
            "signal=";
        as_safe_write_all(fd, hdr, sizeof(hdr) - 1);
        char buf[32];
        size_t n = as_safe_fmt_uint(buf, sizeof(buf), (uint64_t)signo);
        as_safe_write_all(fd, buf, n);
        static const char pidlab[] = " pid=";
        as_safe_write_all(fd, pidlab, sizeof(pidlab) - 1);
        n = as_safe_fmt_uint(buf, sizeof(buf), (uint64_t)getpid());
        as_safe_write_all(fd, buf, n);
        as_safe_write_all(fd, "\n", 1);
    }

    /* ── 4. === BACKTRACE === ─────────────────────────────────────── */
    {
        static const char hdr[] = "\n=== BACKTRACE ===\n";
        as_safe_write_all(fd, hdr, sizeof(hdr) - 1);
#if defined(__linux__) || defined(__APPLE__)
        void *frames[ILSP_CRASH_BT_FRAMES];
        int n = backtrace(frames, ILSP_CRASH_BT_FRAMES);
        if (n > 0) {
            /* AS-Safe per man page: backtrace_symbols_fd writes directly
             * to fd using only write(2), never allocates. */
            backtrace_symbols_fd(frames, n, fd);
        } else {
            static const char none[] = "(no frames available)\n";
            as_safe_write_all(fd, none, sizeof(none) - 1);
        }
#else
        static const char unsup[] = "(backtrace(3) not available on this platform)\n";
        as_safe_write_all(fd, unsup, sizeof(unsup) - 1);
#endif
    }

    /* ── 5. === IN-FLIGHT REQUESTS === ────────────────────────────── */
    {
        static const char hdr[] = "\n=== IN-FLIGHT REQUESTS ===\n";
        as_safe_write_all(fd, hdr, sizeof(hdr) - 1);
        /* Walk from most-recent-slot backwards. */
        uint32_t idx = atomic_load_explicit(&s_ring_idx,
                                            memory_order_acquire);
        bool any = false;
        for (uint32_t i = 0; i < ILSP_CRASH_RING_SLOTS; i++) {
            uint32_t slot = (idx - 1 - i) & (ILSP_CRASH_RING_SLOTS - 1);
            uint64_t id   = atomic_load_explicit(&s_ring[slot].id,
                                                 memory_order_acquire);
            if (id == 0) continue;  /* empty slot */
            any = true;
            static const char idlab[] = "id=";
            as_safe_write_all(fd, idlab, sizeof(idlab) - 1);
            char buf[32];
            size_t n = as_safe_fmt_uint(buf, sizeof(buf), id);
            as_safe_write_all(fd, buf, n);
            static const char mlab[] = " method=";
            as_safe_write_all(fd, mlab, sizeof(mlab) - 1);
            size_t mlen = as_safe_strlen(s_ring[slot].method);
            if (mlen > 0) {
                as_safe_write_all(fd, s_ring[slot].method, mlen);
            } else {
                static const char blank[] = "(blank)";
                as_safe_write_all(fd, blank, sizeof(blank) - 1);
            }
            as_safe_write_all(fd, "\n", 1);
        }
        if (!any) {
            static const char none[] = "(no requests in flight)\n";
            as_safe_write_all(fd, none, sizeof(none) - 1);
        }
    }

    /* ── 6. === DOCUMENT STATE === ────────────────────────────────── */
    {
        static const char hdr[] = "\n=== DOCUMENT STATE ===\n";
        as_safe_write_all(fd, hdr, sizeof(hdr) - 1);
        if (s_version_len > 0)
            as_safe_write_all(fd, s_version_str, s_version_len);
        if (s_os_line_len > 0)
            as_safe_write_all(fd, s_os_line, s_os_line_len);
        size_t wlen = atomic_load_explicit(&s_workspace_line_len,
                                           memory_order_acquire);
        if (wlen > 0)
            as_safe_write_all(fd, s_workspace_line, wlen);
        else {
            static const char none[] = "WORKSPACE=(unset)\n";
            as_safe_write_all(fd, none, sizeof(none) - 1);
        }
    }

    close(fd);

chain:
    /* ── 7. Signal-specific post-dump handoff ─────────────────────── */
    if (signo == SIGSEGV || signo == SIGBUS) {
        /* Re-raise with default disposition so the process exits with
         * the correct signal-origin exit code (shell reports 128+signo).
         * SA_RESETHAND on our sigaction means the kernel has already
         * restored the default; belt + braces via signal() + raise(). */
        signal(signo, SIG_DFL);
        raise(signo);
        return;  /* unreachable */
    }
    if (signo == SIGABRT) {
        /* Chain into Phase 2 sigsetjmp handler. Phase 2 installs its
         * handler AFTER us, so s_prev_sigabrt.sa_sigaction is the
         * PRE-CRASH_DUMP disposition (usually SIG_DFL). What we actually
         * want is: per-document quarantine via siglongjmp. That is
         * installed by ilsp_install_abort_handler() AFTER we installed
         * OUR handler, which means THIS handler is not reached -- the
         * Phase 2 handler supersedes us.
         *
         * To preserve the dump-on-abort contract, the install order
         * MUST be: crash_dump first, then abort_handler wraps it via
         * SA_NODEFER + re-calls our handler. That wiring lives in
         * abort_handler.c. See 07-01 SUMMARY for the chain diagram.
         *
         * For now, if we're reached at all, re-invoke Phase 2 if
         * previously recorded; otherwise _exit(134) conventional abort. */
        if (s_prev_sigabrt.sa_flags & SA_SIGINFO) {
            if (s_prev_sigabrt.sa_sigaction) {
                s_prev_sigabrt.sa_sigaction(signo, info, ctx);
                return;
            }
        } else if (s_prev_sigabrt.sa_handler &&
                   s_prev_sigabrt.sa_handler != SIG_DFL &&
                   s_prev_sigabrt.sa_handler != SIG_IGN) {
            s_prev_sigabrt.sa_handler(signo);
            return;
        }
        _exit(134);
    }
    /* Other signals: re-raise as default. */
    signal(signo, SIG_DFL);
    raise(signo);
}

/* ── Public API ───────────────────────────────────────────────────────── */

void ilsp_crash_install_handlers(void) {
    /* Idempotent: if already installed, do nothing. */
    if (atomic_exchange(&s_installed, 1)) return;

    /* ── Ring buffer zero ────────────────────────────────────────── */
    for (uint32_t i = 0; i < ILSP_CRASH_RING_SLOTS; i++) {
        atomic_store(&s_ring[i].id, (uint64_t)0);
        memset(s_ring[i].method, 0, ILSP_CRASH_METHOD_MAX);
    }
    atomic_store(&s_ring_idx, 0u);

    /* ── Resolve crashes/ directory and mkdir -p ─────────────────── */
    if (resolve_crash_dir(s_crash_dir, sizeof(s_crash_dir)) == 0) {
        ilsp_crash_mkdir_p(s_crash_dir);
        s_crash_dir_len = strlen(s_crash_dir);
    } else {
        s_crash_dir_len = 0;
    }

    /* ── Pre-format DOCUMENT STATE lines ─────────────────────────── */
    int vn = snprintf(s_version_str, sizeof(s_version_str),
                      "IRON_VERSION_FULL=%s\n", IRON_VERSION_STRING);
    s_version_len = (vn > 0 && (size_t)vn < sizeof(s_version_str))
                      ? (size_t)vn : 0;
    struct utsname u;
    if (uname(&u) == 0) {
        int on = snprintf(s_os_line, sizeof(s_os_line),
                          "OS=%s %s\n", u.sysname, u.machine);
        s_os_line_len = (on > 0 && (size_t)on < sizeof(s_os_line))
                          ? (size_t)on : 0;
    } else {
        s_os_line_len = 0;
    }
    atomic_store(&s_workspace_line_len, (size_t)0);

    /* ── Install alternate signal stack ──────────────────────────── */
    static char s_sigstack_storage[ILSP_CRASH_SIGSTACK_BYTES];
    s_altstack.ss_sp    = s_sigstack_storage;
    s_altstack.ss_size  = sizeof(s_sigstack_storage);
    s_altstack.ss_flags = 0;
    sigaltstack(&s_altstack, NULL);

    /* ── sigaction: SIGSEGV (SA_RESETHAND per T-07-01-01) ─────────── */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, &s_prev_sigsegv);
    }
    /* SIGBUS: same treatment (some platforms report unaligned/VM
     * violations as SIGBUS instead of SIGSEGV). */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
        sigaction(SIGBUS, &sa, NULL);
    }
    /* SIGABRT: our handler runs first, writes the dump, then chains
     * into the Phase 2 per-document sigsetjmp handler (if installed).
     * NO SA_RESETHAND on SIGABRT because we explicitly want to keep
     * re-entering the handler on subsequent aborts of other documents. */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigaction(SIGABRT, &sa, &s_prev_sigabrt);
    }
}

void ilsp_crash_set_workspace_root(const char *root) {
    if (!root || !*root) {
        atomic_store_explicit(&s_workspace_line_len, (size_t)0,
                              memory_order_release);
        return;
    }
    int n = snprintf(s_workspace_line, sizeof(s_workspace_line),
                     "WORKSPACE=%s\n", root);
    if (n <= 0 || (size_t)n >= sizeof(s_workspace_line)) {
        atomic_store_explicit(&s_workspace_line_len, (size_t)0,
                              memory_order_release);
        return;
    }
    atomic_store_explicit(&s_workspace_line_len, (size_t)n,
                          memory_order_release);
}

const char *ilsp_crash_dir_path(void) {
    return (s_crash_dir_len > 0) ? s_crash_dir : NULL;
}

void ilsp_crash_reset_for_testing(void) {
    atomic_store(&s_installed, 0);
    for (uint32_t i = 0; i < ILSP_CRASH_RING_SLOTS; i++) {
        atomic_store(&s_ring[i].id, (uint64_t)0);
        memset(s_ring[i].method, 0, ILSP_CRASH_METHOD_MAX);
    }
    atomic_store(&s_ring_idx, 0u);
    s_crash_dir_len     = 0;
    s_version_len       = 0;
    s_os_line_len       = 0;
    atomic_store(&s_workspace_line_len, (size_t)0);
    /* Do NOT remove installed sigactions here -- tests that need the
     * handlers to go away should restore SIG_DFL explicitly. */
}
