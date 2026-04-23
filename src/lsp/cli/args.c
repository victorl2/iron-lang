/* Phase 7 Plan 07-01 Task 02 -- argv parser.
 *
 * Shared between supervisor (parent) and worker (child) invocations.
 * The `--__worker` flag is INTERNAL: it has two leading underscores so
 * it cannot be confused with user-facing --worker-style options and it
 * is intentionally omitted from any --help output the server prints. */

#include "lsp/cli/args.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static IlspArgsLogLevel parse_level(const char *s) {
    if (!s || !*s) return ILSP_ARGS_LOG_UNSET;
    if (strcmp(s, "ERROR") == 0 || strcmp(s, "error") == 0) return ILSP_ARGS_LOG_ERROR;
    if (strcmp(s, "WARN")  == 0 || strcmp(s, "warn")  == 0) return ILSP_ARGS_LOG_WARN;
    if (strcmp(s, "INFO")  == 0 || strcmp(s, "info")  == 0) return ILSP_ARGS_LOG_INFO;
    if (strcmp(s, "DEBUG") == 0 || strcmp(s, "debug") == 0) return ILSP_ARGS_LOG_DEBUG;
    return ILSP_ARGS_LOG_UNSET;
}

/* Parse "--rss-cap=<bytes>". Returns 0 on valid value written to *out,
 * -1 on parse failure. Values below 65536 or >= UINT64_MAX are rejected
 * per plan. `0` is permitted and means "disable the cap". */
static int parse_rss_cap(const char *s, uint64_t *out) {
    if (!s || !*s) return -1;
    char *endp = NULL;
    unsigned long long v = strtoull(s, &endp, 10);
    if (!endp || *endp != '\0') return -1;
    if (v == 0ULL) {
        *out = 0ULL;
        return 0;
    }
    if (v < 65536ULL) return -1;
    if (v >= (unsigned long long)UINT64_MAX) return -1;
    *out = (uint64_t)v;
    return 0;
}

IlspArgs ilsp_args_parse(int argc, char **argv) {
    IlspArgs a;
    memset(&a, 0, sizeof(a));
    a.mode      = ILSP_MODE_NORMAL;
    a.log_level = ILSP_ARGS_LOG_UNSET;

    for (int i = 1; i < argc; i++) {
        const char *ai = argv[i];
        if (strcmp(ai, "--version") == 0 || strcmp(ai, "-v") == 0) {
            a.want_version = true;
            continue;
        }
        if (strcmp(ai, "--supervised") == 0) {
            a.mode = ILSP_MODE_SUPERVISED_PARENT;
            continue;
        }
        if (strcmp(ai, "--__worker") == 0) {
            /* Internal flag: only meaningful when the supervisor parent
             * has forked us with it; user-side forgery is harmless
             * (we just skip the supervisor layer and run as normal,
             * same as ILSP_MODE_NORMAL). Recorded as SUPERVISED_WORKER
             * for diagnostic clarity. */
            a.mode = ILSP_MODE_SUPERVISED_WORKER;
            continue;
        }
        if (strncmp(ai, "--log-dir=", 10) == 0) {
            a.log_dir = ai + 10;
            continue;
        }
        if (strncmp(ai, "--log-level=", 12) == 0) {
            IlspArgsLogLevel l = parse_level(ai + 12);
            if (l != ILSP_ARGS_LOG_UNSET) a.log_level = l;
            continue;
        }
        if (strncmp(ai, "--rss-cap=", 10) == 0) {
            uint64_t v = 0;
            if (parse_rss_cap(ai + 10, &v) == 0) {
                a.rss_cap_bytes    = v;
                a.rss_cap_explicit = true;
            } else {
                fprintf(stderr,
                        "ironls: --rss-cap=<bytes> invalid: '%s' (must be 0 or >= 65536)\n",
                        ai + 10);
            }
            continue;
        }
        /* Unknown -- tolerate. */
    }

    return a;
}
