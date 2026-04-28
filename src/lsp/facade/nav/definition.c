/* Phase 3 Plan 03 Task 02 (NAV-02) -- textDocument/definition facade.
 *
 * Data flow (per D-01, D-02, D-14, D-16):
 *   1. Analyze the doc via ilsp_facade_compile_for_nav (the SINGLE
 *      iron_analyze_buffer seam, returns the Iron_Program).
 *   2. Resolve cursor -> AST node via ilsp_nav_node_at.
 *   3. If the node is IRON_NODE_IDENT, read resolved_sym.
 *   4. Walk resolved_sym->decl_node; determine its span.
 *   5. Determine the target file:
 *        - if decl's span.filename matches doc's URI: same-file
 *        - otherwise: look up via workspace_index for cross-file
 *   6. Build a LocationLink:
 *        - origin_selection_range = cursor ident span
 *        - target_range           = decl node's full span
 *        - target_selection_range = decl's identifier span
 *
 * Empty array return for: no resolved_sym, no decl_node, or cursor
 * outside any identifier. */

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/nav/visibility.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "lsp/server/server.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Identifier-only span for a decl node. For now this matches the full
 * span; when the parser grows separate identifier-token spans the
 * selection range will narrow. */
static Iron_Span decl_ident_span(Iron_Node *decl) {
    if (!decl) { Iron_Span s = {0}; return s; }
    return decl->span;
}

/* Determine whether the decl's recorded span.filename points at the
 * same file as `doc`. */
static bool decl_is_in_doc(const Iron_Node *decl, const IronLsp_Document *doc) {
    if (!decl || !doc || !doc->uri) return false;
    const char *fn = decl->span.filename;
    if (!fn || !*fn) return true;  /* default to same-file when unset */
    if (strcmp(fn, doc->uri) == 0) return true;
    /* doc->uri may be "file://..." while fn is a raw path; match suffix. */
    size_t fl = strlen(fn);
    size_t ul = strlen(doc->uri);
    return (ul >= fl && strcmp(doc->uri + (ul - fl), fn) == 0);
}

/* Build the same-file LocationLink. */
static IronLsp_LocationLink link_same_file(IronLsp_Document        *doc,
                                             Iron_Node               *ident,
                                             Iron_Node               *decl,
                                             IronLsp_PositionEncoding enc,
                                             Iron_Arena              *arena) {
    IronLsp_LocationLink L;
    memset(&L, 0, sizeof(L));
    L.origin_selection_range   = ilsp_span_to_lsp_range(ident->span, doc, enc);
    L.target_range             = ilsp_span_to_lsp_range(decl->span, doc, enc);
    L.target_selection_range   = ilsp_span_to_lsp_range(decl_ident_span(decl), doc, enc);
    L.target_uri = doc->uri
        ? iron_arena_strdup(arena, doc->uri, strlen(doc->uri))
        : "";
    return L;
}

/* Build a cross-file LocationLink using a workspace-index entry for
 * the target file's line index + source bytes. */
static IronLsp_LocationLink link_cross_file(IronLsp_Document        *doc,
                                               Iron_Node               *ident,
                                               IronLsp_IndexEntry      *entry,
                                               Iron_Node               *decl,
                                               IronLsp_PositionEncoding enc,
                                               Iron_Arena              *arena) {
    IronLsp_LocationLink L;
    memset(&L, 0, sizeof(L));
    L.origin_selection_range = ilsp_span_to_lsp_range(ident->span, doc, enc);
    L.target_range           = ilsp_nav_entry_span_to_range(entry, decl->span, enc);
    L.target_selection_range = ilsp_nav_entry_span_to_range(
        entry, decl_ident_span(decl), enc);
    L.target_uri = ilsp_nav_path_to_uri(entry->canonical_path, arena);
    if (!L.target_uri) L.target_uri = "";
    return L;
}

/* Fallback LocationLink when we know the file name but have no
 * workspace_index entry. Positions fall back to doc's line index
 * (best effort). */
static IronLsp_LocationLink link_fallback_fname(IronLsp_Document        *doc,
                                                   Iron_Node               *ident,
                                                   Iron_Node               *decl,
                                                   IronLsp_PositionEncoding enc,
                                                   Iron_Arena              *arena) {
    IronLsp_LocationLink L;
    memset(&L, 0, sizeof(L));
    L.origin_selection_range = ilsp_span_to_lsp_range(ident->span, doc, enc);
    L.target_range           = ilsp_span_to_lsp_range(decl->span, doc, enc);
    L.target_selection_range = L.target_range;
    const char *fn = decl->span.filename;
    L.target_uri = fn ? ilsp_nav_path_to_uri(fn, arena) : "";
    if (!L.target_uri) L.target_uri = "";
    return L;
}

/* Walk the sealed program to find the Iron_Ident at cursor and the
 * symbol it resolved to. On success sets *out_ident and returns the
 * symbol; otherwise returns NULL. Only IDENT nodes carry
 * resolved_sym, so this is currently decl-top-level only (walk into
 * expression bodies is a Plan 04 extension). */
static const Iron_Symbol *ident_at_cursor(IronLsp_Document        *doc,
                                             const Iron_Program     *program,
                                             IronLsp_Position        pos,
                                             IronLsp_PositionEncoding enc,
                                             Iron_Node             **out_ident) {
    if (out_ident) *out_ident = NULL;
    Iron_Node *n = ilsp_nav_node_at(doc, program, pos, enc);
    if (!n) return NULL;
    if (out_ident) *out_ident = n;
    if (n->kind == IRON_NODE_IDENT) {
        const Iron_Ident *id = (const Iron_Ident *)n;
        return id->resolved_sym;
    }
    /* Cursor on the decl itself -- synthesize a "self-definition"
     * return below by having the caller handle ident == decl. */
    return NULL;
}

/* Shared resolver for definition + declaration. */
static void resolve(IronLsp_Server          *server,
                     IronLsp_Document        *doc,
                     IronLsp_Position         pos,
                     _Atomic bool            *cancel,
                     Iron_Arena              *arena,
                     IronLsp_LocationLink   **out_links,
                     size_t                  *out_n) {
    if (out_links) *out_links = NULL;
    if (out_n)     *out_n     = 0;
    if (!server || !doc || !arena || !out_links || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    /* Analyze doc via the single nav seam. Walk-arena is freed at
     * function exit; the caller's arena holds returned strings. */
    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);

    if (!program) goto done;
    if (cancel && atomic_load(cancel)) goto done;

    Iron_Node         *ident = NULL;
    const Iron_Symbol *sym   = ident_at_cursor(doc, program, pos, enc, &ident);

    /* Cursor on the decl itself (e.g. clicking on the function name in
     * the declaration). Treat as a self-definition -- return the decl
     * span as the link target. */
    if (!sym && ident) {
        switch ((int)ident->kind) {
            case IRON_NODE_FUNC_DECL:
            case IRON_NODE_METHOD_DECL:
            case IRON_NODE_OBJECT_DECL:
            case IRON_NODE_INTERFACE_DECL:
            case IRON_NODE_ENUM_DECL:
            case IRON_NODE_FIELD:
            case IRON_NODE_ENUM_VARIANT:
            case IRON_NODE_IMPORT_DECL: {
                IronLsp_LocationLink *arr = (IronLsp_LocationLink *)iron_arena_alloc(
                    arena, sizeof(*arr), _Alignof(IronLsp_LocationLink));
                if (!arr) goto done;
                arr[0] = link_same_file(doc, ident, ident, enc, arena);
                *out_links = arr;
                *out_n = 1;
                goto done;
            }
            default:
                goto done;
        }
    }

    if (!sym || !sym->decl_node || !ident) goto done;

    /* NEW Phase 10 VIS-03 (REQUIREMENTS.md:164): gate definition target
     * on cross-module visibility. When requester != decl module AND
     * symbol not visible, return empty result (definition request
     * returns null per LSP contract). Stdlib carve-out (D-08) flows
     * through ilsp_vis_can_see automatically. */
    {
        const char *decl_path =
            (sym->decl_node && sym->decl_node->span.filename)
                ? sym->decl_node->span.filename : "";
        const char *requester = (doc && doc->uri) ? doc->uri : "";
        if (!ilsp_vis_can_see(decl_path, requester, sym->decl_node)) {
            goto done;  /* leaves *out_n == 0 */
        }
    }

    /* Allocate into caller's arena. */
    IronLsp_LocationLink *arr = (IronLsp_LocationLink *)iron_arena_alloc(
        arena, sizeof(*arr), _Alignof(IronLsp_LocationLink));
    if (!arr) goto done;

    if (decl_is_in_doc(sym->decl_node, doc)) {
        arr[0] = link_same_file(doc, ident, sym->decl_node, enc, arena);
    } else {
        IronLsp_IndexEntry *entry = NULL;
        if (server->workspace_index && sym->decl_node->span.filename) {
            entry = ilsp_workspace_index_lookup(
                server->workspace_index, sym->decl_node->span.filename);
        }
        arr[0] = entry
            ? link_cross_file(doc, ident, entry, sym->decl_node, enc, arena)
            : link_fallback_fname(doc, ident, sym->decl_node, enc, arena);
    }
    *out_links = arr;
    *out_n = 1;

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}

void ilsp_facade_nav_definition(IronLsp_Server        *server,
                                 IronLsp_Document      *doc,
                                 IronLsp_Position       pos,
                                 _Atomic bool          *cancel,
                                 Iron_Arena            *arena,
                                 IronLsp_LocationLink **out_links,
                                 size_t                *out_n) {
    resolve(server, doc, pos, cancel, arena, out_links, out_n);
}
