#ifndef IRON_LSP_OBS_TRACE_H
#define IRON_LSP_OBS_TRACE_H

/* Phase 2 Plan 06 Task 01 -- Per-request timing histogram.
 *
 * ilsp_trace_begin()/ilsp_trace_end() form a timing bracket; the token
 * returned from begin() carries a monotonic-clock start-time and the
 * event name. At end(), the elapsed duration is folded into a
 * stb_ds-keyed histogram (count/sum/min/max per name).
 *
 * ilsp_trace_dump() prints the histogram to `sink` as one line per
 * event in the format:
 *   trace <name> count=N sum_ms=X avg_ms=Y min_ms=Z max_ms=W
 *
 * The module uses clock_gettime(CLOCK_MONOTONIC) for drift-free
 * durations and an internal mutex around the histogram so worker
 * threads can time their compiles concurrently. */

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IronLsp_TraceToken {
    int64_t     start_ns;
    const char *name;
} IronLsp_TraceToken;

IronLsp_TraceToken ilsp_trace_begin(const char *name);
void               ilsp_trace_end  (IronLsp_TraceToken t);
void               ilsp_trace_dump (FILE *sink);
void               ilsp_trace_reset(void);  /* test hook */

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_OBS_TRACE_H */
