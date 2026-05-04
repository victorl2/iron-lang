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
#include "lsp/facade/nav/references_index.h"  /* IronLsp_RefSite */
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

/* ── References (Plan 04 Task 01, NAV-06, D-09) ─────────────────── */

/* Facade entry for textDocument/references. Triggers bulk-analyze
 * of every non-open workspace entry on first call; queries the
 * reverse-ref index; optionally prepends the decl span (when
 * include_declaration == true); returns arena-allocated array of
 * {uri, range}. Stdlib/dep use-sites are filtered UNCONDITIONALLY
 * (D-09 LOCKED). Returns empty on missing resolved_sym / cursor
 * outside any ident. Cancel flag polled between files. */
void ilsp_facade_nav_references(struct IronLsp_Server         *server,
                                  struct IronLsp_Document       *doc,
                                  IronLsp_Position               pos,
                                  bool                           include_declaration,
                                  _Atomic bool                  *cancel,
                                  Iron_Arena                    *arena,
                                  IronLsp_RefSite              **out_sites,
                                  size_t                        *out_n);

/* ── Hover (Plan 04 Task 02, NAV-09, D-04) ──────────────────────── */

typedef struct IronLsp_HoverResult {
    const char    *markdown;    /* arena-allocated; NULL on no-hover */
    IronLsp_Range  range;        /* target ident span; zeroed on no-hover */
    bool           has_range;
} IronLsp_HoverResult;

void ilsp_facade_hover(struct IronLsp_Server   *server,
                        struct IronLsp_Document *doc,
                        IronLsp_Position         pos,
                        _Atomic bool            *cancel,
                        Iron_Arena              *arena,
                        IronLsp_HoverResult     *out);

/* ── SignatureHelp (Plan 04 Task 03, NAV-10, D-13) ───────────────── */

typedef struct IronLsp_SigParam {
    int start;    /* byte offset in label (inclusive) */
    int end;      /* byte offset in label (exclusive) */
} IronLsp_SigParam;

typedef struct IronLsp_SignatureInfo {
    const char        *label;            /* arena-allocated full signature */
    const char        *documentation;    /* arena-allocated doc_comment; may be NULL */
    IronLsp_SigParam  *parameter_offsets;/* stb_ds / arena array */
    int                parameter_count;
} IronLsp_SignatureInfo;

void ilsp_facade_signature_help(struct IronLsp_Server    *server,
                                  struct IronLsp_Document  *doc,
                                  IronLsp_Position          pos,
                                  _Atomic bool             *cancel,
                                  Iron_Arena               *arena,
                                  IronLsp_SignatureInfo   **out_sigs,
                                  size_t                   *out_n,
                                  int                      *out_active_sig,
                                  int                      *out_active_param);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_FACADE_NAV_CORE_H */
