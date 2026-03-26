#ifndef IRON_CLI_FMT_H
#define IRON_CLI_FMT_H

/* Format a .iron source file in-place.
 * Parses the file, pretty-prints the AST, and atomically replaces the
 * original file.  Refuses to overwrite if the file has syntax errors.
 * Returns 0 on success, non-zero on error. */
int iron_fmt(const char *source_path);

#endif /* IRON_CLI_FMT_H */
