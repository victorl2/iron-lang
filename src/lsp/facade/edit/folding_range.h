#ifndef ILSP_FACADE_FOLDING_RANGE_H
#define ILSP_FACADE_FOLDING_RANGE_H

/* Phase 4 Plan 04-07 Task 02 (EDIT-14, D-13) -- textDocument/foldingRange.
 *
 * Strict parser-only endpoint -- walks the document's lexer+parser
 * output WITHOUT triggering iron_analyze_buffer. This guarantees that
 * folding keeps working on syntactically broken files (Iron_ErrorNode
 * spans are skipped; well-formed sibling decls still produce folds).
 *
 * Emitted fold categories (0-indexed lines, LSP):
 *   "region"  : function/method/object/interface/enum bodies; any
 *                Iron_Block ≥ 2 lines; multi-line Iron_MatchStmt.
 *   "imports" : consecutive run of Iron_ImportDecl collapses to ONE fold.
 *   "comment" : multi-line `///` doc-comment run attached to a decl;
 *                multi-line `//` non-doc runs when detectable.
 *
 * The output is arena-allocated by the caller. */

#include "util/arena.h"  /* Iron_Arena typedef */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct IronLsp_Server;
struct IronLsp_Document;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IronLsp_FoldingRange {
    uint32_t    start_line;      /* 0-indexed LSP line */
    uint32_t    end_line;        /* 0-indexed LSP line */
    const char *kind;            /* "region" | "imports" | "comment"; NULL => default "region" */
} IronLsp_FoldingRange;

/* Facade entry: emit foldable ranges for the current document. The
 * walker is parser-only; no analyzer re-entry. On cancel / missing
 * parse it returns 0 entries gracefully. */
void ilsp_facade_folding_range(struct IronLsp_Server   *server,
                                 struct IronLsp_Document *doc,
                                 _Atomic bool            *cancel,
                                 Iron_Arena              *arena,
                                 IronLsp_FoldingRange   **out,
                                 size_t                  *out_n);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_FACADE_FOLDING_RANGE_H */
