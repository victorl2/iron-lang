#ifndef ILSP_FACADE_FMT_FORMAT_INTERNAL_H
#define ILSP_FACADE_FMT_FORMAT_INTERNAL_H

/* Phase 5 Plan 05-03 / 05-04 internal API -- lex+parse-only helper for
 * sub-node printers.
 *
 * Range-formatting (Plan 05-03) and on-type-formatting (Plan 05-04)
 * both need to LEX+PARSE the document buffer to walk
 * Iron_Program.decls[] (range) or descend the AST to find an enclosing
 * block (on-type). They do NOT print the whole program -- they
 * re-render selected sub-nodes only.
 *
 * To preserve the D-10 single compile-format-primitive call site we
 * expose a lex+parse-only helper here; sibling TUs include this
 * header instead of the public fmt/format.h, so the grep invariant
 * stays at exactly one hit.
 *
 * This header is PRIVATE to src/lsp/facade/fmt/ -- no installation,
 * no use from src/lsp/server/ or elsewhere. */

#include <stdatomic.h>
#include <stdbool.h>

#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "util/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

struct IronLsp_Document;

typedef struct IronFmtParseResult {
    Iron_Node *program;       /* IRON_NODE_PROGRAM root, or NULL on refuse */
    bool       ok;            /* false on lex/parse error or cancel */
    int        error_count;
} IronFmtParseResult;

/* Lex + parse the document buffer; refuse on any lexer/parser error.
 * Tokens are arrfree'd before returning. Returns the AST root for
 * walker consumption. Caller owns arena and diags.
 *
 * NOTE: this function does NOT reach the compiler-side format primitive
 * -- the range and on-type walkers print sub-nodes themselves via
 * iron_print_ast, keeping the single compile-time format-primitive site
 * in format.c (used only by _full). */
IronFmtParseResult ilsp_facade_fmt_lex_parse(
    const struct IronLsp_Document *doc,
    Iron_Arena                    *arena,
    Iron_DiagList                 *diags,
    const _Atomic bool            *cancel);

#ifdef __cplusplus
}
#endif

#endif /* ILSP_FACADE_FMT_FORMAT_INTERNAL_H */
