#include "iron_log.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* ── Internal state ──────────────────────────────────────────────────────── */

static Iron_LogLevel s_log_level = IRON_LOG_INFO;

/* ANSI color codes — only emitted when stderr is a terminal */
#define ANSI_RESET  "\x1b[0m"
#define ANSI_GRAY   "\x1b[90m"
#define ANSI_GREEN  "\x1b[32m"
#define ANSI_YELLOW "\x1b[33m"
#define ANSI_RED    "\x1b[31m"

void Iron_log_set_level(Iron_LogLevel level) {
    s_log_level = level;
}

/* ── Internal logging helper ─────────────────────────────────────────────── */

static void iron_log_emit(Iron_LogLevel level, const char *level_str,
                          const char *color, Iron_String msg) {
    if (level < s_log_level) return;

    /* Format timestamp from wall-clock time */
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    bool use_color = isatty(STDERR_FILENO);
    const char *reset = use_color ? ANSI_RESET : "";
    const char *col   = use_color ? color      : "";

    const char *text = iron_string_cstr(&msg);

    fprintf(stderr, "%s[%s] [%s]%s %s\n",
            col, ts, level_str, reset, text);
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
