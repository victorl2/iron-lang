/* test_v3_visibility_symbol_identity_drift -- Phase 10 Plan 10-01 (D-12).
 *
 * Visibility is metadata, not identity. Toggling `pub` (i.e., flipping
 * Iron_FuncDecl.is_private) MUST NOT change the FNV-1a hash of the
 * (canonical_path, name_path, kind) symbol identity triple. Otherwise
 * rename history breaks every time a user adds/removes pub.
 *
 * Field name notes (verified against src/parser/ast.h + src/analyzer/scope.h):
 *   - Iron_FuncDecl is an anonymous typedef struct -- its first two fields
 *     are { Iron_Span span; Iron_NodeKind kind; }. There is no `base`
 *     member; the structural-subtyping prefix is laid out inline.
 *   - Iron_Symbol uses `sym_kind` (Iron_SymbolKind), not `kind`.
 *   - The function symbol kind is IRON_SYM_FUNCTION (not IRON_SYMBOL_FUNC).
 *   - ilsp_symbol_id_derive's signature is
 *       (sym, canonical_path, program, arena)  -- 4 args.
 *     The drift-guard passes program=NULL because the function under test
 *     is a top-level Iron_FuncDecl whose name_path falls back to
 *     "<module>.<name>" (Phase 9 D-04 disjoint-encoding contract). */

#include "unity.h"

#include "analyzer/scope.h"
#include "lsp/facade/nav/symbol_id.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Build a minimal Iron_Symbol whose decl_node is a FuncDecl with the
 * given is_private bit. Same canonical_path + same name on both sides;
 * the only difference is the visibility bit. */
static Iron_Symbol make_func_symbol(Iron_FuncDecl *fd, bool is_private,
                                     const char *name) {
    memset(fd, 0, sizeof(*fd));
    fd->kind          = IRON_NODE_FUNC_DECL;
    fd->span.filename = "/abs/decl.iron";
    fd->span.line     = 1;
    fd->span.col      = 1;
    fd->name          = name;
    fd->is_private    = is_private;

    Iron_Symbol s;
    memset(&s, 0, sizeof(s));
    s.name       = name;
    s.sym_kind   = IRON_SYM_FUNCTION;
    s.decl_node  = (Iron_Node *)fd;
    s.span       = fd->span;
    s.is_private = is_private;
    return s;
}

static void test_pub_toggle_does_not_change_hash(void) {
    Iron_Arena arena = iron_arena_create(16 * 1024);

    Iron_FuncDecl fd_pub;
    Iron_FuncDecl fd_priv;
    Iron_Symbol sym_pub  = make_func_symbol(&fd_pub,  /*is_private=*/false, "foo");
    Iron_Symbol sym_priv = make_func_symbol(&fd_priv, /*is_private=*/true,  "foo");

    /* program=NULL: top-level FuncDecl falls back to "<module>.<name>"
     * via the name_path encoding (Phase 9 D-04). The hash MUST be
     * derived from (canonical_path, name_path, kind) only -- the
     * visibility bit lives on neither side of that triple. */
    IronLsp_SymbolId id_pub  = ilsp_symbol_id_derive(&sym_pub,  "/abs/decl.iron",
                                                       NULL, &arena);
    IronLsp_SymbolId id_priv = ilsp_symbol_id_derive(&sym_priv, "/abs/decl.iron",
                                                       NULL, &arena);

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(id_pub.hash, id_priv.hash,
        "D-12: FNV-1a hash MUST NOT change when pub keyword toggled");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(id_pub.name_path, id_priv.name_path,
        "D-12: name_path MUST NOT encode visibility");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)id_pub.kind, (int)id_priv.kind,
        "D-12: symbol kind MUST NOT change with visibility");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(id_pub.canonical_path,
                                      id_priv.canonical_path,
        "D-12: canonical_path MUST NOT encode visibility");

    iron_arena_free(&arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pub_toggle_does_not_change_hash);
    return UNITY_END();
}
