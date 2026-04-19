#ifndef IRON_FMT_OPTIONS_H
#define IRON_FMT_OPTIONS_H

#include <stdbool.h>

/* Phase 5 Plan 05-01 (D-02, FMT-05): formatter options loaded from
 * iron.toml [fmt] section. Both `iron fmt` CLI and `ironls` LSP read
 * the same struct -- single source of truth. */
typedef struct IronFmtOptions {
    int  line_width;    /* default 100. v1 HINT ONLY -- not enforced by
                         * printer. Forward-compat for v1.x soft-wrap.
                         * RESEARCH Pitfall 9 / Section State of the Art. */
    int  indent_width;  /* default 2. Preserves existing iron_print_ast
                         * behavior (src/parser/printer.c:20-24 emits
                         * "  " per level). RESEARCH A1 overrides
                         * CONTEXT.md D-02 default of 4 to avoid
                         * churning tests/integration goldens. */
    bool use_tabs;      /* default false */
} IronFmtOptions;

IronFmtOptions iron_fmt_options_default(void);

#endif /* IRON_FMT_OPTIONS_H */
