/* Phase 2 Plan 06 Task 01 (CORE-21) -- Structured JSON-line log sink.
 *
 * XDG-resolved log path, mutex-guarded fwrite, JSON-line schema:
 *   {"ts":"<ISO-8601 UTC>","pid":<n>,"lvl":"<ERROR|WARN|INFO|DEBUG>",
 *    "event":"<name>","msg":"<free-form>"}
 *
 * Design choices:
 *
 *   - One module-level sink (`s_sink`) guarded by one module-level mutex
 *     (`s_lock`). Initialization happens under `pthread_once` so the
 *     ordering is correct even when two threads try to log before
 *     ilsp_log_open() has been called (both get a no-op). This is the
 *     documented "one acceptable static-mutable-state exception" per
 *     CLAUDE.md, matching src/analyzer/types.c's interned-primitives
 *     pattern and src/runtime/iron_net_init.c's WSAStartup singleton.
 *
 *   - `ilsp_mkdir_p` copied ~verbatim from src/pkg/fetcher.c:31-54 so
 *     the mkdir-semantics and platform ifdefs match the existing
 *     package manager implementation.
 *
 *   - JSON escaping covers the four ASCII bytes that would otherwise
 *     break the line-delimited schema: ", \, \n, \r. Other control
 *     characters are passed through as-is (the log is a diagnostic
 *     channel, not a spec-compliant RFC 8259 stream), but the four
 *     escapes alone are sufficient to keep one-log-per-line + one-JSON-
 *     per-line invariants -- which is all the log grep pipeline depends
 *     on.
 *
 *   - Rotation at 100 MB: when fwrite would push the file past the
 *     threshold, fclose + rename to <path>.1 + open a fresh file under
 *     the same mutex; one-compile's worth of lines may straddle a
 *     rotation but will never interleave within a line.
 *
 *   - Level is initialized from $IRONLS_LOG inside ilsp_log_open (first
 *     time) and can be overridden afterwards via ilsp_log_set_level.
 *     Unknown values default to WARN. */

#include "lsp/obs/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── Module-level state ─────────────────────────────────────────────────── */

#define ILSP_LOG_PATH_MAX   4096
#define ILSP_LOG_ROTATE_BYTES (100u * 1024u * 1024u)  /* 100 MB */

static pthread_once_t    s_once  = PTHREAD_ONCE_INIT;
static pthread_mutex_t   s_lock;
static FILE             *s_sink  = NULL;
static char              s_path[ILSP_LOG_PATH_MAX] = {0};
static size_t            s_bytes_written = 0;
static _Atomic int       s_level = ILSP_LOG_WARN;   /* default threshold */
static bool              s_level_from_env = false;   /* set once from getenv */

static void log_init_once(void) {
    pthread_mutex_init(&s_lock, NULL);
}

/* ── Helpers ────────────────────────────────────────────────────────────── */

/* Recursive mkdir (copy of src/pkg/fetcher.c:31-54 iron_mkdirp). */
static int ilsp_mkdir_p(const char *path) {
    char tmp[ILSP_LOG_PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            tmp[i] = '/';
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
    return 0;
}

/* Resolve the state dir under XDG rules. Writes `out` of size `cap`;
 * returns 0 on success, -1 on failure. `warned_fallback` is set to true
 * when the last-ditch /tmp path is chosen (caller can emit a stderr
 * warning). */
static int resolve_state_dir(char *out, size_t cap, bool *warned_fallback) {
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && xdg[0] != '\0') {
        int n = snprintf(out, cap, "%s/iron-lsp", xdg);
        if (n <= 0 || (size_t)n >= cap) return -1;
        if (warned_fallback) *warned_fallback = false;
        return 0;
    }
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        int n = snprintf(out, cap, "%s/.local/state/iron-lsp", home);
        if (n <= 0 || (size_t)n >= cap) return -1;
        if (warned_fallback) *warned_fallback = false;
        return 0;
    }
    int n = snprintf(out, cap, "/tmp/iron-lsp");
    if (n <= 0 || (size_t)n >= cap) return -1;
    if (warned_fallback) *warned_fallback = true;
    return 0;
}

static IronLsp_LogLevel parse_level(const char *s, IronLsp_LogLevel fallback) {
    if (!s || !*s) return fallback;
    if (strcmp(s, "ERROR") == 0 || strcmp(s, "error") == 0) return ILSP_LOG_ERROR;
    if (strcmp(s, "WARN")  == 0 || strcmp(s, "warn")  == 0) return ILSP_LOG_WARN;
    if (strcmp(s, "INFO")  == 0 || strcmp(s, "info")  == 0) return ILSP_LOG_INFO;
    if (strcmp(s, "DEBUG") == 0 || strcmp(s, "debug") == 0) return ILSP_LOG_DEBUG;
    return fallback;
}

static const char *level_name(IronLsp_LogLevel l) {
    switch (l) {
        case ILSP_LOG_ERROR: return "ERROR";
        case ILSP_LOG_WARN:  return "WARN";
        case ILSP_LOG_INFO:  return "INFO";
        case ILSP_LOG_DEBUG: return "DEBUG";
    }
    return "INFO";
}

/* JSON-escape a single byte into `out` (which must have room for up to
 * 2 bytes plus a terminator). Only the four line-structural escapes are
 * handled; returns the number of bytes written (1 or 2). */
static size_t json_escape_byte(char c, char out[2]) {
    switch (c) {
        case '"':  out[0] = '\\'; out[1] = '"';  return 2;
        case '\\': out[0] = '\\'; out[1] = '\\'; return 2;
        case '\n': out[0] = '\\'; out[1] = 'n';  return 2;
        case '\r': out[0] = '\\'; out[1] = 'r';  return 2;
        default:   out[0] = c;                   return 1;
    }
}

/* Write a JSON-escaped string value to `f` (no surrounding quotes). */
static void fwrite_escaped(FILE *f, const char *s) {
    if (!s) return;
    for (const char *p = s; *p; p++) {
        char buf[2];
        size_t n = json_escape_byte(*p, buf);
        fwrite(buf, 1, n, f);
    }
}

/* Perform rotation (caller holds the lock). Closes the current sink,
 * renames it to <path>.1, opens a fresh file at <path>. Best-effort:
 * on rename failure we simply continue appending to the old file. */
static void rotate_locked(void) {
    if (!s_sink || s_path[0] == '\0') return;
    fflush(s_sink);
    fclose(s_sink);
    s_sink = NULL;

    char rotated[ILSP_LOG_PATH_MAX + 8];
    snprintf(rotated, sizeof(rotated), "%s.1", s_path);
    /* Best effort: old .1 is overwritten. */
    (void)rename(s_path, rotated);

    s_sink = fopen(s_path, "a");
    s_bytes_written = 0;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int ilsp_log_open(const char *override_dir) {
    pthread_once(&s_once, log_init_once);

    char dir[ILSP_LOG_PATH_MAX];
    bool fallback_warn = false;
    if (override_dir && override_dir[0] != '\0') {
        size_t n = strlen(override_dir);
        if (n >= sizeof(dir)) return -1;
        memcpy(dir, override_dir, n + 1);
    } else if (resolve_state_dir(dir, sizeof(dir), &fallback_warn) != 0) {
        return -1;
    }

    if (ilsp_mkdir_p(dir) != 0 && errno != EEXIST) {
        /* mkdir_p fails silently on intermediate EEXIST; a true terminal
         * failure (ENOTDIR, EACCES on last component) lands here. Report
         * to stderr so the operator knows why log disappeared. */
        fprintf(stderr, "ironls: log dir %s unusable (errno=%d)\n",
                dir, errno);
    }

    char path[ILSP_LOG_PATH_MAX];
    int nn = snprintf(path, sizeof(path), "%s/server-%d.log",
                      dir, (int)getpid());
    if (nn <= 0 || (size_t)nn >= sizeof(path)) return -1;

    FILE *f = fopen(path, "a");
    if (!f) {
        fprintf(stderr, "ironls: cannot open log %s (errno=%d)\n",
                path, errno);
        return -1;
    }

    pthread_mutex_lock(&s_lock);
    if (s_sink) {
        fflush(s_sink);
        fclose(s_sink);
    }
    s_sink = f;
    s_bytes_written = 0;
    memcpy(s_path, path, (size_t)nn + 1);

    if (!s_level_from_env) {
        const char *env = getenv("IRONLS_LOG");
        IronLsp_LogLevel lvl = parse_level(env, ILSP_LOG_WARN);
        atomic_store(&s_level, (int)lvl);
        s_level_from_env = true;
    }
    pthread_mutex_unlock(&s_lock);

    if (fallback_warn) {
        fprintf(stderr,
                "ironls: $XDG_STATE_HOME and $HOME unset; logging to %s\n",
                path);
    }
    return 0;
}

void ilsp_log_close(void) {
    pthread_once(&s_once, log_init_once);
    pthread_mutex_lock(&s_lock);
    if (s_sink) {
        fflush(s_sink);
        fclose(s_sink);
        s_sink = NULL;
    }
    s_path[0] = '\0';
    s_bytes_written = 0;
    pthread_mutex_unlock(&s_lock);
}

IronLsp_LogLevel ilsp_log_level(void) {
    return (IronLsp_LogLevel)atomic_load(&s_level);
}

void ilsp_log_set_level(IronLsp_LogLevel lvl) {
    atomic_store(&s_level, (int)lvl);
    s_level_from_env = true;
}

const char *ilsp_log_path(void) {
    pthread_once(&s_once, log_init_once);
    pthread_mutex_lock(&s_lock);
    const char *r = (s_path[0] != '\0') ? s_path : NULL;
    pthread_mutex_unlock(&s_lock);
    return r;
}

void ilsp_log(IronLsp_LogLevel level, const char *event,
              const char *fmt, ...) {
    pthread_once(&s_once, log_init_once);

    IronLsp_LogLevel threshold = (IronLsp_LogLevel)atomic_load(&s_level);
    if ((int)level > (int)threshold) return;

    /* Format the message under the caller's stack so we don't keep the
     * lock any longer than the one-fwrite critical section. */
    char msg[4096];
    if (fmt && *fmt) {
        va_list ap;
        va_start(ap, fmt);
        int mn = vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        if (mn < 0) msg[0] = '\0';
    } else {
        msg[0] = '\0';
    }

    /* ISO-8601 UTC timestamp -- %Y-%m-%dT%H:%M:%SZ. gmtime_r for
     * thread safety. */
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);

    pthread_mutex_lock(&s_lock);
    if (!s_sink) {
        pthread_mutex_unlock(&s_lock);
        return;
    }

    if (s_bytes_written >= ILSP_LOG_ROTATE_BYTES) {
        rotate_locked();
        if (!s_sink) {
            pthread_mutex_unlock(&s_lock);
            return;
        }
    }

    /* Emit one JSON object, terminated with '\n'. */
    long before = ftell(s_sink);
    fprintf(s_sink,
            "{\"ts\":\"%s\",\"pid\":%d,\"lvl\":\"%s\",\"event\":\"",
            ts, (int)getpid(), level_name(level));
    fwrite_escaped(s_sink, event ? event : "");
    fputs("\",\"msg\":\"", s_sink);
    fwrite_escaped(s_sink, msg);
    fputs("\"}\n", s_sink);
    fflush(s_sink);
    long after = ftell(s_sink);
    if (after > before) s_bytes_written += (size_t)(after - before);

    pthread_mutex_unlock(&s_lock);
}
