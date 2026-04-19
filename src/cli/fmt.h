#ifndef IRON_CLI_FMT_H
#define IRON_CLI_FMT_H

/* Format a .iron source file in-place.
 * Parses the file, pretty-prints the AST, and atomically replaces the
 * original file.  Refuses to overwrite if the file has syntax errors.
 * Returns 0 on success, non-zero on error. */
int iron_fmt(const char *source_path);

/* Phase 5 Plan 05-01 (D-15): non-mutating check mode used by CI / pre-commit
 * hooks. Reads the file but never writes. Exit codes:
 *   0  -- file is already iron-fmt-clean (no diff)
 *   1  -- file would be reformatted; emits "would reformat <path>" to stderr
 *   2  -- file has lexer/parser errors (refused; diags printed)
 *   3  -- I/O error (cannot open / read the file) */
int iron_fmt_check(const char *source_path);

#endif /* IRON_CLI_FMT_H */
