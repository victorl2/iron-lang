#ifndef IRON_LSP_FACADE_NAV_SYMBOL_ID_H
#define IRON_LSP_FACADE_NAV_SYMBOL_ID_H

/* Phase 3 Plan 01 Task 04 (NAV-16) -- stable symbol identity triple.
 *
 * Every NAV/hover endpoint needs to know "is symbol A the same as
 * symbol B" across re-analyze cycles. We cannot use Iron_Symbol* for
 * identity: pointers are arena-scoped and a second analyze pass yields
 * fresh pointers for the same semantic symbol.
 *
 * The identity triple is content-addressed:
 *   (canonical_path, name_path, kind)
 *
 *   canonical_path -- arena-owned string. One of:
 *     - absolute filesystem path, e.g. "/abs/path.iron"
 *     - stdlib sentinel, e.g. "stdlib://math"
 *     - dependency sentinel, e.g. "dep://<name>/<rel>"
 *
 *   name_path -- arena-owned dotted trail of identifiers from the
 *     module to the symbol, e.g. "mymodule.Foo.bar" for method `bar`
 *     on object `Foo` in file `mymodule.iron`.
 *
 *   kind -- Iron_SymbolKind, frozen from this phase onward.
 *
 * A 64-bit FNV-1a hash over the triple is precomputed at derive time
 * so downstream shmap lookups are O(1) without re-hashing on every
 * equality check.
 *
 * FNV-1a constants (pinned -- Phase 3 lock, D-02):
 *   offset basis = 0xcbf29ce484222325
 *   prime        = 0x100000001b3
 */

#include "analyzer/scope.h"   /* Iron_SymbolKind */
#include "parser/ast.h"       /* Iron_Program typedef */
#include "util/arena.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decls keep the header light. */
struct Iron_Symbol;

typedef struct IronLsp_SymbolId {
    const char      *canonical_path;  /* arena-owned; see header comment */
    const char      *name_path;       /* arena-owned dotted trail */
    Iron_SymbolKind  kind;            /* frozen from Phase 3 onward */
    uint64_t         hash;            /* FNV-1a(canonical + 0x00 + name_path
                                       *       + 0x00 + kind_byte) */
} IronLsp_SymbolId;

/* Derive an identity triple for `sym`. canonical_path must be the
 * already-canonicalised filename / sentinel (caller owns normalization).
 * `program` is optional: when non-NULL, the walker uses it to assemble
 * the name_path for methods/fields by locating the owning object decl;
 * when NULL, name_path falls back to "<module>.<name>".
 *
 * arena is used to intern the joined name_path string. Returns a
 * zero-filled triple if `sym` is NULL. */
IronLsp_SymbolId ilsp_symbol_id_derive(const struct Iron_Symbol *sym,
                                        const char               *canonical_path,
                                        const Iron_Program       *program,
                                        Iron_Arena               *arena);

/* Strict equality: all three components match (string equality on paths
 * and name_path, direct compare on kind). Pointer identity on the
 * strings is NOT sufficient -- the triple may have been derived from
 * different arenas. */
bool ilsp_symbol_id_equal(IronLsp_SymbolId a, IronLsp_SymbolId b);

/* Accessor. Returns the precomputed FNV-1a hash from derive. */
uint64_t ilsp_symbol_id_hash(IronLsp_SymbolId id);

/* Standalone FNV-1a 64-bit helper exposed for tests and for callers
 * that need to hash raw bytes with the same pinned constants
 * (0xcbf29ce484222325 offset, 0x100000001b3 prime). */
uint64_t ilsp_symbol_id_fnv1a64(const void *bytes, size_t len);

/* Extract the module stem from a canonical path. Rules:
 *   - "/abs/path/mymodule.iron"  -> "mymodule"
 *   - "stdlib://math"            -> "math"
 *   - "dep://pkg/rel/util.iron"  -> "util"
 * Returns arena-interned stem string. Returns "" for NULL/empty input. */
const char *ilsp_symbol_id_module_stem(const char *canonical_path,
                                         Iron_Arena *arena);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_SYMBOL_ID_H */
