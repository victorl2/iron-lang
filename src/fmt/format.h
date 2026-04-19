#ifndef IRON_FMT_FORMAT_H
#define IRON_FMT_FORMAT_H

#include <stdbool.h>
#include <stddef.h>

#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "fmt/options.h"

/* Phase 5 Plan 05-01 (D-01, FMT-01): public library entry for Iron
 * source formatting. Reused byte-for-byte by `iron fmt` CLI and by
 * the LSP facade (`src/lsp/facade/fmt/format.c`). No subprocess
 * shell-out from the LSP -- this is the whole point.
 *
 * On parse errors, returns { formatted="", ok=false, error_count=N }
 * per D-03 refusal policy. Caller decides how to surface (CLI prints
 * diags; LSP returns empty TextEdit[] + window/logMessage Info).
 *
 * Caller owns `arena` + `diags` per HARD-06 arena discipline. */
typedef struct IronFmtResult {
    const char *formatted;      /* arena-allocated; "" on refuse, never NULL on ok=true */
    size_t      formatted_len;
    bool        ok;
    int         error_count;
} IronFmtResult;

/* Format `source` (UTF-8 Iron text). Returns the formatted source as an
 * arena-allocated string when ok=true. opts may be NULL (uses defaults).
 * Empty source returns ok=true, formatted_len=0, formatted="". */
IronFmtResult iron_format_source(const char           *source,
                                 const char           *filename,
                                 const IronFmtOptions *opts,   /* NULL = defaults */
                                 Iron_Arena           *arena,
                                 Iron_DiagList        *diags);

#endif /* IRON_FMT_FORMAT_H */
