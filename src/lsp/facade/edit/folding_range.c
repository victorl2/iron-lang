/* Phase 4 Plan 04-07 Task 02 (EDIT-14, D-13) -- foldingRange facade.
 *
 * Parser-only walker: runs a fresh lex + parse of the document's
 * current text and emits folds structurally. No analyzer re-entry
 * (no iron-analyze-buffer seam) -- the endpoint keeps working on
 * syntactically broken files because the parser's Iron_ErrorNode
 * recovery path lets well-formed siblings still appear in
 * program->decls[].
 *
 * Fold targets (locked by D-13):
 *   - Iron_FuncDecl.body              → "region"
 *   - Iron_MethodDecl.body            → "region"
 *   - Iron_ObjectDecl                 → "region" (whole decl span)
 *   - Iron_InterfaceDecl              → "region"
 *   - Iron_EnumDecl                   → "region"
 *   - Iron_Block (≥ 2 lines)          → "region"
 *   - Iron_MatchStmt (≥ 2 lines)      → "region"
 *   - consecutive Iron_ImportDecl run → "imports"
 *   - consecutive IRON_TOK_DOC_COMMENT run (≥ 2 lines) → "comment"
 *
 * Lines are 1-indexed in Iron_Span and 0-indexed in LSP; we subtract
 * one at emit time. Single-line spans (start == end) are filtered out
 * per D-13 "skip single-line blocks" rule.
 *
 * Arena discipline: the walker creates its own per-request 64KB
 * Iron_Arena for the lex+parse; the caller's `arena` argument owns
 * the returned IronLsp_FoldingRange array. */

#include "lsp/facade/edit/folding_range.h"

#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"    /* IRON_ANALYSIS_MODE_LSP */
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Emit one fold row into a caller-maintained growing array. Returns
 * false only on allocation failure. Skips single-line spans. */
static bool emit_fold(IronLsp_FoldingRange **arr,
                       size_t                *n,
                       size_t                *cap,
                       Iron_Arena            *arena,
                       uint32_t               start_1based,
                       uint32_t               end_1based,
                       const char            *kind) {
    if (start_1based == 0 || end_1based == 0) return true;  /* skip unknown */
    if (start_1based >= end_1based) return true;           /* single line */
    /* One fold per line range (#362): a body block and the statement or
     * declaration around it can span the same lines. */
    for (size_t i = 0; i < *n; i++) {
        if ((*arr)[i].start_line == start_1based - 1 &&
            (*arr)[i].end_line == end_1based - 1) return true;
    }
    if (*n == *cap) {
        size_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
        IronLsp_FoldingRange *next = (IronLsp_FoldingRange *)
            iron_arena_alloc(arena,
                              new_cap * sizeof(**arr),
                              _Alignof(IronLsp_FoldingRange));
        if (!next) return false;
        if (*arr && *n > 0) memcpy(next, *arr, (*n) * sizeof(**arr));
        *arr = next;
        *cap = new_cap;
    }
    (*arr)[*n].start_line = start_1based - 1;  /* LSP 0-based */
    (*arr)[*n].end_line   = end_1based   - 1;
    (*arr)[*n].kind       = kind;
    (*n)++;
    return true;
}

/* Recurse into a block / statement body, emitting folds for any Iron_Block
 * whose span is ≥ 2 lines, plus any nested structural spans. */
static void visit_body(Iron_Node             *n,
                        IronLsp_FoldingRange **arr,
                        size_t                *count,
                        size_t                *cap,
                        Iron_Arena            *arena) {
    if (!n || n->kind == IRON_NODE_ERROR) return;
    switch ((int)n->kind) {
        case IRON_NODE_BLOCK: {
            Iron_Block *b = (Iron_Block *)n;
            (void)emit_fold(arr, count, cap, arena,
                             b->span.line, b->span.end_line, "region");
            for (int i = 0; i < b->stmt_count; i++)
                visit_body(b->stmts[i], arr, count, cap, arena);
            break;
        }
        case IRON_NODE_IF: {
            Iron_IfStmt *s = (Iron_IfStmt *)n;
            visit_body(s->body, arr, count, cap, arena);
            for (int i = 0; i < s->elif_count; i++)
                visit_body(s->elif_bodies[i], arr, count, cap, arena);
            visit_body(s->else_body, arr, count, cap, arena);
            break;
        }
        case IRON_NODE_WHILE: {
            Iron_WhileStmt *w = (Iron_WhileStmt *)n;
            visit_body(w->body, arr, count, cap, arena);
            break;
        }
        case IRON_NODE_FOR: {
            Iron_ForStmt *f = (Iron_ForStmt *)n;
            visit_body(f->body, arr, count, cap, arena);
            break;
        }
        case IRON_NODE_MATCH: {
            Iron_MatchStmt *m = (Iron_MatchStmt *)n;
            /* Multi-line match expression itself is a fold target. */
            (void)emit_fold(arr, count, cap, arena,
                             m->span.line, m->span.end_line, "region");
            for (int i = 0; i < m->case_count; i++)
                visit_body(m->cases[i], arr, count, cap, arena);
            visit_body(m->else_body, arr, count, cap, arena);
            break;
        }
        case IRON_NODE_MATCH_CASE: {
            Iron_MatchCase *mc = (Iron_MatchCase *)n;
            visit_body(mc->body, arr, count, cap, arena);
            break;
        }
        case IRON_NODE_SPAWN: {
            Iron_SpawnStmt *sn = (Iron_SpawnStmt *)n;
            visit_body(sn->body, arr, count, cap, arena);
            break;
        }
        default:
            break;
    }
}

/* Folds every run of own-line comment lines (`//`, and `--` / `///`
 * while code migrates) on consecutive lines when it spans >= 2 lines.
 * A `//` line comes as a NEWLINE token carrying the comment, a `///` line
 * as a DOC_COMMENT token; both carry their 1-based line. (The old walk
 * looked for DOC_COMMENT tokens back to back, but a NEWLINE always sits
 * between two of them, so no comment run was ever folded.) */
static void emit_doc_comment_folds(Iron_Token            *tokens,
                                     int                    tok_count,
                                     IronLsp_FoldingRange **arr,
                                     size_t                *count,
                                     size_t                *cap,
                                     Iron_Arena            *arena) {
    if (!tokens || tok_count <= 0) return;
    uint32_t first_line = 0, last_line = 0;
    for (int i = 0; i <= tok_count; i++) {
        bool is_line = false;
        uint32_t line = 0;
        if (i < tok_count) {
            const Iron_Token *t = &tokens[i];
            if ((t->comment & IRON_COMMENT_PRESENT) &&
                (t->comment & IRON_COMMENT_OWN_LINE) &&
                !(t->comment & IRON_COMMENT_MARKER)) {
                is_line = true;
                line = t->line;
            } else if (t->kind == IRON_TOK_NEWLINE && t->comment == 0) {
                continue;  /* the newline after a `///` line, or a blank line */
            }
        }
        if (is_line && first_line != 0 && line == last_line + 1) {
            last_line = line;
            continue;
        }
        if (first_line != 0 && first_line < last_line) {
            (void)emit_fold(arr, count, cap, arena, first_line, last_line, "comment");
        }
        first_line = is_line ? line : 0;
        last_line  = is_line ? line : 0;
    }
}

void ilsp_facade_folding_range(struct IronLsp_Server   *server,
                                 struct IronLsp_Document *doc,
                                 _Atomic bool            *cancel,
                                 Iron_Arena              *arena,
                                 IronLsp_FoldingRange   **out,
                                 size_t                  *out_n) {
    if (out)   *out   = NULL;
    if (out_n) *out_n = 0;
    if (!doc || !arena || !out || !out_n) return;
    (void)server;  /* reserved for future workspace_index queries */

    if (cancel && atomic_load(cancel)) return;
    if (!doc->text || doc->text_len == 0) return;

    /* Parser-only pipeline: fresh lex + parse, no analyzer seam. */
    Iron_Arena    parse_arena = iron_arena_create(128 * 1024);
    Iron_DiagList diags       = iron_diaglist_create();

    Iron_Lexer lex = iron_lexer_create(doc->text,
                                         doc->uri ? doc->uri : "<doc>",
                                         &parse_arena, &diags);
    iron_lexer_set_cancel_flag(&lex, cancel);
    Iron_Token *tokens = iron_lex_all(&lex);
    if (!tokens) goto cleanup;
    int tok_count = (int)arrlen(tokens);

    Iron_Parser p = iron_parser_create(tokens, tok_count, doc->text,
                                         doc->uri ? doc->uri : "<doc>",
                                         &parse_arena, &diags);
    iron_parser_set_mode(&p, IRON_ANALYSIS_MODE_LSP);
    iron_parser_set_cancel_flag(&p, cancel);
    Iron_Node *root = iron_parse(&p);
    if (!root || root->kind != IRON_NODE_PROGRAM) goto cleanup;
    if (cancel && atomic_load(cancel)) goto cleanup;

    Iron_Program *program = (Iron_Program *)root;

    IronLsp_FoldingRange *arr = NULL;
    size_t arr_n = 0, arr_cap = 0;

    /* Pass 1: walk program->decls[]. For each top-level decl:
     *   - consecutive IRON_NODE_IMPORT_DECL runs -> single "imports" fold
     *   - Iron_FuncDecl / Iron_MethodDecl bodies -> "region" plus
     *     recursive visit_body for nested blocks/match/if/while/for
     *   - Iron_ObjectDecl / Iron_InterfaceDecl / Iron_EnumDecl decl
     *     spans -> "region" (covers the whole decl including braces)
     *   - Iron_ErrorNode is skipped (per D-13) */
    int i = 0;
    while (i < program->decl_count) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind == IRON_NODE_ERROR) { i++; continue; }

        if (d->kind == IRON_NODE_IMPORT_DECL) {
            /* Run-collapse consecutive imports. */
            uint32_t first_line = d->span.line;
            uint32_t last_line  = d->span.end_line;
            int j = i + 1;
            while (j < program->decl_count) {
                Iron_Node *nxt = program->decls[j];
                if (!nxt || nxt->kind == IRON_NODE_ERROR) { j++; continue; }
                if (nxt->kind != IRON_NODE_IMPORT_DECL) break;
                last_line = nxt->span.end_line;
                j++;
            }
            if (first_line < last_line) {
                (void)emit_fold(&arr, &arr_n, &arr_cap, arena,
                                 first_line, last_line, "imports");
            }
            i = j;
            continue;
        }

        switch ((int)d->kind) {
            case IRON_NODE_FUNC_DECL: {
                Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
                /* The body is a block: visit_body emits its fold. */
                if (fd->body) visit_body(fd->body, &arr, &arr_n, &arr_cap, arena);
                break;
            }
            case IRON_NODE_METHOD_DECL: {
                Iron_MethodDecl *md = (Iron_MethodDecl *)d;
                /* The body is a block: visit_body emits its fold. */
                if (md->body) visit_body(md->body, &arr, &arr_n, &arr_cap, arena);
                break;
            }
            case IRON_NODE_OBJECT_DECL: {
                /* Object decl span covers { field: Type; ... } region. */
                (void)emit_fold(&arr, &arr_n, &arr_cap, arena,
                                 d->span.line, d->span.end_line, "region");
                break;
            }
            case IRON_NODE_INTERFACE_DECL: {
                (void)emit_fold(&arr, &arr_n, &arr_cap, arena,
                                 d->span.line, d->span.end_line, "region");
                break;
            }
            case IRON_NODE_ENUM_DECL: {
                (void)emit_fold(&arr, &arr_n, &arr_cap, arena,
                                 d->span.line, d->span.end_line, "region");
                break;
            }
            default:
                break;
        }
        i++;
    }

    /* Pass 2: multi-line `///` doc-comment runs from the token stream.
     * Note: regular `//` non-doc comments are NOT tokenized into the
     * Iron stream (lexer consumes them without emitting tokens); those
     * folds would require a secondary text scan which is deferred. */
    emit_doc_comment_folds(tokens, tok_count, &arr, &arr_n, &arr_cap, arena);

    *out   = arr_n > 0 ? arr : NULL;
    *out_n = arr_n;

cleanup:
    if (tokens) arrfree(tokens);
    iron_diaglist_free(&diags);
    iron_arena_free(&parse_arena);
}
