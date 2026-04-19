#ifndef IRON_PRINTER_H
#define IRON_PRINTER_H

#include "parser/ast.h"
#include "util/arena.h"
#include "fmt/options.h"
#include <stdio.h>

/* ── AST pretty-printer ──────────────────────────────────────────────────── */

/* Phase 5 Plan 05-01 (D-01, FMT-01): iron_print_ast gains
 * `const IronFmtOptions *opts`. opts == NULL is interpreted as
 * iron_fmt_options_default() (indent_width=2, use_tabs=false), which
 * is byte-identical to the pre-Phase-5 behavior (hardcoded "  " indent). */

/* Pretty-print the AST rooted at `root` and return an arena-allocated string
 * of readable Iron source code.
 * The returned pointer is valid for the lifetime of `arena`.
 * Never returns NULL — returns an empty string on empty input. */
char *iron_print_ast(Iron_Node *root, const IronFmtOptions *opts, Iron_Arena *arena);

/* Pretty-print the AST rooted at `root` to the given file handle.
 * Useful for debugging; use stdout or stderr as `out`. */
void iron_print_ast_to_file(Iron_Node *root, const IronFmtOptions *opts, FILE *out);

#endif /* IRON_PRINTER_H */
