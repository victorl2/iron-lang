/* `//` comments, Go style documentation, directive comments, compiler
 * hints and `fmt --migrate-comments` (manual section 1.2). */
#include "unity.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "parser/printer.h"
#include "fmt/migrate.h"
#include "fmt/format.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "stb_ds.h"

#include <stdio.h>
#include <string.h>

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(262144);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

static Iron_Token *lex(const char *src, int *count) {
    Iron_Lexer l = iron_lexer_create(src, "t.iron", &arena, &diags);
    Iron_Token *t = iron_lex_all(&l);
    *count = (int)arrlen(t);
    return t;
}

static Iron_Program *parse(const char *src) {
    int n;
    Iron_Token *t = lex(src, &n);
    Iron_Parser p = iron_parser_create(t, n, src, "t.iron", &arena, &diags);
    Iron_Program *prog = (Iron_Program *)iron_parse(&p);
    arrfree(t);
    return prog;
}

static int count_code(int code) {
    int c = 0;
    for (int i = 0; i < diags.count; i++)
        if (diags.items[i].code == code) c++;
    return c;
}

/* ── Lexer ───────────────────────────────────────────────────────────────── */

static void test_lex_slash_comment_is_newline_with_payload(void) {
    int n;
    const char *src = "val a = 10 / 2 // half\n// own\n";
    Iron_Token *t = lex(src, &n);
    /* val a = 10 / 2 NEWLINE(comment) NEWLINE(comment) EOF */
    TEST_ASSERT_EQUAL_INT(9, n);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_SLASH, t[4].kind);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_NEWLINE, t[6].kind);
    TEST_ASSERT_EQUAL_STRING(" half", t[6].value);
    TEST_ASSERT_TRUE(t[6].comment & IRON_COMMENT_PRESENT);
    TEST_ASSERT_FALSE(t[6].comment & IRON_COMMENT_OWN_LINE);
    TEST_ASSERT_EQUAL_UINT32(15, t[6].offset);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_NEWLINE, t[7].kind);
    TEST_ASSERT_TRUE(t[7].comment & IRON_COMMENT_OWN_LINE);
    TEST_ASSERT_EQUAL_STRING(" own", t[7].value);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    arrfree(t);
}

static void test_lex_spellings_and_directives(void) {
    int n;
    const char *src = "-- dash\n/// triple\n//json:id \r\n// json:id\n//Json:id\n";
    Iron_Token *t = lex(src, &n);
    TEST_ASSERT_TRUE(t[0].comment & IRON_COMMENT_DASH);
    TEST_ASSERT_EQUAL_STRING(" dash", t[0].value);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_DOC_COMMENT, t[1].kind);
    TEST_ASSERT_TRUE(t[1].comment & IRON_COMMENT_TRIPLE);
    TEST_ASSERT_EQUAL_STRING("triple", t[1].value);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_NEWLINE, t[2].kind);
    TEST_ASSERT_EQUAL_INT(0, t[2].comment);
    TEST_ASSERT_TRUE(t[3].comment & IRON_COMMENT_DIRECTIVE);
    TEST_ASSERT_EQUAL_STRING("json:id", t[3].value);   /* trailing space, \r gone */
    TEST_ASSERT_FALSE(t[4].comment & IRON_COMMENT_DIRECTIVE);
    TEST_ASSERT_FALSE(t[5].comment & IRON_COMMENT_DIRECTIVE);
    arrfree(t);
}

static void test_lex_markers_inside_strings_stay_text(void) {
    int n;
    const char *src = "val u = \"http://x.y/a//b -- c /// d //e:f\"\n";
    Iron_Token *t = lex(src, &n);
    TEST_ASSERT_EQUAL_INT(IRON_TOK_STRING, t[3].kind);
    TEST_ASSERT_EQUAL_STRING("http://x.y/a//b -- c /// d //e:f", t[3].value);
    TEST_ASSERT_EQUAL_INT(0, t[4].comment);
    arrfree(t);
}

static void test_lex_slash_file_marker(void) {
    int n;
    const char *src = "// @file: other.iron\nval x = 1\n";
    /* A file marker is honored in multi-file test mode; in user source it
     * is a comment like any other. */
    Iron_Token *plain = lex(src, &n);
    TEST_ASSERT_FALSE(plain[0].comment & IRON_COMMENT_MARKER);
    TEST_ASSERT_TRUE(plain[0].comment & IRON_COMMENT_PRESENT);
    arrfree(plain);
    iron_file_markers_set_test_mode(true);
    Iron_Token *t = lex(src, &n);
    iron_file_markers_set_test_mode(false);
    TEST_ASSERT_TRUE(t[0].comment & IRON_COMMENT_MARKER);
    TEST_ASSERT_EQUAL_STRING("other.iron", t[1].filename);
    arrfree(t);
}

/* ── Documentation ───────────────────────────────────────────────────────── */

static void test_doc_slash_run_attaches(void) {
    Iron_Program *p = parse("// first\n//\n//  indented\nfunc f() {}\n");
    Iron_FuncDecl *f = (Iron_FuncDecl *)p->decls[0];
    TEST_ASSERT_EQUAL_STRING("first\n\n indented", f->doc_comment);
    TEST_ASSERT_EQUAL_INT(0, f->directive_count);
}

static void test_doc_blank_line_breaks(void) {
    Iron_Program *p = parse("// detached\n\nfunc f() {}\n");
    TEST_ASSERT_NULL(((Iron_FuncDecl *)p->decls[0])->doc_comment);
}

static void test_doc_trailing_comment_is_not_doc(void) {
    Iron_Program *p = parse("val a = 1 // about a\nfunc f() {}\n");
    TEST_ASSERT_NULL(((Iron_FuncDecl *)p->decls[1])->doc_comment);
}

static void test_doc_mixed_spellings(void) {
    Iron_Program *p = parse("-- dash\n/// triple\n// slash\nfunc f() {}\n");
    TEST_ASSERT_EQUAL_STRING("dash\ntriple\nslash",
                             ((Iron_FuncDecl *)p->decls[0])->doc_comment);
}

/* #651: the run was collected after `pub`, so pub declarations and pub
 * fields lost their documentation. */
static void test_doc_pub_decl_and_field(void) {
    Iron_Program *p = parse("/// adds\npub func add() {}\n"
                            "pub object P {\n    /// the x\n    pub val x: Int\n    // the y\n    val y: Int\n}\n");
    TEST_ASSERT_EQUAL_STRING("adds", ((Iron_FuncDecl *)p->decls[0])->doc_comment);
    Iron_ObjectDecl *o = NULL;
    for (int i = 0; i < p->decl_count; i++)
        if (p->decls[i]->kind == IRON_NODE_OBJECT_DECL) o = (Iron_ObjectDecl *)p->decls[i];
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_STRING("the x", ((Iron_Field *)o->fields[0])->doc_comment);
    TEST_ASSERT_EQUAL_STRING("the y", ((Iron_Field *)o->fields[1])->doc_comment);
}

/* #689: top-level bindings are declarations too; their collected run used
 * to be discarded because the AST had no documentation slots for them. */
static void test_doc_and_directives_on_top_level_bindings(void) {
    Iron_Program *p = parse("// The port.\n//config:env\nval port = 8080 //json:port\n"
                            "// Mutable state.\nvar state = 0\n");
    Iron_ValDecl *v = (Iron_ValDecl *)p->decls[0];
    TEST_ASSERT_EQUAL_STRING("The port.", v->doc_comment);
    TEST_ASSERT_EQUAL_INT(1, v->directive_count);
    TEST_ASSERT_EQUAL_STRING("config", v->directives[0].key);
    TEST_ASSERT_EQUAL_STRING("env", v->directives[0].value);
    TEST_ASSERT_FALSE(v->directives[0].trailing);
    Iron_VarDecl *w = (Iron_VarDecl *)p->decls[1];
    TEST_ASSERT_EQUAL_STRING("Mutable state.", w->doc_comment);
}

/* ── Directives ──────────────────────────────────────────────────────────── */

static void test_directives_above_and_trailing(void) {
    Iron_Program *p = parse("object Item {\n"
                            "    // The id.\n"
                            "    //db:primary_key\n"
                            "    pub val item_id: Int  //json:id\n"
                            "    val name: String // not:a directive (space)\n"
                            "}\n");
    Iron_ObjectDecl *o = (Iron_ObjectDecl *)p->decls[0];
    Iron_Field *f = (Iron_Field *)o->fields[0];
    TEST_ASSERT_EQUAL_STRING("The id.", f->doc_comment);
    TEST_ASSERT_EQUAL_INT(2, f->directive_count);
    TEST_ASSERT_EQUAL_STRING("db", f->directives[0].key);
    TEST_ASSERT_EQUAL_STRING("primary_key", f->directives[0].value);
    TEST_ASSERT_EQUAL_UINT32(3, f->directives[0].line);
    TEST_ASSERT_FALSE(f->directives[0].trailing);
    TEST_ASSERT_EQUAL_STRING("json", f->directives[1].key);
    TEST_ASSERT_EQUAL_STRING("id", f->directives[1].value);
    TEST_ASSERT_EQUAL_UINT32(4, f->directives[1].line);
    TEST_ASSERT_TRUE(f->directives[1].trailing);
    TEST_ASSERT_EQUAL_INT(0, ((Iron_Field *)o->fields[1])->directive_count);
    TEST_ASSERT_EQUAL_INT(0, o->directive_count);

    const char *doc; const Iron_Directive *d; int nd;
    TEST_ASSERT_TRUE(iron_node_doc((Iron_Node *)f, &doc, &d, &nd));
    TEST_ASSERT_EQUAL_INT(2, nd);

    Iron_Program *inline_p = parse("object Inline { val x: Int //json:x\n}\n");
    Iron_ObjectDecl *inline_o = (Iron_ObjectDecl *)inline_p->decls[0];
    Iron_Field *inline_f = (Iron_Field *)inline_o->fields[0];
    TEST_ASSERT_EQUAL_INT(1, inline_f->directive_count);
    TEST_ASSERT_TRUE(inline_f->directives[0].trailing);
}

static void test_directives_on_variants_sigs_and_decls(void) {
    Iron_Program *p = parse("//rpc:service\ninterface S {\n    //rpc:name\n    func m() -> Int\n}\n"
                            "enum E {\n    //tag:a\n    A,\n    B //tag:not_attached\n}\n"
                            "//inline:never\nfunc f() {} //inline:not_attached\n");
    Iron_InterfaceDecl *s = (Iron_InterfaceDecl *)p->decls[0];
    TEST_ASSERT_EQUAL_INT(1, s->directive_count);
    TEST_ASSERT_EQUAL_INT(1, ((Iron_FuncDecl *)s->method_sigs[0])->directive_count);
    Iron_EnumDecl *e = (Iron_EnumDecl *)p->decls[1];
    TEST_ASSERT_EQUAL_INT(1, ((Iron_EnumVariant *)e->variants[0])->directive_count);
    TEST_ASSERT_FALSE(((Iron_EnumVariant *)e->variants[0])->directives[0].trailing);
    TEST_ASSERT_EQUAL_INT(0, ((Iron_EnumVariant *)e->variants[1])->directive_count);
    Iron_FuncDecl *f = (Iron_FuncDecl *)p->decls[2];
    TEST_ASSERT_EQUAL_INT(1, f->directive_count);
    TEST_ASSERT_EQUAL_STRING("never", f->directives[0].value);
    TEST_ASSERT_FALSE(f->directives[0].trailing);
}

/* ── Compiler hints ──────────────────────────────────────────────────────── */

static void test_iron_fusible_hint(void) {
    Iron_Program *p = parse("//iron:fusible\nfunc f() {}\n@fusible\nfunc g() {}\nfunc h() {}\n");
    TEST_ASSERT_TRUE(((Iron_FuncDecl *)p->decls[0])->is_fusible);
    TEST_ASSERT_TRUE(((Iron_FuncDecl *)p->decls[1])->is_fusible);
    TEST_ASSERT_FALSE(((Iron_FuncDecl *)p->decls[2])->is_fusible);
    TEST_ASSERT_EQUAL_INT(0, count_code(IRON_WARN_UNKNOWN_IRON_HINT));
}

static void test_unknown_or_misplaced_hint_warns(void) {
    Iron_Program *p = parse("//iron:fusable\nfunc f() {}\n"
                            "//iron:fusible\nobject O {}\n"
                            "//iron:fusible\nval x = 1\n"
                            "func trailing() {} //iron:fusible\n");
    TEST_ASSERT_EQUAL_INT(4, count_code(IRON_WARN_UNKNOWN_IRON_HINT));
    Iron_FuncDecl *trailing = NULL;
    for (int i = 0; i < p->decl_count; i++) {
        if (p->decls[i]->kind != IRON_NODE_FUNC_DECL) continue;
        Iron_FuncDecl *f = (Iron_FuncDecl *)p->decls[i];
        if (f->name && strcmp(f->name, "trailing") == 0) trailing = f;
    }
    TEST_ASSERT_NOT_NULL(trailing);
    TEST_ASSERT_FALSE(trailing->is_fusible);
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
}

/* ── Printer ─────────────────────────────────────────────────────────────── */

static void test_printer_round_trips_docs_and_directives(void) {
    const char *src = "// doc\n//k:v\nfunc f() {} //t:not_attached\n"
                      "object O {\n    // field doc\n    val a: Int  //json:a\n}\n";
    Iron_Program *p = parse(src);
    char *out = iron_print_ast((Iron_Node *)p, NULL, &arena);
    TEST_ASSERT_NOT_NULL(strstr(out, "// doc\n//k:v\nfunc f() {\n"));
    TEST_ASSERT_NOT_NULL(strstr(out, "  // field doc\n  val a: Int  //json:a\n"));
    Iron_Program *q = parse(out);
    Iron_FuncDecl *f = (Iron_FuncDecl *)q->decls[0];
    TEST_ASSERT_EQUAL_STRING("doc", f->doc_comment);
    TEST_ASSERT_EQUAL_INT(1, f->directive_count);
    TEST_ASSERT_FALSE(f->directives[0].trailing);
}

static void test_stubs_carry_docs_and_directives(void) {
    Iron_Program *p = parse("// Adds.\n//k:v\npub func add(a: Int) -> Int { return a }\n"
                            "pub object P {\n    // x doc\n    //json:x\n    pub val x: Int\n}\n");
    FILE *f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    (void)iron_print_pub_stubs(p, f, "pkg", "0.1.0");
    rewind(f);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "// Adds.\n//k:v\npub func add(a: Int) -> Int {\n"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "    // x doc\n    //json:x\n    val x: Int\n"));
}

/* ── Migration ───────────────────────────────────────────────────────────── */

static void expect_migrate(const char *src, const char *want) {
    IronMigrateResult r = iron_migrate_comments(src, strlen(src), "t.iron", &arena);
    TEST_ASSERT_TRUE_MESSAGE(r.ok, r.error ? r.error : "not ok");
    TEST_ASSERT_EQUAL_STRING(want, r.text);
    /* Idempotent. */
    IronMigrateResult again = iron_migrate_comments(r.text, r.len, "t.iron", &arena);
    TEST_ASSERT_TRUE(again.ok);
    TEST_ASSERT_FALSE(again.changed);
}

static void test_migrate_rewrites_comment_tokens_only(void) {
    expect_migrate("-- head\n/// doc\nfunc f() {\n    val s = \"-- x /// y\"  -- trail\n    val d = 1 - -2\n}\n",
                   "// head\n// doc\nfunc f() {\n    val s = \"-- x /// y\"  // trail\n    val d = 1 - -2\n}\n");
}

static void test_migrate_keeps_meaning_of_odd_bodies(void) {
    /* `--json:x` and `///k:v` would read as directives, `--/` and `////`
     * as `///`: a space keeps them plain comments with the same text. */
    expect_migrate("--json:x\n---/\n--/x\n////q\n///k:v\nval a = 1\n",
                   "// json:x\n//-/\n// /x\n// /q\n// k:v\nval a = 1\n");
}

static void test_migrate_fusible(void) {
    expect_migrate("@fusible\nfunc f() {}\n    @fusible func g() {}\n",
                   "//iron:fusible\nfunc f() {}\n    //iron:fusible\n    func g() {}\n");
    const char *bad = "@fusible -- why\nfunc f() {}\n";
    IronMigrateResult r = iron_migrate_comments(bad, strlen(bad), "t.iron", &arena);
    TEST_ASSERT_FALSE(r.ok);
}

static void test_migrate_aligned_trailing_comments(void) {
    expect_migrate("object Q {\n    val a: Int   -- one\n    val bb: Int  -- two\n}\n",
                   "object Q {\n    val a: Int   // one\n    val bb: Int  // two\n}\n");
}

/* Found by comment_fuzz: a `///` body kept its trailing whitespace while a
 * `//` comment drops it, so the migrated text looked like a different doc
 * and the migration refused the file. */
static void test_migrate_triple_with_trailing_whitespace(void) {
    expect_migrate("val a = 1 ////q\t \n/// doc  \nfunc f() {}\n",
                   "val a = 1 // /q\t \n// doc  \nfunc f() {}\n");
    Iron_Program *p = parse("/// doc \t\nfunc f() {}\n");
    TEST_ASSERT_EQUAL_STRING("doc", ((Iron_FuncDecl *)p->decls[0])->doc_comment);
}

static void test_migrate_file_with_errors(void) {
    /* A file that does not parse migrates as long as it reports the same
     * diagnostics afterwards. */
    expect_migrate("-- broken\nfunc ( {\n", "// broken\nfunc ( {\n");
}

/* The formatter keeps comments as written and refuses a change to one. */
static void test_formatter_keeps_comment_forms(void) {
    const char *src = "func f() {\n      -- dash\n  // slash\n   /// triple\n}\n";
    IronFmtResult r = iron_format_source(src, "t.iron", NULL, &arena, &diags);
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("func f() {\n    -- dash\n    // slash\n    /// triple\n}\n", r.formatted);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_lex_slash_comment_is_newline_with_payload);
    RUN_TEST(test_lex_spellings_and_directives);
    RUN_TEST(test_lex_markers_inside_strings_stay_text);
    RUN_TEST(test_lex_slash_file_marker);
    RUN_TEST(test_doc_slash_run_attaches);
    RUN_TEST(test_doc_blank_line_breaks);
    RUN_TEST(test_doc_trailing_comment_is_not_doc);
    RUN_TEST(test_doc_mixed_spellings);
    RUN_TEST(test_doc_pub_decl_and_field);
    RUN_TEST(test_doc_and_directives_on_top_level_bindings);
    RUN_TEST(test_directives_above_and_trailing);
    RUN_TEST(test_directives_on_variants_sigs_and_decls);
    RUN_TEST(test_iron_fusible_hint);
    RUN_TEST(test_unknown_or_misplaced_hint_warns);
    RUN_TEST(test_printer_round_trips_docs_and_directives);
    RUN_TEST(test_stubs_carry_docs_and_directives);
    RUN_TEST(test_migrate_rewrites_comment_tokens_only);
    RUN_TEST(test_migrate_keeps_meaning_of_odd_bodies);
    RUN_TEST(test_migrate_fusible);
    RUN_TEST(test_migrate_aligned_trailing_comments);
    RUN_TEST(test_migrate_triple_with_trailing_whitespace);
    RUN_TEST(test_migrate_file_with_errors);
    RUN_TEST(test_formatter_keeps_comment_forms);
    return UNITY_END();
}
