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

/* ── Log level constants (Iron_Log_DEBUG etc.) ───────────────────────────── */
/* These #defines map the compiler-emitted Iron_Log_XXX identifiers (produced
 * by Iron code `Log.DEBUG`, `Log.INFO`, etc.) to the corresponding enum values.
 * The Iron codegen emits type-name field accesses as Iron_TypeName_FieldName
 * (e.g. Iron_Log_DEBUG) rather than as a struct member access. */
#define Iron_Log_DEBUG ((int64_t)IRON_LOG_DEBUG)
#define Iron_Log_INFO  ((int64_t)IRON_LOG_INFO)
#define Iron_Log_WARN  ((int64_t)IRON_LOG_WARN)
#define Iron_Log_ERROR ((int64_t)IRON_LOG_ERROR)

#endif /* IRON_LOG_H */
