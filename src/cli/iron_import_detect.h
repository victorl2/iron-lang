#ifndef IRON_IMPORT_DETECT_H
#define IRON_IMPORT_DETECT_H

#include <stdbool.h>
#include "util/arena.h"

/**
 * Returns true if `source` contains a real `import <module_name>` statement.
 *
 * Uses the Iron lexer — immune to false positives from `import` inside
 * comments or string literals. Scans for IRON_TOK_IMPORT immediately followed
 * by IRON_TOK_IDENTIFIER whose value equals module_name.
 *
 * @param source      Null-terminated source text (not modified, not freed)
 * @param filename    Source filename for diagnostic spans (may be NULL -> "<import-detect>")
 * @param module_name Module name to search for (e.g. "math", "io", "raylib")
 * @param arena       Caller-owned arena used for token allocation (contents discarded after call)
 * @return            true if a real `import <module_name>` token sequence is present
 */
bool iron_detect_import(const char *source, const char *filename,
                        const char *module_name, Iron_Arena *arena);

#endif /* IRON_IMPORT_DETECT_H */
