/* migrate.h -- `iron fmt --migrate-comments`.
 *
 * Iron's one comment form is `//` to the end of the line. While code moves
 * over, the lexer still accepts `--` line comments, `///` documentation
 * lines and the `@fusible` annotation. iron_migrate_comments rewrites them
 * in one source text:
 *
 *   -- text     ->  // text        (`--` becomes `//`; the rest is kept, so
 *                                   trailing comments stay aligned)
 *   /// text    ->  // text        (one slash fewer)
 *   @fusible    ->  //iron:fusible
 *
 * Only comment tokens as the lexer sees them change: a `--` or `///` inside
 * a string literal stays. When the rewritten comment would read
 * differently as `//` (its text starts with '/', or looks like a directive
 * `key:value`), a space goes after `//`, which keeps its documentation text
 * the same.
 *
 * The result is checked before it is returned: it must lex to the same
 * tokens apart from comment spelling, report the same diagnostics, and
 * parse to the same documentation, directives and `fusible` marks. A file
 * that fails the check is left alone (ok = false, error says why). Running
 * the migration twice changes nothing the second time.
 */
#ifndef IRON_FMT_MIGRATE_H
#define IRON_FMT_MIGRATE_H

#include <stdbool.h>
#include <stddef.h>

#include "util/arena.h"

typedef struct {
    bool        ok;        /* false: not migrated, see error */
    const char *error;     /* arena or static; NULL when ok */
    const char *text;      /* arena; the migrated text (the input when unchanged) */
    size_t      len;
    bool        changed;
    int         comments;  /* `--` and `///` comments rewritten */
    int         hints;     /* `@fusible` annotations rewritten */
} IronMigrateResult;

IronMigrateResult iron_migrate_comments(const char *source, size_t len,
                                        const char *filename, Iron_Arena *arena);

#endif /* IRON_FMT_MIGRATE_H */
