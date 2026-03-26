#ifndef IRON_CLI_CHECK_H
#define IRON_CLI_CHECK_H

#include <stdbool.h>

/* Type-check a .iron source file without invoking clang.
 * Runs: lex -> parse -> analyze, then prints diagnostics.
 * Returns 0 if no errors, 1 if errors found.
 */
int iron_check(const char *source_path, bool verbose);

#endif /* IRON_CLI_CHECK_H */
