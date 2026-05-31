/* Phase 34 LSP-04 (Plan 34-03) — `defer free <binding>` backward-scan
 * implementation. See defer_free.h for the public contract.
 *
 * Walks the cursor's enclosing function body (Iron_FuncDecl or
 * Iron_MethodDecl) reverse-iterating statements. For each
 * Iron_ValDecl / Iron_VarDecl whose `init` is an IRON_NODE_HEAP or
 * IRON_NODE_RC expression and whose span strictly precedes the cursor
 * line, the binding name is arena-strdup'd into `out_names[]`.
 *
 * Recurses into if/while/for body blocks so a heap-binding inside an
 * `if` block surfaces when the cursor sits on a statement below that
 * block. Cap honored before any recursion.
 */

#include "lsp/facade/edit/complete/defer_free.h"

#include <stdbool.h>
#include <string.h>

/* True iff `init` is `heap T(...)` or `rc T(...)`. The parser emits
 * IRON_NODE_HEAP / IRON_NODE_RC directly for those forms (see
 * src/parser/ast.h:841/854). */
static bool is_heap_or_rc_init(const Iron_Node *init) {
    if (!init) return false;
    return init->kind == IRON_NODE_HEAP || init->kind == IRON_NODE_RC;
}

/* Forward decl for mutual recursion. */
static size_t collect_from_stmt(const Iron_Node *stmt,
                                  uint32_t cursor_line_1,
                                  Iron_Arena *arena,
                                  const char **out_names,
                                  size_t out_cap, size_t out_n);

static size_t collect_from_block(const Iron_Block *blk,
                                   uint32_t cursor_line_1,
                                   Iron_Arena *arena,
                                   const char **out_names,
                                   size_t out_cap, size_t out_n) {
    if (!blk || !blk->stmts) return out_n;
    /* Reverse walk so most-recent binding lands at out_names[0]. */
    for (int i = blk->stmt_count - 1; i >= 0 && out_n < out_cap; i--) {
        out_n = collect_from_stmt(blk->stmts[i], cursor_line_1,
                                     arena, out_names, out_cap, out_n);
    }
    return out_n;
}

static size_t collect_from_stmt(const Iron_Node *stmt,
                                  uint32_t cursor_line_1,
                                  Iron_Arena *arena,
                                  const char **out_names,
                                  size_t out_cap, size_t out_n) {
    if (!stmt) return out_n;
    /* Only count bindings whose span strictly precedes the cursor line.
     * Multi-line constructs (if/while/for) qualify only if they end
     * before the cursor. */
    if (stmt->span.end_line >= cursor_line_1) return out_n;
    if (out_n >= out_cap) return out_n;

    switch ((int)stmt->kind) {
        case IRON_NODE_VAL_DECL: {
            const Iron_ValDecl *d = (const Iron_ValDecl *)stmt;
            if (d->name && is_heap_or_rc_init(d->init)) {
                out_names[out_n++] = iron_arena_strdup(arena, d->name,
                                                         strlen(d->name));
            }
            break;
        }
        case IRON_NODE_VAR_DECL: {
            const Iron_VarDecl *d = (const Iron_VarDecl *)stmt;
            if (d->name && is_heap_or_rc_init(d->init)) {
                out_names[out_n++] = iron_arena_strdup(arena, d->name,
                                                         strlen(d->name));
            }
            break;
        }
        case IRON_NODE_BLOCK:
            out_n = collect_from_block((const Iron_Block *)stmt,
                                          cursor_line_1, arena,
                                          out_names, out_cap, out_n);
            break;
        case IRON_NODE_IF: {
            const Iron_IfStmt *s = (const Iron_IfStmt *)stmt;
            out_n = collect_from_stmt(s->body, cursor_line_1, arena,
                                         out_names, out_cap, out_n);
            for (int i = 0; i < s->elif_count && out_n < out_cap; i++) {
                out_n = collect_from_stmt(s->elif_bodies[i],
                                             cursor_line_1, arena,
                                             out_names, out_cap, out_n);
            }
            if (s->else_body && out_n < out_cap) {
                out_n = collect_from_stmt(s->else_body, cursor_line_1,
                                             arena, out_names, out_cap, out_n);
            }
            break;
        }
        case IRON_NODE_WHILE: {
            const Iron_WhileStmt *s = (const Iron_WhileStmt *)stmt;
            out_n = collect_from_stmt(s->body, cursor_line_1, arena,
                                         out_names, out_cap, out_n);
            break;
        }
        case IRON_NODE_FOR: {
            const Iron_ForStmt *s = (const Iron_ForStmt *)stmt;
            out_n = collect_from_stmt(s->body, cursor_line_1, arena,
                                         out_names, out_cap, out_n);
            break;
        }
        default:
            break;
    }
    return out_n;
}

/* Find the func / method decl whose body block span covers
 * cursor_line_1. Returns the body block, or NULL when the cursor is
 * outside any function. */
static const Iron_Block *find_enclosing_function_body(const Iron_Program *program,
                                                       uint32_t cursor_line_1) {
    if (!program) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        const Iron_Node *d = program->decls[i];
        if (!d) continue;
        const Iron_Node *body = NULL;
        if (d->kind == IRON_NODE_FUNC_DECL) {
            body = ((const Iron_FuncDecl *)d)->body;
        } else if (d->kind == IRON_NODE_METHOD_DECL) {
            body = ((const Iron_MethodDecl *)d)->body;
        }
        if (!body || body->kind != IRON_NODE_BLOCK) continue;
        if (cursor_line_1 < body->span.line) continue;
        if (cursor_line_1 > body->span.end_line) continue;
        return (const Iron_Block *)body;
    }
    return NULL;
}

size_t ilsp_collect_recent_heap_rc_bindings(const Iron_Program *program,
                                              uint32_t            cursor_line_1,
                                              Iron_Arena         *arena,
                                              const char        **out_names,
                                              size_t              out_cap) {
    if (!program || !out_names || out_cap == 0 || !arena) return 0;
    const Iron_Block *body = find_enclosing_function_body(program,
                                                            cursor_line_1);
    if (!body) return 0;
    return collect_from_block(body, cursor_line_1, arena,
                                out_names, out_cap, 0);
}
