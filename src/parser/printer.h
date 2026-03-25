#ifndef IRON_PRINTER_H
#define IRON_PRINTER_H

#include "parser/ast.h"
#include "util/arena.h"
#include <stdio.h>

/* ── AST pretty-printer ──────────────────────────────────────────────────── */

/* Pretty-print the AST rooted at `root` and return an arena-allocated string
 * of readable Iron source code.
 * The returned pointer is valid for the lifetime of `arena`.
 * Never returns NULL — returns an empty string on empty input. */
char *iron_print_ast(Iron_Node *root, Iron_Arena *arena);

/* Pretty-print the AST rooted at `root` to the given file handle.
 * Useful for debugging; use stdout or stderr as `out`. */
void iron_print_ast_to_file(Iron_Node *root, FILE *out);

#endif /* IRON_PRINTER_H */
