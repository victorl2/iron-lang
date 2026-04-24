/* Phase 7 Plan 07-02 Task 01 (HARD-15, D-03) -- RSS measurement + cap.
 *
 * See rss.h for the design rationale. This TU implements:
 *
 *   - ilsp_rss_current_bytes(): Linux procfs / macOS task_info abstraction
 *   - ilsp_rss_cap_init():       background 5s sampler thread
 *   - Exit-42 trip path:         window/logMessage ERROR (synchronous
 *                                framed write to stdout) + marker dump +
 *                                _exit(42)
 *
 * Critical ordering inside the trip path (T-07-02-05):
 *   1. Build the LSP framed logMessage into a stack buffer and write
 *      it directly with write(2) to fd 1, then fsync-ish (write+flush
 *      via fprintf does NOT go through us -- see note below).
 *      Rationale: the async writer queue (src/lsp/transport/writer.c)
 *      owns fd 1 via a FILE*. After we _exit(42), the writer thread
 *      never wakes again and its in-flight items are lost. We bypass
 *      the queue and write directly. There is a narrow race where the
 *      writer thread interleaves bytes with ours (libc stdio + raw
 *      write(2) on the same fd); that is accepted per D-03 -- the
 *      marker file holds the canonical record if the editor misses
 *      the logMessage.
 *   2. Write the marker dump to
 *      $XDG_STATE_HOME/iron-lsp/rss-restart-<ISO8601>.log.
 *   3. _exit(42). */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "lsp/obs/rss.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/task.h>
#include <mach/task_info.h>
#endif

/* ── Tunables (plan-mandated literals) ──────────────────────────────── */

#define ILSP_RSS_SAMPLE_INTERVAL_SEC   5
#define ILSP_RSS_RESTART_EXIT_CODE     42
#define ILSP_RSS_DEFAULT_CAP_BYTES     (1024ULL * 1024ULL * 1024ULL)  /* 1 GiB */
#define ILSP_RSS_MARKER_PATH_MAX       4096u

/* LSP MessageType.Error = 1 (3.17 spec). */
#define ILSP_MSG_TYPE_ERROR            1

/* ── Module state ──────────────────────────────────────────────────── */

static _Atomic uint64_t s_cap_bytes        = 0;
static _Atomic int      s_installed        = 0;
static _Atomic int      s_thread_running   = 0;
static pthread_t        s_sampler_thread;

/* Test-only override: when non-zero, ilsp_rss_current_bytes returns
 * this value instead of probing the OS. */
static _Atomic uint64_t s_rss_override     = 0;

/* Test-only override: when non-NULL, the Linux procfs parser consumes
 * this buffer instead of reading /proc/self/status. Read-only pointer;
 * lifetime managed by the caller (tests). */
static const char * _Atomic s_procfs_override = NULL;

/* ── Platform probes ──────────────────────────────────────────────── */

/* Parse a VmRSS line from a procfs-style status buffer. Returns the
 * RSS in bytes, or 0 if no VmRSS line is present or parsing fails. */
static uint64_t parse_vmrss_from_status(const char *status) {
    if (!status) return 0;
    const char *p = status;
    while (*p) {
        /* Find start of a line matching "VmRSS:". */
        if (strncmp(p, "VmRSS:", 6) == 0) {
            const char *q = p + 6;
            /* Skip whitespace (spaces + tabs). */
            while (*q == ' ' || *q == '\t') q++;
            /* Read a decimal integer (kibibytes). */
            uint64_t kb = 0;
            bool any   = false;
            while (*q >= '0' && *q <= '9') {
                kb = kb * 10u + (uint64_t)(*q - '0');
                any = true;
                q++;
            }
            if (!any) return 0;
            /* Skip whitespace + optional "kB" suffix. */
            while (*q == ' ' || *q == '\t') q++;
            /* proc(5) documents kB as the unit; we accept any suffix
             * (or none) and treat the integer as kibibytes. */
            return kb * 1024ULL;
        }
        /* Advance to next line. */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return 0;
}

#if defined(__linux__)
/* Read /proc/self/status into a stack buffer; return bytes read or -1. */
static ssize_t read_proc_self_status(char *buf, size_t cap) {
    int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t r = read(fd, buf + total, cap - 1 - total);
        if (r < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    close(fd);
    buf[total] = '\0';
    return (ssize_t)total;
}
#endif

static uint64_t rss_probe_platform(void) {
#if defined(__linux__)
    char buf[4096];
    if (read_proc_self_status(buf, sizeof(buf)) < 0) return 0;
    return parse_vmrss_from_status(buf);
#elif defined(__APPLE__)
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO,
                  (task_info_t)&info, &count) != KERN_SUCCESS) {
        return 0;
    }
    return (uint64_t)info.resident_size;
#else
    return 0;
#endif
}

uint64_t ilsp_rss_current_bytes(void) {
    uint64_t ov = atomic_load_explicit(&s_rss_override,
                                        memory_order_acquire);
    if (ov != 0) return ov;
    const char *procfs_ov =
        atomic_load_explicit(&s_procfs_override, memory_order_acquire);
    if (procfs_ov) return parse_vmrss_from_status(procfs_ov);
    return rss_probe_platform();
}

bool ilsp_rss_cap_installed(void) {
    return atomic_load_explicit(&s_installed, memory_order_acquire) != 0;
}

uint64_t ilsp_rss_cap_bytes(void) {
    return atomic_load_explicit(&s_cap_bytes, memory_order_acquire);
}

void ilsp_rss_reset_for_testing(void) {
    atomic_store_explicit(&s_installed,      0,      memory_order_release);
    atomic_store_explicit(&s_cap_bytes,      0ULL,   memory_order_release);
    atomic_store_explicit(&s_thread_running, 0,      memory_order_release);
}

void ilsp_rss_set_override_for_testing(uint64_t bytes) {
    atomic_store_explicit(&s_rss_override, bytes, memory_order_release);
}

void ilsp_rss_set_procfs_override_for_testing(const char *status_text) {
    atomic_store_explicit(&s_procfs_override, status_text,
                          memory_order_release);
}

/* ── Marker-file + logMessage helpers ─────────────────────────────── */

/* Build an ISO8601 UTC timestamp "YYYY-MM-DDTHHMMSSZ" (17 chars + NUL). */
static void iso8601_now(char *out, size_t cap) {
    if (cap < 18) { if (cap > 0) out[0] = '\0'; return; }
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        snprintf(out, cap, "x");
        return;
    }
    struct tm tmv;
    if (gmtime_r(&ts.tv_sec, &tmv) == NULL) {
        snprintf(out, cap, "%lld", (long long)ts.tv_sec);
        return;
    }
    snprintf(out, cap, "%04d-%02d-%02dT%02d%02d%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* Resolve $XDG_STATE_HOME/iron-lsp (for marker files). Same discipline
 * as src/lsp/obs/log.c + src/lsp/obs/crash_dump.c. */
static int resolve_marker_dir(char *out, size_t cap) {
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] != '\0') {
        int n = snprintf(out, cap, "%s/iron-lsp", xdg);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        int n = snprintf(out, cap, "%s/.local/state/iron-lsp", home);
        return (n > 0 && (size_t)n < cap) ? 0 : -1;
    }
    int n = snprintf(out, cap, "/tmp/iron-lsp");
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* Recursive mkdir -p. Best-effort; errors are ignored -- caller checks
 * open() below for the real failure signal. */
static void mkdir_p(const char *path) {
    char tmp[ILSP_RSS_MARKER_PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return;
    memcpy(tmp, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Append `n` bytes of `buf` to `fd`, retrying on EINTR. */
static void write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (w == 0) return;
        buf += (size_t)w;
        n   -= (size_t)w;
    }
}

/* Write the marker dump. Returns 0 on success, -1 on failure (best-
 * effort; a failure here does NOT abort the exit-42 path). */
static int write_marker_file(uint64_t current_bytes, uint64_t cap_bytes) {
    char dir[ILSP_RSS_MARKER_PATH_MAX];
    if (resolve_marker_dir(dir, sizeof(dir)) != 0) return -1;
    mkdir_p(dir);

    char ts[32];
    iso8601_now(ts, sizeof(ts));

    char path[ILSP_RSS_MARKER_PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/rss-restart-%s.log", dir, ts);
    if (n <= 0 || (size_t)n >= sizeof(path)) return -1;

    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return -1;

    /* Header. */
    {
        char hdr[256];
        int hn = snprintf(hdr, sizeof(hdr),
            "Iron LSP RSS-cap restart marker\n"
            "pid=%lld current_bytes=%llu cap_bytes=%llu timestamp=%s\n\n",
            (long long)getpid(),
            (unsigned long long)current_bytes,
            (unsigned long long)cap_bytes,
            ts);
        if (hn > 0) write_all(fd, hdr, (size_t)hn);
    }

#if defined(__linux__)
    /* Snapshot /proc/self/status for operator forensics. */
    {
        static const char section[] = "=== /proc/self/status ===\n";
        write_all(fd, section, sizeof(section) - 1);
        char buf[8192];
        ssize_t got = read_proc_self_status(buf, sizeof(buf));
        if (got > 0) {
            write_all(fd, buf, (size_t)got);
        } else {
            static const char none[] = "(unable to read /proc/self/status)\n";
            write_all(fd, none, sizeof(none) - 1);
        }
    }
#elif defined(__APPLE__)
    {
        static const char section[] = "=== task_info(TASK_BASIC_INFO) ===\n";
        write_all(fd, section, sizeof(section) - 1);
        struct task_basic_info info;
        mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_BASIC_INFO,
                      (task_info_t)&info, &count) == KERN_SUCCESS) {
            char buf[256];
            int bn = snprintf(buf, sizeof(buf),
                "resident_size=%llu\nvirtual_size=%llu\n",
                (unsigned long long)info.resident_size,
                (unsigned long long)info.virtual_size);
            if (bn > 0) write_all(fd, buf, (size_t)bn);
        } else {
            static const char none[] = "(task_info failed)\n";
            write_all(fd, none, sizeof(none) - 1);
        }
    }
#else
    {
        static const char none[] = "(platform has no RSS probe)\n";
        write_all(fd, none, sizeof(none) - 1);
    }
#endif

    /* Best-effort last-N requests: the 07-01 crash ring buffer's slot
     * data is file-scope in crash_dump.c, which means we cannot walk
     * it from this TU without breaking the encapsulation contract
     * (crash_dump.h deliberately does not expose read access). The
     * canonical forensic record lives in the SIGSEGV .dmp produced
     * at the subsequent crash if the editor re-launches ironls and
     * the cap trips again -- the marker here serves as the "why did
     * ironls exit 42" breadcrumb per D-03. */
    {
        static const char section[] = "\n=== IN-FLIGHT REQUESTS ===\n"
                                       "(see most recent crash dump under "
                                       "$XDG_STATE_HOME/iron-lsp/crashes/)\n";
        write_all(fd, section, sizeof(section) - 1);
    }

    close(fd);
    return 0;
}

/* Build and synchronously emit a framed window/logMessage notification
 * to stdout. Deliberately DOES NOT go through the async writer queue:
 * we're about to _exit(42), and any items enqueued after us will be
 * lost with the writer thread's memory. */
static void emit_sync_log_message(uint64_t current_bytes, uint64_t cap_bytes) {
    const uint64_t MIB = 1024ULL * 1024ULL;
    uint64_t cur_mib = current_bytes / MIB;
    uint64_t cap_mib = cap_bytes     / MIB;

    char body[256];
    int bn = snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\","
        "\"params\":{\"type\":%d,\"message\":"
        "\"RSS cap exceeded: current=%lluMiB cap=%lluMiB. Restarting.\"}}",
        ILSP_MSG_TYPE_ERROR,
        (unsigned long long)cur_mib,
        (unsigned long long)cap_mib);
    if (bn <= 0) return;
    size_t body_len = (size_t)bn;

    char header[96];
    int hn = snprintf(header, sizeof(header),
                      "Content-Length: %zu\r\n\r\n", body_len);
    if (hn <= 0) return;

    /* fflush stdout first so any buffered bytes from the writer's
     * FILE* don't interleave inside our frame. Then write our frame
     * via the raw fd. (Writer's FILE* is line-buffered by libc default
     * when piped; explicit flush drains it.) */
    fflush(stdout);
    write_all(STDOUT_FILENO, header, (size_t)hn);
    write_all(STDOUT_FILENO, body,   body_len);
}

/* Trip the cap: log, dump marker, _exit(42) (or, in test mode, set
 * *out_tripped and return). */
static void trip_cap(uint64_t current_bytes, uint64_t cap_bytes,
                     bool *test_out_tripped) {
    emit_sync_log_message(current_bytes, cap_bytes);
    write_marker_file(current_bytes, cap_bytes);
    if (test_out_tripped) {
        *test_out_tripped = true;
        return;
    }
    _exit(ILSP_RSS_RESTART_EXIT_CODE);
}

/* ── Public sync-check hook (tests) ───────────────────────────────── */

int ilsp_rss_sample_and_check_for_testing(bool *out_tripped) {
    if (out_tripped) *out_tripped = false;
    uint64_t cap = atomic_load_explicit(&s_cap_bytes, memory_order_acquire);
    if (cap == 0) return 0;
    uint64_t cur = ilsp_rss_current_bytes();
    if (cur == 0) return 0;  /* unknown -> never trip */
    if (cur <= cap) return 0;
    trip_cap(cur, cap, out_tripped);
    return 1;
}

/* ── Background sampler thread ────────────────────────────────────── */

static void *sampler_main(void *arg) {
    (void)arg;
    /* Mark the thread live so introspection can report it. */
    atomic_store_explicit(&s_thread_running, 1, memory_order_release);

    for (;;) {
        /* Sleep first so the server has a chance to finish its own
         * startup allocations before we sample. */
        sleep(ILSP_RSS_SAMPLE_INTERVAL_SEC);

        uint64_t cap = atomic_load_explicit(&s_cap_bytes,
                                             memory_order_acquire);
        if (cap == 0) continue;       /* cap was disabled after install */

        uint64_t cur = ilsp_rss_current_bytes();
        if (cur == 0) continue;       /* RSS unknown -- never trip */

        if (cur > cap) {
            /* Production path: _exit(42) -- no return. */
            trip_cap(cur, cap, NULL);
            /* unreachable */
        }
    }
    /* unreachable */
    return NULL;
}

int ilsp_rss_cap_init(uint64_t cap_bytes) {
    /* Disabled: honour request, do NOT spawn thread. */
    if (cap_bytes == 0) {
        atomic_store_explicit(&s_cap_bytes, 0ULL, memory_order_release);
        atomic_store_explicit(&s_installed, 0,    memory_order_release);
        return 0;
    }

    /* Idempotent: if already installed, just update the cap so a
     * second caller with a different value wins. */
    int already = atomic_exchange_explicit(&s_installed, 1,
                                            memory_order_acq_rel);
    atomic_store_explicit(&s_cap_bytes, cap_bytes, memory_order_release);
    if (already) return 0;

    /* Spawn the daemon sampler thread. Detached so we don't need to
     * join at shutdown (the main thread exits the loop via reader
     * EOF / SIGTERM; the sampler gets killed with the process). */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&s_sampler_thread, &attr, sampler_main, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        atomic_store_explicit(&s_installed, 0, memory_order_release);
        atomic_store_explicit(&s_cap_bytes, 0ULL, memory_order_release);
        return -1;
    }
    return 0;
}
