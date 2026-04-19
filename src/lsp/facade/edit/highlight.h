#ifndef ILSP_FACADE_HIGHLIGHT_H
#define ILSP_FACADE_HIGHLIGHT_H

/* Phase 4 Plan 04-07 Task 01 (EDIT-13, D-12) -- textDocument/documentHighlight.
 *
 * AST-parent-kind classifier. Returns one DocumentHighlight per
 * matching symbol use-site in the current document:
 *   kind = 3 (Write) : decl sites + assign LHS + compound-assign LHS +
 *                       object/field/param/for-loop binding.
 *   kind = 2 (Read)  : everything else that resolves to the same symbol.
 *   kind = 1 (Text)  : never emitted in M3; reserved for future passes.
 *
 * Rules (D-12, locked):
 *   - Cursor must land on an Iron_Ident with resolved_sym != NULL;
 *     NULL resolved_sym => empty array (T-4-1 mitigation).
 *   - NEVER walk into IRON_NODE_STRING_LIT or comment tokens.
 *   - Exhaustive switches over Iron_NodeKind (-Werror=switch-enum safe).
 *   - Arena-allocated output; caller owns lifetime.
 *
 * This endpoint is analyzer-backed: it needs resolved_sym, which is
 * produced by the analyzer. Goes through ilsp_facade_compile_for_nav
 * (the single iron_analyze_buffer call site preserved by CORE-22). */

#include "lsp/facade/types.h"  /* IronLsp_Position */
#include "util/arena.h"        /* Iron_Arena typedef */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct IronLsp_Server;
struct IronLsp_Document;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IronLsp_DocumentHighlight {
    uint32_t range_start_line;
    uint32_t range_start_char;
    uint32_t range_end_line;
    uint32_t range_end_char;
    int      kind;   /* 1 = Text, 2 = Read, 3 = Write (LSP 3.17) */
} IronLsp_DocumentHighlight;

/* Facade entry: produce Read/Write highlights for the symbol under the
 * cursor. out is arena-allocated. *out_n = 0 on graceful fallback. */
void ilsp_facade_document_highlight(struct IronLsp_Server      *server,
                                      struct IronLsp_Document    *doc,
                                      IronLsp_Position            pos,
                                      _Atomic bool               *cancel,
                                      Iron_Arena                 *arena,
                                      IronLsp_DocumentHighlight **out,
                                      size_t                     *out_n);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_FACADE_HIGHLIGHT_H */
