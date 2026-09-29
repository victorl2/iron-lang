#ifndef IRON_HIR_STDLIB_ORIGIN_H
#define IRON_HIR_STDLIB_ORIGIN_H

#include <stdbool.h>

/* Which source files are stdlib wrappers.
 *
 * Stdlib .iron files declare runtime-implemented functions and methods with
 * an empty body (`readonly func sqrt(x: Float) -> Float {}`); lowering
 * treats those as extern stubs whose C body lives in the runtime. The same
 * shape written in user code is an ordinary empty function and must get a
 * real (empty) C body. The driver registers every stdlib file it prepends
 * under the path it writes into the `-- @file:` marker, which is the
 * filename the declarations' spans carry.
 *
 * When a stdlib file is prepended without a marker (its path cannot be
 * quoted), its declarations carry the user file's name, so origin is
 * unknown and every empty body is treated as a stub, as before.
 *
 * `.iron-stub` companion files of library packages are always stub files.
 * Pipelines that prepend no stdlib (the LSP's buffer mode) register
 * nothing, so every empty body they see is user code. */

void iron_stdlib_origin_reset(void);
void iron_stdlib_origin_add(const char *path);
void iron_stdlib_origin_mark_unknown(void);

/* True when a declaration from `filename` with an empty body is a runtime
 * stub rather than a user function. Used by lowering (no C body, no self
 * for methods) and by the missing-return check. */
bool iron_stdlib_origin_is_stub_file(const char *filename);

/* Whether `filename` is a prepended stdlib file: 1 yes, 0 no, -1 unknown
 * (nothing registered, or a stdlib file was prepended without a marker so
 * its declarations carry the user file's name). The resolver's visibility
 * carve-out falls back to its line threshold on -1. */
int iron_stdlib_origin_classify(const char *filename);

#endif
