#ifndef IRON_STDLIB_PREPEND_H
#define IRON_STDLIB_PREPEND_H

/* The directory holding stdlib/ (from IRON_LIB_DIR, next to the binary, or
 * the source tree in a dev build); malloc'd, NULL with a message when there
 * is none. */
char *iron_stdlib_lib_dir(void);

/* Prepend to *source_io (malloc'd, replaced) the stdlib prelude for the
 * source at source_path: a line marker for the user's file, then every
 * stdlib file it needs under `-- @file:` markers. Returns the number of
 * lines added; the user's first line is that number + 1. */
int iron_stdlib_prepend(char **source_io, const char *source_path, const char *base_dir);

#endif /* IRON_STDLIB_PREPEND_H */
