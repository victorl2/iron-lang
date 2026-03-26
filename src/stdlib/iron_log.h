#ifndef IRON_LOG_H
#define IRON_LOG_H

#include "runtime/iron_runtime.h"

/* ── Log levels ──────────────────────────────────────────────────────────── */
typedef enum {
    IRON_LOG_DEBUG = 0,
    IRON_LOG_INFO  = 1,
    IRON_LOG_WARN  = 2,
    IRON_LOG_ERROR = 3
} Iron_LogLevel;

/* ── Log API ─────────────────────────────────────────────────────────────── */
void Iron_log_set_level(Iron_LogLevel level);
void Iron_log_debug(Iron_String msg);
void Iron_log_info(Iron_String msg);
void Iron_log_warn(Iron_String msg);
void Iron_log_error(Iron_String msg);

#endif /* IRON_LOG_H */
