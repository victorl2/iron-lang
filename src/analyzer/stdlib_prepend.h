#ifndef IRON_STDLIB_PREPEND_H
#define IRON_STDLIB_PREPEND_H

#include <stdbool.h>

/* The directory holding stdlib/ (from IRON_LIB_DIR, next to the binary, or
 * the source tree in a dev build); malloc'd, NULL with a message when there
 * is none. */
char *iron_stdlib_lib_dir(void);

/* Prepend to *source_io (malloc'd, replaced) the stdlib prelude for the
 * source at source_path: a line marker for the user's file, then every
 * stdlib file it needs under `-- @file:` markers. Returns the number of
 * lines added; the user's first line is that number + 1. */
int iron_stdlib_prepend(char **source_io, const char *source_path, const char *base_dir);

/* The same, for a source that is several files packed under file markers
 * (each giving its length with `@lines:`, see diagnostics/diagnostics.h)
 * when `packed`: the marker for the user's file then says `@packed`, and
 * the lexer honors the markers between those files. iron_stdlib_prepend
 * passes what --packed-source set. */
int iron_stdlib_prepend_packed(char **source_io, const char *source_path,
                               const char *base_dir, bool packed);

#endif /* IRON_STDLIB_PREPEND_H */
