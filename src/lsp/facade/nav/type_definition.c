/* Phase 3 Plan 03 Task 02 (NAV-04, D-05) -- textDocument/typeDefinition.
 *
 * Data flow:
 *   1. Analyze doc via ilsp_facade_compile_for_nav.
 *   2. Resolve cursor -> AST node -> resolved_sym.
 *   3. Walk resolved_sym->type and take its Iron_Type.decl backpointer.
 *   4. Empty result for primitives (type.decl == NULL), function types,
 *      tuple types, and generic params (Phase 3 scope defers generic-
 *      param span to hover/completion).
 *   5. Build a LocationLink pointing at the type-decl's span.
 */

#include "lsp/facade/nav/nav_core.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "lsp/server/server.h"
#include "analyzer/analyzer.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Return the declaring AST node for a type, or NULL for primitives /
 * function types / tuple types / generic params (D-05). */
static Iron_Node *type_decl_node(const Iron_Type *t) {
    if (!t) return NULL;
    switch (t->kind) {
        case IRON_TYPE_OBJECT:
            return (Iron_Node *)t->object.decl;
        case IRON_TYPE_INTERFACE:
            return (Iron_Node *)t->interface.decl;
        case IRON_TYPE_ENUM:
            return (Iron_Node *)t->enu.decl;
        case IRON_TYPE_NULLABLE:
            return type_decl_node(t->nullable.inner);
        case IRON_TYPE_RC:
            return type_decl_node(t->rc.inner);
        case IRON_TYPE_ARRAY:
            return type_decl_node(t->array.elem);
        /* Primitives + function + tuple + generic -> empty (D-05). */
        case IRON_TYPE_INT:  case IRON_TYPE_INT8:  case IRON_TYPE_INT16:
        case IRON_TYPE_INT32: case IRON_TYPE_INT64:
        case IRON_TYPE_UINT: case IRON_TYPE_UINT8: case IRON_TYPE_UINT16:
        case IRON_TYPE_UINT32: case IRON_TYPE_UINT64:
        case IRON_TYPE_FLOAT: case IRON_TYPE_FLOAT32: case IRON_TYPE_FLOAT64:
        case IRON_TYPE_BOOL: case IRON_TYPE_STRING: case IRON_TYPE_VOID:
        case IRON_TYPE_NULL: case IRON_TYPE_ERROR:
        case IRON_TYPE_FUNC: case IRON_TYPE_TUPLE:
        case IRON_TYPE_GENERIC_PARAM:
            return NULL;
    }
    return NULL;
}

static const Iron_Symbol *ident_sym_at_cursor(
    IronLsp_Document         *doc,
    const Iron_Program       *program,
    IronLsp_Position          pos,
    IronLsp_PositionEncoding  enc,
    Iron_Node               **out_ident) {
    if (out_ident) *out_ident = NULL;
    Iron_Node *n = ilsp_nav_node_at(doc, program, pos, enc);
    if (!n) return NULL;
    if (out_ident) *out_ident = n;
    if (n->kind == IRON_NODE_IDENT) {
        return ((const Iron_Ident *)n)->resolved_sym;
    }
    return NULL;
}

static bool decl_is_in_doc(const Iron_Node *decl, const IronLsp_Document *doc) {
    if (!decl || !doc || !doc->uri) return false;
    const char *fn = decl->span.filename;
    if (!fn || !*fn) return true;
    if (strcmp(fn, doc->uri) == 0) return true;
    size_t fl = strlen(fn);
    size_t ul = strlen(doc->uri);
    return (ul >= fl && strcmp(doc->uri + (ul - fl), fn) == 0);
}

void ilsp_facade_nav_type_definition(IronLsp_Server        *server,
                                      IronLsp_Document      *doc,
                                      IronLsp_Position       pos,
                                      _Atomic bool          *cancel,
                                      Iron_Arena            *arena,
                                      IronLsp_LocationLink **out_links,
                                      size_t                *out_n) {
    if (out_links) *out_links = NULL;
    if (out_n)     *out_n     = 0;
    if (!server || !doc || !arena || !out_links || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);

    if (!program) goto done;
    if (cancel && atomic_load(cancel)) goto done;

    Iron_Node *ident = NULL;
    const Iron_Symbol *sym = ident_sym_at_cursor(doc, program, pos, enc, &ident);
    if (!sym || !ident) goto done;

    Iron_Node *tdecl = type_decl_node(sym->type);
    if (!tdecl) goto done;   /* primitive / func / tuple / generic */

    IronLsp_LocationLink *arr = (IronLsp_LocationLink *)iron_arena_alloc(
        arena, sizeof(*arr), _Alignof(IronLsp_LocationLink));
    if (!arr) goto done;

    IronLsp_LocationLink L;
    memset(&L, 0, sizeof(L));
    L.origin_selection_range = ilsp_span_to_lsp_range(ident->span, doc, enc);

    if (decl_is_in_doc(tdecl, doc)) {
        L.target_range           = ilsp_span_to_lsp_range(tdecl->span, doc, enc);
        L.target_selection_range = L.target_range;
        L.target_uri = doc->uri
            ? iron_arena_strdup(arena, doc->uri, strlen(doc->uri)) : "";
    } else {
        IronLsp_IndexEntry *entry = NULL;
        if (server->workspace_index && tdecl->span.filename) {
            entry = ilsp_workspace_index_lookup(
                server->workspace_index, tdecl->span.filename);
        }
        if (entry) {
            L.target_range           = ilsp_nav_entry_span_to_range(entry, tdecl->span, enc);
            L.target_selection_range = L.target_range;
            L.target_uri             = ilsp_nav_path_to_uri(entry->canonical_path, arena);
        } else {
            L.target_range           = ilsp_span_to_lsp_range(tdecl->span, doc, enc);
            L.target_selection_range = L.target_range;
            L.target_uri = tdecl->span.filename
                ? ilsp_nav_path_to_uri(tdecl->span.filename, arena)
                : "";
        }
    }
    if (!L.target_uri) L.target_uri = "";
    arr[0] = L;
    *out_links = arr;
    *out_n = 1;

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
