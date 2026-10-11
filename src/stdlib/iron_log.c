#include "iron_log.h"
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>
#include "util/os.h"

/* ── Internal state ──────────────────────────────────────────────────────── */

/* An atomic: tasks may log while another changes the level. Kept as a
 * 64-bit Int: set_level(-5) was converted to the (unsigned) enum and hid
 * every message instead of showing them all (#625). */
static _Atomic int64_t s_log_level = IRON_LOG_INFO;

/* ANSI color codes — only emitted when stderr is a terminal */
#define ANSI_RESET  "\x1b[0m"
#define ANSI_GRAY   "\x1b[90m"
#define ANSI_GREEN  "\x1b[32m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_RED    "\x1b[31m"

void Iron_log_set_level(int64_t level) {
    atomic_store_explicit(&s_log_level, level, memory_order_relaxed);
}

/* ── Internal logging helper ─────────────────────────────────────────────── */

static void iron_log_emit(Iron_LogLevel level, const char *level_str,
                          const char *color, Iron_String msg) {
    if ((int64_t)level < atomic_load_explicit(&s_log_level, memory_order_relaxed)) return;

    /* Format timestamp from wall-clock time */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    bool use_color = isatty(STDERR_FILENO);
    const char *reset = use_color ? ANSI_RESET : "";
    const char *col   = use_color ? color      : "";

    /* The message is written as its bytes (a zero byte does not end it,
     * #625), in one locked write so lines from several tasks never mix. */
    const char *text = iron_string_cstr(&msg);
    size_t text_len = iron_string_byte_len(&msg);
#ifdef _WIN32
    _lock_file(stderr);
#else
    flockfile(stderr);
#endif
    fprintf(stderr, "%s[%s] [%s]%s ", col, ts, level_str, reset);
    fwrite(text, 1, text_len, stderr);
    fputc('\n', stderr);
#ifdef _WIN32
    _unlock_file(stderr);
#else
    funlockfile(stderr);
#endif
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void Iron_log_debug(Iron_String msg) {
    iron_log_emit(IRON_LOG_DEBUG, "DEBUG", ANSI_GRAY, msg);
}

void Iron_log_info(Iron_String msg) {
    iron_log_emit(IRON_LOG_INFO, "INFO", ANSI_GREEN, msg);
}

void Iron_log_warn(Iron_String msg) {
    iron_log_emit(IRON_LOG_WARN, "WARN", ANSI_YELLOW, msg);
}

void Iron_log_error(Iron_String msg) {
    iron_log_emit(IRON_LOG_ERROR, "ERROR", ANSI_RED, msg);
}
