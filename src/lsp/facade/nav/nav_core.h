#ifndef IRON_LSP_FACADE_NAV_CORE_H
#define IRON_LSP_FACADE_NAV_CORE_H

/* Phase 3 Plan 03 Task 02/03 -- facade entry points for the 5 nav
 * endpoints shipped in this plan.
 *
 * Contract: every entry point takes the server (for workspace_index
 * access), the open document (may be NULL for workspace-index-only
 * entries), the cursor position, a cancel flag, and a caller-owned
 * arena. Output is an array of LocationLink / DocSymbol /
 * WorkspaceSymbol values arena-allocated into the caller's arena, plus
 * a count via `out_n`.
 *
 * All entry points honour the SINGLE iron_analyze_buffer rule by
 * routing workspace-index analyze through ilsp_workspace_index_analyze_lazy
 * (which itself funnels into ilsp_facade_compile_pure). */

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lsp/facade/nav/nav_common.h"   /* IronLsp_LocationLink */
#include "lsp/facade/types.h"
#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Server;
struct IronLsp_Document;

/* ── LocationLink-returning endpoints ─────────────────────────────── */

void ilsp_facade_nav_definition(struct IronLsp_Server         *server,
                                 struct IronLsp_Document       *doc,
                                 IronLsp_Position               pos,
                                 _Atomic bool                  *cancel,
                                 Iron_Arena                    *arena,
                                 IronLsp_LocationLink         **out_links,
                                 size_t                        *out_n);

void ilsp_facade_nav_declaration(struct IronLsp_Server         *server,
                                  struct IronLsp_Document       *doc,
                                  IronLsp_Position               pos,
                                  _Atomic bool                  *cancel,
                                  Iron_Arena                    *arena,
                                  IronLsp_LocationLink         **out_links,
                                  size_t                        *out_n);

void ilsp_facade_nav_type_definition(struct IronLsp_Server         *server,
                                      struct IronLsp_Document       *doc,
                                      IronLsp_Position               pos,
                                      _Atomic bool                  *cancel,
                                      Iron_Arena                    *arena,
                                      IronLsp_LocationLink         **out_links,
                                      size_t                        *out_n);

/* ── DocumentSymbol (Task 03) ─────────────────────────────────────── */

typedef struct IronLsp_DocSymbol {
    const char    *name;
    const char    *detail;         /* optional; NULL if absent */
    int            kind;           /* LSP SymbolKind code */
    IronLsp_Range  range;
    IronLsp_Range  selection_range;
    /* stb_ds array of child symbols (for hierarchical tree). */
    struct IronLsp_DocSymbol *children;
    size_t         child_count;
} IronLsp_DocSymbol;

void ilsp_facade_nav_document_symbol(struct IronLsp_Server       *server,
                                      struct IronLsp_Document     *doc,
                                      _Atomic bool                *cancel,
                                      Iron_Arena                  *arena,
                                      IronLsp_DocSymbol          **out_syms,
                                      size_t                      *out_n,
                                      bool                         hierarchical);

/* ── WorkspaceSymbol (Task 03) ────────────────────────────────────── */

typedef struct IronLsp_WorkspaceSymbol {
    const char    *name;
    const char    *container_name;
    int            kind;
    const char    *uri;
    IronLsp_Range  range;
    IronLsp_Range  selection_range;
    double         fuzzy_score;     /* internal; omitted from wire */
    int            kind_rank;       /* tiebreak rank from fuzzy.h */
} IronLsp_WorkspaceSymbol;

#define ILSP_WORKSPACE_SYMBOL_MAX_RESULTS  256
#define ILSP_WORKSPACE_SYMBOL_QUERY_MAX    256

void ilsp_facade_nav_workspace_symbol(struct IronLsp_Server          *server,
                                       const char                     *query,
                                       _Atomic bool                   *cancel,
                                       Iron_Arena                     *arena,
                                       IronLsp_WorkspaceSymbol       **out,
                                       size_t                         *out_n);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_CORE_H */
