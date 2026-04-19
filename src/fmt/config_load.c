/* Phase 5 Plan 05-01 (D-02, FMT-05): overlay IronProject.fmt onto
 * defaults. The IronProject.fmt struct is populated by toml.c
 * section==4 branch (added in Task 2 of this plan); this loader is
 * defensive about partial population (each numeric field guarded by
 * > 0 check so calloc-zeroed defaults flow through to the getter). */

#include "fmt/config_load.h"
#include "fmt/options.h"
#include "cli/toml.h"   /* IronProject definition (gains `fmt` field in Task 2) */

IronFmtOptions iron_fmt_options_from_toml(const IronProject *proj) {
    IronFmtOptions opts = iron_fmt_options_default();
    if (!proj) return opts;
    if (proj->fmt.line_width   > 0) opts.line_width   = proj->fmt.line_width;
    if (proj->fmt.indent_width > 0) opts.indent_width = proj->fmt.indent_width;
    opts.use_tabs = proj->fmt.use_tabs;
    return opts;
}
