/* Phase 4 Plan 04-06 Task 01 (EDIT-10, D-09) -- textDocument/prepareRename
 * facade.
 *
 * Classifies the cursor into one of 10 cases (6 reject + 4 accept) per
 * D-09:
 *   R1 keyword/literal/whitespace/comment                -> SILENT
 *   R2 ident with resolved_sym == NULL (partial parse)  -> SILENT
 *   R3 symbol with canonical_path starting stdlib://    -> STDLIB + msg
 *   R4 symbol with canonical_path starting dep://       -> DEP + msg
 *   R5 symbol with is_extern == true                    -> EXTERN + msg
 *   R6 symbol that is a builtin primitive type alias    -> BUILTIN + msg
 *   A1..A4 user workspace function / method / field /
 *          local val / var / param / object / iface /
 *          enum / enum-variant                          -> ACCEPT {range, placeholder}
 *
 * Source of truth for "canonical_path" on a symbol:
 *   sym->decl_node->span.filename  (Iron_Span.filename is the decl's
 *   owning file or sentinel string). Stdlib-cached symbols carry
 *   filenames starting with "stdlib://"; dep-resolved symbols carry
 *   "dep://" prefixes. User workspace decls carry absolute POSIX paths.
 */

#include "lsp/facade/edit/rename/prepare.h"

#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "analyzer/scope.h"
#include "parser/ast.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Copy of definition.c:ident_at_cursor per PATTERNS.md §rename/prepare.
 * Returns the resolved Iron_Symbol if cursor is on IRON_NODE_IDENT with
 * a resolved_sym; otherwise NULL. *out_ident is populated with the
 * innermost AST node (possibly NULL) regardless of outcome. */
static const Iron_Symbol *ident_at_cursor(const IronLsp_Document   *doc,
                                            const Iron_Program       *program,
                                            IronLsp_Position          pos,
                                            IronLsp_PositionEncoding  enc,
                                            Iron_Node               **out_ident) {
    if (out_ident) *out_ident = NULL;
    Iron_Node *n = ilsp_nav_node_at(doc, program, pos, enc);
    if (!n) return NULL;
    if (out_ident) *out_ident = n;
    if (n->kind == IRON_NODE_IDENT) {
        const Iron_Ident *id = (const Iron_Ident *)n;
        return id->resolved_sym;
    }
    return NULL;
}

/* Return the decl file of a symbol. Stdlib symbols carry "stdlib://..."
 * filenames; dep symbols carry "dep://..."; user symbols carry absolute
 * POSIX paths. Returns "" (never NULL) for defensive callers. */
static const char *sym_decl_path(const Iron_Symbol *sym) {
    if (!sym) return "";
    if (sym->decl_node && sym->decl_node->span.filename) {
        return sym->decl_node->span.filename;
    }
    if (sym->span.filename) return sym->span.filename;
    return "";
}

/* D-09 category 6: is this symbol a builtin type alias?
 *
 * Iron's primitive types (Int, Bool, String, Float, Void, UInt8..64,
 * Int8..64, Float32/64) are introduced at compile startup with no
 * user-rewritable decl. We detect them by name + symbol kind: a
 * TYPE-kind symbol whose name matches one of the primitive-type names
 * AND whose decl_node is NULL (primitives have no AST decl) is a
 * builtin.
 *
 * (List / Map / Set / Tuple are also built in but they're generic
 * constructors, not aliases; renaming them still targets the stdlib
 * definition which is already caught by R3 stdlib. We keep this
 * whitelist to the pure primitives.) */
static bool is_builtin_type_alias(const Iron_Symbol *sym) {
    if (!sym || !sym->name) return false;
    if (sym->sym_kind != IRON_SYM_TYPE) return false;
    /* Primitives are registered by the compiler with no AST decl node. */
    if (sym->decl_node != NULL) return false;
    static const char *const PRIMITIVES[] = {
        "Int", "Int8", "Int16", "Int32", "Int64",
        "UInt8", "UInt16", "UInt32", "UInt64",
        "Float", "Float32", "Float64",
        "Bool", "String", "Void", "Any",
    };
    size_t n = sizeof(PRIMITIVES) / sizeof(PRIMITIVES[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(sym->name, PRIMITIVES[i]) == 0) return true;
    }
    return false;
}

/* Arena-printf. Returns arena-owned NUL-terminated string. */
static const char *arena_fmt_msg(Iron_Arena *arena, const char *fmt,
                                   const char *name) {
    if (!arena) return "";
    const char *safe = (name && *name) ? name : "(unnamed)";
    /* Message templates are small; 256 bytes is more than enough. */
    size_t cap = strlen(fmt) + strlen(safe) + 32;
    char *buf = (char *)iron_arena_alloc(arena, cap, 1);
    if (!buf) return "";
    int n = snprintf(buf, cap, fmt, safe);
    if (n < 0) buf[0] = '\0';
    return buf;
}

void ilsp_facade_prepare_rename(IronLsp_Server              *server,
                                  IronLsp_Document            *doc,
                                  IronLsp_Position             pos,
                                  _Atomic bool                *cancel,
                                  Iron_Arena                  *arena,
                                  IronLsp_PrepareRenameResult *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    /* Default posture: if analyze or cursor resolution fails we emit
     * SILENT reject (null response, no showMessage) per D-09. */
    out->kind = ILSP_PREPARE_RENAME_REJECT_SILENT;
    if (!doc || !arena) return;

    IronLsp_PositionEncoding enc = server
        ? server->position_encoding : ILSP_ENC_UTF8;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    if (!program) goto done;
    if (cancel && atomic_load(cancel)) goto done;

    Iron_Node *ident = NULL;
    const Iron_Symbol *sym = ident_at_cursor(doc, program, pos, enc, &ident);

    /* Category 1: not an ident node (keyword / literal / whitespace /
     * comment / operator).  ident_at_cursor returns NULL for sym AND
     * ident is either NULL or a non-IDENT kind -- both funnel here. */
    if (!ident || ident->kind != IRON_NODE_IDENT) {
        out->kind = ILSP_PREPARE_RENAME_REJECT_SILENT;
        goto done;
    }

    /* Category 2: ident with no resolved_sym (partial parse / error
     * recovery). Silent -- too noisy to showMessage per D-09. */
    if (!sym) {
        out->kind = ILSP_PREPARE_RENAME_REJECT_SILENT;
        goto done;
    }

    /* Category 3: stdlib symbol. */
    const char *decl_path = sym_decl_path(sym);
    if (decl_path && strncmp(decl_path, "stdlib://", 9) == 0) {
        out->kind = ILSP_PREPARE_RENAME_REJECT_STDLIB;
        out->show_message = arena_fmt_msg(
            arena, "Cannot rename: %s is defined in the standard library.",
            sym->name);
        goto done;
    }

    /* Category 4: dep symbol. */
    if (decl_path && strncmp(decl_path, "dep://", 6) == 0) {
        out->kind = ILSP_PREPARE_RENAME_REJECT_DEP;
        out->show_message = arena_fmt_msg(
            arena, "Cannot rename: %s is defined in a dependency.",
            sym->name);
        goto done;
    }

    /* Category 5: extern / C interop symbol. */
    if (sym->is_extern) {
        out->kind = ILSP_PREPARE_RENAME_REJECT_EXTERN;
        out->show_message = arena_fmt_msg(
            arena, "Cannot rename: %s is an extern (C interop) symbol.",
            sym->name);
        goto done;
    }

    /* Category 6: builtin type alias. */
    if (is_builtin_type_alias(sym)) {
        out->kind = ILSP_PREPARE_RENAME_REJECT_BUILTIN_TYPE;
        out->show_message = arena_fmt_msg(
            arena, "Cannot rename: %s is a builtin type.",
            sym->name);
        goto done;
    }

    /* Accept. Compose the response: {range: ident_span, placeholder: sym->name}.
     * ident->span is 1-based byte-column; convert to LSP encoding-aware
     * 0-based Range via the standard span helper. */
    out->kind  = ILSP_PREPARE_RENAME_ACCEPT;
    out->range = ilsp_span_to_lsp_range(ident->span, doc, enc);
    const char *nm = sym->name ? sym->name : "";
    out->placeholder = iron_arena_strdup(arena, nm, strlen(nm));
    if (!out->placeholder) out->placeholder = "";

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}
