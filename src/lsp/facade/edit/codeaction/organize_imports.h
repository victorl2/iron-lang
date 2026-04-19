#ifndef ILSP_ORGANIZE_IMPORTS_H
#define ILSP_ORGANIZE_IMPORTS_H

/* Phase 4 Plan 04-05 Task 01 (EDIT-09, D-08) -- source.organizeImports
 * facade.
 *
 * The facade walks the leading Iron_ImportDecl run of a document's
 * parsed Iron_Program, classifies each import into one of three groups
 * (stdlib / dep / local), sorts within each group alphabetically, dedupes
 * exact duplicates, optionally removes unused imports (when the workspace
 * index has completed its bulk analyze), and emits a single LSP TextEdit
 * replacing the full import-run span with the reformatted block.
 *
 * D-08 LOCKED behavior (summary):
 *   Group A: stdlib   (import path matches a stdlib_cache key)
 *   Group B: deps     (import path matches a dep_map key)
 *   Group C: local    (everything else)
 *   - Within group: alpha-sort by path; non-aliased precedes aliased.
 *   - Empty groups omit their blank-line separator.
 *   - Exact (path, alias) duplicates collapse to one.
 *   - `import x` + `import x as y` -> both kept (different local bindings).
 *   - Unused-removal REQUIRES wi->bulk_analyze_done == true. When false,
 *     skip unused-removal AND set out->cold_workspace_warning so the
 *     caller can emit a window/showMessage Info notification.
 *   - Doc-comments (Iron_ImportDecl.doc_comment) are preserved and move
 *     with their import.
 *   - Blank lines and comments BEFORE the first import are NOT part of
 *     the replaced range -- they are preserved naturally.
 *
 * Sentinel code value:
 *   ILSP_ORGANIZE_IMPORTS_SENTINEL = -1 sits outside the Iron_Diagnostic
 *   code namespace (see src/diagnostics/diagnostics.h -- lexer 1..99,
 *   parser 101..199, semantic 200..299, LIR 300..399, lowering 400..499,
 *   HIR 500..599, warnings 600+). ilsp_quickfix_lookup(-1) returns NULL
 *   so the codeAction/resolve path takes a dedicated organize-imports
 *   branch rather than the quickfix dispatch.
 */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Iron_Program / Iron_DiagList are typedefs over anonymous structs in
 * parser/ast.h + diagnostics/diagnostics.h -- they cannot be forward
 * declared as `struct`s; include the headers directly. */
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#define ILSP_ORGANIZE_IMPORTS_SENTINEL (-1)

struct IronLsp_Document;
struct IronLsp_WorkspaceIndex;

#ifdef __cplusplus
extern "C" {
#endif

/* Result shape returned by ilsp_organize_imports. */
typedef struct IronLsp_OrganizeImportsResult {
    /* LSP TextEdit covering the full import-run span. Endpoints are
     * 0-indexed line, 0-indexed character in the negotiated position
     * encoding. When new_text == NULL the caller treats this as a
     * "no edit" result (no imports present, or the run was malformed). */
    uint32_t    range_start_line;
    uint32_t    range_start_char;
    uint32_t    range_end_line;
    uint32_t    range_end_char;
    const char *new_text;        /* arena-owned; NULL => no edit */

    /* Out-of-band signal: true when bulk_analyze_done was false at the
     * time of invocation AND the facade had imports worth inspecting.
     * Caller emits a window/showMessage Info explaining that unused-
     * removal was skipped because the workspace index hasn't completed
     * its bulk analyze yet. Sort+dedup is still applied in this case. */
    bool        cold_workspace_warning;
} IronLsp_OrganizeImportsResult;

/* Compute the organize-imports edit for `program` (the open doc's
 * parsed AST). Never errors; writes out->new_text = NULL when no
 * imports exist or the run is otherwise not worth rewriting.
 *
 *   program - the parsed Iron_Program (from ilsp_facade_compile_for_nav
 *             or equivalent). May be NULL -- no edit emitted.
 *   doc     - open document (only used for URI + line-count context).
 *   diags   - current Iron_DiagList for `program`. Used to locate
 *             IRON_WARN_UNUSED_IMPORT diagnostics for the gated unused-
 *             removal pass. May be NULL -- no unused removal attempted.
 *   wi      - workspace index (bulk_analyze_done gate). May be NULL --
 *             treated as cold.
 *   cancel  - optional atomic bool; polled at phase boundaries.
 *   arena   - per-request arena owning the produced new_text.
 *   out     - caller-owned; zero-filled on entry.
 */
void ilsp_organize_imports(const Iron_Program              *program,
                            struct IronLsp_Document         *doc,
                            const Iron_DiagList             *diags,
                            struct IronLsp_WorkspaceIndex   *wi,
                            _Atomic bool                    *cancel,
                            Iron_Arena                      *arena,
                            IronLsp_OrganizeImportsResult   *out);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_ORGANIZE_IMPORTS_H */
