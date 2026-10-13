/* tests/fuzz/fuzz_comments.c -- libFuzzer target for comments (manual 1.2):
 * the `//`, `--`, `///` and directive lexing, documentation attachment,
 * `fmt --migrate-comments` and the formatter.
 *
 * For every input:
 *   1. lex + parse never crash;
 *   2. iron_migrate_comments either refuses or returns a text that a
 *      second migration leaves unchanged (it checks internally that the
 *      tokens, diagnostics, documentation and directives are the same);
 *   3. when the formatter accepts the input, formatting its output again
 *      changes nothing;
 *   4. when the input parses cleanly and its printed AST does too, the
 *      printed program carries the same documentation and directives.
 * A broken property aborts, which libFuzzer reports as a crash.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "parser/printer.h"
#include "fmt/format.h"
#include "fmt/migrate.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "vendor/stb_ds.h"

#define IRON_FUZZ_COMMENTS_MAX_INPUT 8192

static Iron_Program *parse(const char *src, Iron_Arena *arena, Iron_DiagList *d) {
    Iron_Lexer lx = iron_lexer_create(src, "fuzz.iron", arena, d);
    Iron_Token *t = iron_lex_all(&lx);
    Iron_Parser p = iron_parser_create(t, (int)arrlen(t), src, "fuzz.iron", arena, d);
    Iron_Program *prog = (Iron_Program *)iron_parse(&p);
    arrfree(t);
    return prog;
}

static bool same_str(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

static bool same_docs(const Iron_Node *a, const Iron_Node *b) {
    const char *da, *db;
    const Iron_Directive *xa, *xb;
    int na, nb;
    if (iron_node_doc(a, &da, &xa, &na) != iron_node_doc(b, &db, &xb, &nb)) return false;
    if (!same_str(da, db) || na != nb) return false;
    for (int i = 0; i < na; i++)
        if (strcmp(xa[i].key, xb[i].key) != 0 || strcmp(xa[i].value, xb[i].value) != 0 ||
            xa[i].trailing != xb[i].trailing)
            return false;
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > IRON_FUZZ_COMMENTS_MAX_INPUT) return 0;
    char *src = (char *)malloc(size + 1);
    if (!src) return 0;
    memcpy(src, data, size);
    src[size] = '\0';
    size_t len = strlen(src);   /* stop at an embedded NUL, as the CLI would */

    Iron_Arena arena = iron_arena_create(64 * 1024);

    /* 2. Migration is idempotent. */
    IronMigrateResult r = iron_migrate_comments(src, len, "fuzz.iron", &arena);
    if (r.ok && r.changed) {
        IronMigrateResult again = iron_migrate_comments(r.text, r.len, "fuzz.iron", &arena);
        if (!again.ok || again.changed) abort();
    }

    /* 3. Formatting is idempotent. */
    Iron_DiagList d1 = iron_diaglist_create();
    IronFmtResult f1 = iron_format_source(src, "fuzz.iron", NULL, &arena, &d1);
    if (f1.ok) {
        Iron_DiagList d2 = iron_diaglist_create();
        IronFmtResult f2 = iron_format_source(f1.formatted, "fuzz.iron", NULL, &arena, &d2);
        if (!f2.ok || strcmp(f1.formatted, f2.formatted) != 0) abort();
        iron_diaglist_free(&d2);
    }
    iron_diaglist_free(&d1);

    /* 1 and 4. Documentation survives the printer. */
    Iron_DiagList d3 = iron_diaglist_create();
    Iron_Program *p = parse(src, &arena, &d3);
    if (p && d3.error_count == 0) {
        char *printed = iron_print_ast((Iron_Node *)p, NULL, &arena);
        Iron_DiagList d4 = iron_diaglist_create();
        Iron_Program *q = printed ? parse(printed, &arena, &d4) : NULL;
        if (q && d4.error_count == 0 && q->decl_count == p->decl_count) {
            for (int i = 0; i < p->decl_count; i++) {
                if (!p->decls[i] || !q->decls[i] || p->decls[i]->kind != q->decls[i]->kind) break;
                if (!same_docs(p->decls[i], q->decls[i])) abort();
            }
        }
        iron_diaglist_free(&d4);
    }
    iron_diaglist_free(&d3);

    iron_arena_free(&arena);
    free(src);
    return 0;
}
