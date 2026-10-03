#include "fmt/options.h"

/* The formatter's defaults. Four spaces is how every Iron source in the
 * repository is written (the scaffold, the manual, the stdlib, the
 * fixtures); iron.toml's [fmt].indent_width overrides it (issue 241). */
IronFmtOptions iron_fmt_options_default(void) {
    IronFmtOptions o;
    o.line_width   = 100;
    o.indent_width = 4;
    o.use_tabs     = false;
    return o;
}
