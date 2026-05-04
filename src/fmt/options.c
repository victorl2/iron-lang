#include "fmt/options.h"

/* Phase 5 Plan 05-01 (D-02, FMT-05) defaults.
 *
 * IMPORTANT: indent_width = 2, NOT 4 (overrides CONTEXT.md D-02).
 * Verified via RESEARCH Assumption A1: src/parser/printer.c:20-24
 * literally emits "  " (2 spaces) per indentation level. Default = 4
 * would churn every integration golden. Users opt into 4-space
 * indent via iron.toml [fmt].indent_width = 4. */
IronFmtOptions iron_fmt_options_default(void) {
    IronFmtOptions o;
    o.line_width   = 100;
    o.indent_width = 2;    /* NOT 4 -- see options.h comment + RESEARCH A1 */
    o.use_tabs     = false;
    return o;
}
