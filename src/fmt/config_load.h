#ifndef IRON_FMT_CONFIG_LOAD_H
#define IRON_FMT_CONFIG_LOAD_H

#include "fmt/options.h"
#include "cli/toml.h"   /* IronProject typedef (anonymous struct) */

/* Phase 5 Plan 05-01 (D-02, FMT-05): overlay IronProject.fmt (populated
 * by src/cli/toml.c section=4 branch) onto the defaults getter. Returns
 * defaults if proj is NULL or if the [fmt] section was absent.
 *
 * Note: IronProject in src/cli/toml.h is `typedef struct { ... } IronProject;`
 * (anonymous struct + typedef alias), so we cannot forward-declare via
 * `struct IronProject;`. The full header include is required. */
IronFmtOptions iron_fmt_options_from_toml(const IronProject *proj);

#endif /* IRON_FMT_CONFIG_LOAD_H */
