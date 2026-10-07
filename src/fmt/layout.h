/* layout.h -- the whitespace formatter behind `iron fmt` and
 * textDocument/formatting.
 *
 * It re-indents each line from the bracket nesting of the code around it,
 * strips trailing whitespace, collapses runs of blank lines and ends the
 * file with one newline. Every token and every comment is kept as written,
 * so formatting cannot change what a program means, and formatting twice
 * gives the same bytes as formatting once.
 *
 * Text inside a string literal is never touched: a line that starts inside
 * a multi-line `"""` string is copied verbatim, and trailing whitespace is
 * kept on a line that ends inside one.
 */
#ifndef IRON_FMT_LAYOUT_H
#define IRON_FMT_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "fmt/options.h"
#include "lexer/lexer.h"
#include "util/arena.h"

/* Formats `src` and returns the text of lines first_line..last_line
 * (1-based, inclusive), each ending in '\n'. first_line == 0 formats the
 * whole file: leading and trailing blank lines are dropped too. Indentation
 * always comes from the whole file. Returns NULL on allocation failure. */
char *iron_fmt_layout(const char *src, const IronFmtOptions *opts,
                      Iron_Arena *arena, uint32_t first_line, uint32_t last_line);

/* Whether `text` lexes to the same tokens as `tokens` (line breaks and the
 * trailing blanks of doc comments aside): the check that a layout pass
 * changed nothing but whitespace. */
bool iron_fmt_same_tokens(const Iron_Token *tokens, int count, const char *text,
                          const char *filename, Iron_Arena *arena);

#endif /* IRON_FMT_LAYOUT_H */
