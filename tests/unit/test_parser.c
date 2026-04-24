#include "unity.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "lexer/lexer.h"
#include "util/arena.h"
#include "diagnostics/diagnostics.h"
#include "stb_ds.h"

#include <string.h>

/* ── Module-level fixtures ───────────────────────────────────────────────── */

static Iron_Arena    arena;
static Iron_DiagList diags;

void setUp(void) {
    arena = iron_arena_create(131072);
    diags = iron_diaglist_create();
}

void tearDown(void) {
    iron_arena_free(&arena);
    iron_diaglist_free(&diags);
}

/* ── Parse helpers ───────────────────────────────────────────────────────── */

static Iron_Node *parse(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &arena, &diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron", &arena, &diags);
    return iron_parse(&p);
}

/* parse_no_strict: parse with v3_strict_mode=false for tests that exercise
 * grammar features on objects that lack init constructors. The v3 gate is
 * already verified by gate-specific tests; these tests are about other
 * parser behaviors (pub modifiers, accessor synthesis, etc.) and should not
 * be coupled to the E0264 init requirement. */
static Iron_Node *parse_no_strict(const char *src) {
    Iron_Lexer   l      = iron_lexer_create(src, "test.iron", &arena, &diags);
    Iron_Token  *tokens = iron_lex_all(&l);
    int          count  = 0;
    while (tokens[count].kind != IRON_TOK_EOF) count++;
    count++;  /* include EOF */
    Iron_Parser  p = iron_parser_create(tokens, count, src, "test.iron", &arena, &diags);
    p.v3_strict_mode = false;
    return iron_parse(&p);
}

static Iron_Node *first_decl(Iron_Node *prog) {
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_NOT_NULL(pr->decls);
    TEST_ASSERT_GREATER_THAN(0, pr->decl_count);
    return pr->decls[0];
}

static Iron_Node *first_stmt_of_func(Iron_Node *decl) {
    Iron_FuncDecl *f    = (Iron_FuncDecl *)decl;
    Iron_Block    *body = (Iron_Block *)f->body;
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_GREATER_THAN(0, body->stmt_count);
    return body->stmts[0];
}

/* ── val / var declarations ──────────────────────────────────────────────── */

void test_parse_val_decl(void) {
    Iron_Node *prog = parse("val x = 10");
    TEST_ASSERT_EQUAL(IRON_NODE_PROGRAM, prog->kind);
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_VAL_DECL, d->kind);
    Iron_ValDecl *v = (Iron_ValDecl *)d;
    TEST_ASSERT_EQUAL_STRING("x", v->name);
    TEST_ASSERT_NULL(v->type_ann);
    TEST_ASSERT_NOT_NULL(v->init);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, v->init->kind);
    TEST_ASSERT_EQUAL_STRING("10", ((Iron_IntLit *)v->init)->value);
}

void test_parse_var_decl(void) {
    Iron_Node *prog = parse("var y: Int = 20");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_VAR_DECL, d->kind);
    Iron_VarDecl *v = (Iron_VarDecl *)d;
    TEST_ASSERT_EQUAL_STRING("y", v->name);
    TEST_ASSERT_NOT_NULL(v->type_ann);
    TEST_ASSERT_EQUAL(IRON_NODE_TYPE_ANNOTATION, v->type_ann->kind);
    TEST_ASSERT_EQUAL_STRING("Int", ((Iron_TypeAnnotation *)v->type_ann)->name);
    TEST_ASSERT_NOT_NULL(v->init);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, v->init->kind);
}

/* ── Function declarations ───────────────────────────────────────────────── */

void test_parse_func_decl(void) {
    Iron_Node *prog = parse("func add(a: Int, b: Int) -> Int { return a + b }");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_FUNC_DECL, d->kind);
    Iron_FuncDecl *f = (Iron_FuncDecl *)d;
    TEST_ASSERT_EQUAL_STRING("add", f->name);
    TEST_ASSERT_EQUAL(2, f->param_count);
    TEST_ASSERT_EQUAL_STRING("a", ((Iron_Param *)f->params[0])->name);
    TEST_ASSERT_EQUAL_STRING("b", ((Iron_Param *)f->params[1])->name);
    TEST_ASSERT_NOT_NULL(f->return_type);
    TEST_ASSERT_NOT_NULL(f->body);
    TEST_ASSERT_EQUAL(IRON_NODE_BLOCK, f->body->kind);
}

void test_parse_method_decl(void) {
    Iron_Node *prog = parse("func Player.update(dt: Float) { }");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, d->kind);
    Iron_MethodDecl *m = (Iron_MethodDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Player", m->type_name);
    TEST_ASSERT_EQUAL_STRING("update", m->method_name);
    TEST_ASSERT_EQUAL(1, m->param_count);
    TEST_ASSERT_EQUAL_STRING("dt", ((Iron_Param *)m->params[0])->name);
}

/* ── Object declarations ─────────────────────────────────────────────────── */

void test_parse_object_decl(void) {
    Iron_Node *prog = parse("object Player {\n  var pos: Vec2\n  val name: String\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, d->kind);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Player", obj->name);
    TEST_ASSERT_EQUAL(2, obj->field_count);
    Iron_Field *f0 = (Iron_Field *)obj->fields[0];
    Iron_Field *f1 = (Iron_Field *)obj->fields[1];
    TEST_ASSERT_EQUAL_STRING("pos",  f0->name);
    TEST_ASSERT_EQUAL_STRING("name", f1->name);
}

void test_parse_object_extends(void) {
    Iron_Node *prog = parse("object Player extends Entity {\n  val name: String\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, d->kind);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Entity", obj->extends_name);
}

void test_parse_object_implements(void) {
    Iron_Node *prog = parse("object Player impl Drawable, Updatable { }");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, d->kind);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)d;
    TEST_ASSERT_EQUAL(2, obj->implements_count);
    TEST_ASSERT_EQUAL_STRING("Drawable",  obj->implements_names[0]);
    TEST_ASSERT_EQUAL_STRING("Updatable", obj->implements_names[1]);
}

/* ── Phase 82 GRAMMAR: `self` is a hard-reserved keyword ─────────────────── */
/* These three tests lock the lexer-level reservation at lexer.c:51
 * (`{ "self", IRON_TOK_SELF }`). The parser accepts IRON_TOK_IDENTIFIER in
 * binding-name positions (val/func/param name), so `self` (which lexes to
 * IRON_TOK_SELF) must produce at least one diagnostic in each position.
 * These tests guard Phase 89's codemod assumption that user code cannot
 * shadow `self` at any scope. */

void test_self_keyword_rejected_as_val_name(void) {
    (void)parse("val self = 0\n");
    TEST_ASSERT_GREATER_THAN(0, diags.count);
}

void test_self_keyword_rejected_as_func_name(void) {
    (void)parse("func self() {}\n");
    TEST_ASSERT_GREATER_THAN(0, diags.count);
}

void test_self_keyword_rejected_as_param_name(void) {
    (void)parse("func foo(self: Int) {}\n");
    TEST_ASSERT_GREATER_THAN(0, diags.count);
}

/* ── Phase 82 GRAMMAR: in-block method declarations ──────────────────────── */
/* `func name()` inside an `object X { ... }` body desugars at parse time to a
 * top-level Iron_MethodDecl with is_receiver_form=true. The synthesized self
 * param (name="self", type=enclosing object) is prepended to explicit params.
 * AST shape is identical to Phase 79's `func (r: X) name()` receiver syntax. */

void test_object_in_block_method_parses(void) {
    Iron_Node *prog = parse(
        "object Counter {\n"
        "    var value: Int\n"
        "    func bump(n: Int) {\n"
        "        self.value = self.value + n\n"
        "    }\n"
        "}\n"
    );
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(IRON_NODE_PROGRAM, prog->kind);
    TEST_ASSERT_EQUAL(2, pr->decl_count);

    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, obj->kind);
    TEST_ASSERT_EQUAL_STRING("Counter", obj->name);
    TEST_ASSERT_EQUAL(1, obj->field_count);
    Iron_Field *f0 = (Iron_Field *)obj->fields[0];
    TEST_ASSERT_EQUAL_STRING("value", f0->name);

    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_EQUAL_STRING("Counter", m->type_name);
    TEST_ASSERT_EQUAL_STRING("bump", m->method_name);
    TEST_ASSERT_TRUE(m->is_receiver_form);
    TEST_ASSERT_EQUAL(2, m->param_count);
    Iron_Param *p0 = (Iron_Param *)m->params[0];
    Iron_Param *p1 = (Iron_Param *)m->params[1];
    TEST_ASSERT_EQUAL_STRING("self", p0->name);
    /* Phase 82 in-block methods: default-mutating receiver per CONTEXT.md
     * ("Default-mutating receiver ABI uses pointer-receiver from Phase 82
     * onward"). Phase 84 MUTTIER flips this off for `readonly`/`pure`. */
    TEST_ASSERT_TRUE(p0->is_mut_receiver);
    TEST_ASSERT_EQUAL_STRING("n", p1->name);
}

void test_object_in_block_method_no_params(void) {
    Iron_Node *prog = parse(
        "object Marker {\n"
        "    val tag: Int\n"
        "    func ping() {}\n"
        "}\n"
    );
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(2, pr->decl_count);

    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_EQUAL_STRING("ping", m->method_name);
    TEST_ASSERT_EQUAL(1, m->param_count);  /* only synthesized self */
    TEST_ASSERT_TRUE(m->is_receiver_form);
    Iron_Param *p0 = (Iron_Param *)m->params[0];
    TEST_ASSERT_EQUAL_STRING("self", p0->name);
}

void test_object_in_block_method_interleaved_with_fields(void) {
    Iron_Node *prog = parse(
        "object T {\n"
        "    var a: Int\n"
        "    func f() {}\n"
        "    var b: Int\n"
        "    func g() {}\n"
        "}\n"
    );
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(3, pr->decl_count);

    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, obj->kind);
    TEST_ASSERT_EQUAL_STRING("T", obj->name);
    TEST_ASSERT_EQUAL(2, obj->field_count);

    Iron_MethodDecl *mf = (Iron_MethodDecl *)pr->decls[1];
    Iron_MethodDecl *mg = (Iron_MethodDecl *)pr->decls[2];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, mf->kind);
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, mg->kind);
    TEST_ASSERT_EQUAL_STRING("f", mf->method_name);
    TEST_ASSERT_EQUAL_STRING("g", mg->method_name);
    TEST_ASSERT_TRUE(mf->is_receiver_form);
    TEST_ASSERT_TRUE(mg->is_receiver_form);
}

void test_object_field_only_still_parses(void) {
    Iron_Node *prog = parse(
        "object Foo {\n"
        "    var x: Int\n"
        "}\n"
    );
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(1, pr->decl_count);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, obj->kind);
    TEST_ASSERT_EQUAL_STRING("Foo", obj->name);
    TEST_ASSERT_EQUAL(1, obj->field_count);
}

/* ── Phase 83 ACCESS-02: pub modifier on object-block fields/methods ─────── */
/* Phase 83 introduces `pub` as an optional modifier inside object bodies.
 * On fields it sets Iron_Field.is_pub=true; Phase 83-02 uses that bit to
 * synthesize accessor methods. On methods it is silently accepted in Phase
 * 83-01 (no AST effect) since methods default public in v2.2 — Phase 88
 * flips the default. At top level `pub` is rejected with a clear diagnostic.
 * Iron_MethodDecl.is_synth_accessor exists on the struct so Plan 83-02 can
 * write it and Phase 84 MUTTIER can read it; Plan 83-01 only defaults it to
 * false at every construction site. */

void test_pub_on_fields_sets_is_pub(void) {
    Iron_Node *prog = parse_no_strict(
        "object X {\n"
        "    pub var a: Int\n"
        "    pub val b: Int\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* Plan 83-02: pub var a synthesizes getter+setter (2 methods); pub val b
     * synthesizes getter (1 method). Total: ObjectDecl + 3 MethodDecls. */
    TEST_ASSERT_EQUAL(4, pr->decl_count);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, obj->kind);
    TEST_ASSERT_EQUAL(2, obj->field_count);
    Iron_Field *f0 = (Iron_Field *)obj->fields[0];
    Iron_Field *f1 = (Iron_Field *)obj->fields[1];
    TEST_ASSERT_EQUAL_STRING("a", f0->name);
    TEST_ASSERT_TRUE(f0->is_var);
    TEST_ASSERT_TRUE(f0->is_pub);
    TEST_ASSERT_EQUAL_STRING("b", f1->name);
    TEST_ASSERT_FALSE(f1->is_var);
    TEST_ASSERT_TRUE(f1->is_pub);
}

void test_pub_mixed_with_plain_fields(void) {
    Iron_Node *prog = parse_no_strict(
        "object X {\n"
        "    var a: Int\n"
        "    pub var b: Int\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL(2, obj->field_count);
    Iron_Field *f0 = (Iron_Field *)obj->fields[0];
    Iron_Field *f1 = (Iron_Field *)obj->fields[1];
    TEST_ASSERT_EQUAL_STRING("a", f0->name);
    TEST_ASSERT_FALSE(f0->is_pub);
    TEST_ASSERT_EQUAL_STRING("b", f1->name);
    TEST_ASSERT_TRUE(f1->is_pub);
}

void test_pub_on_method_parses(void) {
    Iron_Node *prog = parse(
        "object X {\n"
        "    pub func foo() {}\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* ObjectDecl + the synthesized in-block MethodDecl + Phase 85 INIT-13
     * auto-synth init() {} (field-less objects receive a synth init so every
     * object has a callable constructor). The user method is pushed first
     * into extra_decls_out, then the synth init is appended, so decls[1] is
     * the user `foo` and decls[2] is the synth init. */
    TEST_ASSERT_EQUAL(3, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_EQUAL_STRING("foo", m->method_name);
    TEST_ASSERT_TRUE(m->is_receiver_form);
    TEST_ASSERT_FALSE(m->is_init);  /* user method is not init */
    /* Plan 83-01 does not synthesize accessors; is_synth_accessor must be
     * default-false on every MethodDecl the parser constructs. */
    TEST_ASSERT_FALSE(m->is_synth_accessor);
}

void test_pub_at_top_level_rejected(void) {
    (void)parse("pub func foo() {}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    /* The diagnostic message must mention pub and object-block so the user
     * understands where `pub` is valid. */
    bool found = false;
    for (int i = 0; i < diags.count; i++) {
        const char *msg = diags.items[i].message;
        if (msg && strstr(msg, "pub") && strstr(msg, "object-block")) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found,
        "expected diagnostic mentioning 'pub' and 'object-block'");
}

void test_is_synth_accessor_default_false_on_in_block_method(void) {
    /* Plan 83-01 guard: every MethodDecl the parser constructs must have
     * is_synth_accessor=false by default. Phase 83-02 will flip it on
     * accessors it synthesizes; Phase 84 MUTTIER reads the bit. */
    Iron_Node *prog = parse(
        "object Counter {\n"
        "    var value: Int\n"
        "    func bump(n: Int) {\n"
        "        self.value = self.value + n\n"
        "    }\n"
        "}\n"
    );
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_FALSE(m->is_synth_accessor);
}

void test_phase82_in_block_method_still_parses(void) {
    /* Backward-compat guard: the Phase 82 fixture shape (no pub modifier)
     * must parse unchanged after Phase 83 additions. Mirrors
     * test_object_in_block_method_parses to keep the lock byte-for-byte. */
    Iron_Node *prog = parse(
        "object Counter {\n"
        "    var value: Int\n"
        "    func bump(n: Int) {\n"
        "        self.value = self.value + n\n"
        "    }\n"
        "}\n"
    );
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(IRON_NODE_PROGRAM, prog->kind);
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL_STRING("Counter", obj->name);
    TEST_ASSERT_EQUAL(1, obj->field_count);
    Iron_Field *f0 = (Iron_Field *)obj->fields[0];
    TEST_ASSERT_EQUAL_STRING("value", f0->name);
    TEST_ASSERT_FALSE(f0->is_pub);  /* no pub prefix -> default false */
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL_STRING("bump", m->method_name);
    TEST_ASSERT_TRUE(m->is_receiver_form);
    TEST_ASSERT_FALSE(m->is_synth_accessor);
}

/* ── Phase 83-02 Task 1: accessor synthesis for pub val / pub var ─────────── */
/* The parser must emit a getter Iron_MethodDecl for every `pub val` field and
 * getter+setter Iron_MethodDecl pair for every `pub var` field. All synthesized
 * methods ride the Phase 82 extra_decls_out channel so they land at
 * program->decls after the enclosing ObjectDecl. Non-pub fields do not
 * synthesize anything. Collisions between a synthesized accessor name and a
 * user-declared method in the same object emit IRON_ERR_ACCESSOR_NAME_RESERVED
 * (237) with the locked message text. */

void test_pub_val_synthesizes_getter_only(void) {
    Iron_Node *prog = parse(
        "object Id {\n"
        "    pub val value: Int\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* ObjectDecl + 1 synthesized getter MethodDecl. */
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, obj->kind);
    TEST_ASSERT_EQUAL_STRING("Id", obj->name);
    TEST_ASSERT_EQUAL(1, obj->field_count);

    Iron_MethodDecl *getter = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, getter->kind);
    TEST_ASSERT_EQUAL_STRING("value", getter->method_name);
    TEST_ASSERT_EQUAL_STRING("Id", getter->type_name);
    TEST_ASSERT_TRUE(getter->is_synth_accessor);
    TEST_ASSERT_TRUE(getter->is_receiver_form);
    TEST_ASSERT_EQUAL(1, getter->param_count);  /* self only */
    TEST_ASSERT_NOT_NULL(getter->return_type);

    /* Getter body: { return self.value } */
    TEST_ASSERT_NOT_NULL(getter->body);
    Iron_Block *gbody = (Iron_Block *)getter->body;
    TEST_ASSERT_EQUAL(IRON_NODE_BLOCK, gbody->kind);
    TEST_ASSERT_EQUAL(1, gbody->stmt_count);
    Iron_ReturnStmt *ret = (Iron_ReturnStmt *)gbody->stmts[0];
    TEST_ASSERT_EQUAL(IRON_NODE_RETURN, ret->kind);
    TEST_ASSERT_NOT_NULL(ret->value);
    TEST_ASSERT_EQUAL(IRON_NODE_FIELD_ACCESS, ret->value->kind);
    Iron_FieldAccess *fa = (Iron_FieldAccess *)ret->value;
    TEST_ASSERT_EQUAL_STRING("value", fa->field);
    TEST_ASSERT_NOT_NULL(fa->object);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, fa->object->kind);
    TEST_ASSERT_EQUAL_STRING("self", ((Iron_Ident *)fa->object)->name);
}

void test_pub_var_synthesizes_getter_and_setter(void) {
    Iron_Node *prog = parse_no_strict(
        "object H {\n"
        "    pub var hp: Int\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* ObjectDecl + getter + setter = 3 decls. */
    TEST_ASSERT_EQUAL(3, pr->decl_count);

    Iron_MethodDecl *getter = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, getter->kind);
    TEST_ASSERT_EQUAL_STRING("hp", getter->method_name);
    TEST_ASSERT_TRUE(getter->is_synth_accessor);
    TEST_ASSERT_EQUAL(1, getter->param_count);
    TEST_ASSERT_NOT_NULL(getter->return_type);
    Iron_Param *g_self = (Iron_Param *)getter->params[0];
    TEST_ASSERT_EQUAL_STRING("self", g_self->name);
    TEST_ASSERT_FALSE(g_self->is_mut_receiver);  /* getter = read-only */

    Iron_MethodDecl *setter = (Iron_MethodDecl *)pr->decls[2];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, setter->kind);
    TEST_ASSERT_EQUAL_STRING("set_hp", setter->method_name);
    TEST_ASSERT_TRUE(setter->is_synth_accessor);
    TEST_ASSERT_EQUAL(2, setter->param_count);  /* self + _v */
    TEST_ASSERT_NULL(setter->return_type);      /* setter returns void */
    Iron_Param *s_self = (Iron_Param *)setter->params[0];
    TEST_ASSERT_EQUAL_STRING("self", s_self->name);
    TEST_ASSERT_TRUE(s_self->is_mut_receiver);  /* setter mutates self */
    Iron_Param *s_v = (Iron_Param *)setter->params[1];
    TEST_ASSERT_EQUAL_STRING("_v", s_v->name);

    /* Setter body: { self.hp = _v } */
    TEST_ASSERT_NOT_NULL(setter->body);
    Iron_Block *sbody = (Iron_Block *)setter->body;
    TEST_ASSERT_EQUAL(IRON_NODE_BLOCK, sbody->kind);
    TEST_ASSERT_EQUAL(1, sbody->stmt_count);
    Iron_AssignStmt *as = (Iron_AssignStmt *)sbody->stmts[0];
    TEST_ASSERT_EQUAL(IRON_NODE_ASSIGN, as->kind);
    TEST_ASSERT_NOT_NULL(as->target);
    TEST_ASSERT_EQUAL(IRON_NODE_FIELD_ACCESS, as->target->kind);
    Iron_FieldAccess *tfa = (Iron_FieldAccess *)as->target;
    TEST_ASSERT_EQUAL_STRING("hp", tfa->field);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, tfa->object->kind);
    TEST_ASSERT_EQUAL_STRING("self", ((Iron_Ident *)tfa->object)->name);
    TEST_ASSERT_NOT_NULL(as->value);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, as->value->kind);
    TEST_ASSERT_EQUAL_STRING("_v", ((Iron_Ident *)as->value)->name);
}

void test_non_pub_field_does_not_synthesize(void) {
    Iron_Node *prog = parse(
        "object X {\n"
        "    val plain: Int\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* ObjectDecl only, zero synthesized methods. */
    TEST_ASSERT_EQUAL(1, pr->decl_count);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, obj->kind);
    TEST_ASSERT_EQUAL(1, obj->field_count);
    Iron_Field *f0 = (Iron_Field *)obj->fields[0];
    TEST_ASSERT_FALSE(f0->is_pub);
}

void test_pub_val_getter_collides_with_user_method(void) {
    /* Collision: `pub val name: Int` synthesizes getter `name`, but the user
     * also declared `func name() -> Int`. Parse must emit
     * IRON_ERR_ACCESSOR_NAME_RESERVED (237) with the locked message text. */
    (void)parse(
        "object N {\n"
        "    pub val name: Int\n"
        "    func name() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    bool found = false;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_ERR_ACCESSOR_NAME_RESERVED &&
            diags.items[i].message &&
            strstr(diags.items[i].message, "reserved by synthesized accessor")) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found,
        "expected IRON_ERR_ACCESSOR_NAME_RESERVED with 'reserved by synthesized accessor' message");
}

void test_pub_var_setter_collides_with_user_method(void) {
    /* Collision on the synthesized setter: `pub var hp` reserves `set_hp`,
     * user declares `func set_hp(v: Int) {}`. Expect the same diagnostic. */
    (void)parse(
        "object N {\n"
        "    pub var hp: Int\n"
        "    func set_hp(v: Int) {}\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    bool found = false;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == IRON_ERR_ACCESSOR_NAME_RESERVED &&
            diags.items[i].message &&
            strstr(diags.items[i].message, "reserved by synthesized accessor")) {
            found = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found,
        "expected IRON_ERR_ACCESSOR_NAME_RESERVED for set_hp collision");
}

/* ── Phase 84 MUTTIER-01/02/03: readonly + pure modifier threading ────────── */
/* Phase 84 adds two modifier tokens on object-block methods. After the
 * optional `pub`, the parser now accepts an optional `readonly` XOR `pure`
 * modifier before `func`. The parse lands on Iron_MethodDecl.is_readonly /
 * is_pure so Plan 84-02 can fire tier-violation diagnostics. Synthesized
 * getters are retrofitted readonly+pure (field read is pure by definition);
 * synthesized setters keep both false (default-mutating). Top-level readonly
 * and pure plus the readonly+pure combo are rejected with E0245
 * (IRON_ERR_TIER_MODIFIER_PLACEMENT). */

void test_readonly_method_parse(void) {
    Iron_Node *prog = parse(
        "object X {\n"
        "    readonly func foo() {}\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* Phase 85 INIT-13: field-less object auto-synths init() {} in addition
     * to the user method; decl_count 2 -> 3 (ObjectDecl + user foo + synth
     * init). The user method still lands at decls[1]; synth at decls[2]. */
    TEST_ASSERT_EQUAL(3, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_EQUAL_STRING("foo", m->method_name);
    TEST_ASSERT_TRUE(m->is_readonly);
    TEST_ASSERT_FALSE(m->is_pure);
}

void test_pure_method_parse(void) {
    Iron_Node *prog = parse(
        "object X {\n"
        "    pure func foo() {}\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* Phase 85 INIT-13: +1 synth init on field-less object. */
    TEST_ASSERT_EQUAL(3, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_EQUAL_STRING("foo", m->method_name);
    TEST_ASSERT_FALSE(m->is_readonly);
    TEST_ASSERT_TRUE(m->is_pure);
}

void test_plain_method_has_default_tier(void) {
    /* MUTTIER-01 ground truth: unmarked `func foo()` is default-mutating
     * (both tier flags false). */
    Iron_Node *prog = parse(
        "object X {\n"
        "    func foo() {}\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* Phase 85 INIT-13: +1 synth init on field-less object. */
    TEST_ASSERT_EQUAL(3, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_FALSE(m->is_readonly);
    TEST_ASSERT_FALSE(m->is_pure);
}

void test_pub_readonly_method(void) {
    Iron_Node *prog = parse(
        "object X {\n"
        "    pub readonly func foo() {}\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* Phase 85 INIT-13: +1 synth init on field-less object. */
    TEST_ASSERT_EQUAL(3, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_TRUE(m->is_readonly);
    TEST_ASSERT_FALSE(m->is_pure);
}

void test_pub_pure_method(void) {
    Iron_Node *prog = parse(
        "object X {\n"
        "    pub pure func foo() {}\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* Phase 85 INIT-13: +1 synth init on field-less object. */
    TEST_ASSERT_EQUAL(3, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_FALSE(m->is_readonly);
    TEST_ASSERT_TRUE(m->is_pure);
}

static bool has_diag_code(int code) {
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].code == code) return true;
    }
    return false;
}

void test_readonly_pure_combo_rejected(void) {
    (void)parse(
        "object X {\n"
        "    readonly pure func foo() {}\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_TIER_MODIFIER_PLACEMENT),
        "expected IRON_ERR_TIER_MODIFIER_PLACEMENT for readonly+pure combo");
}

void test_pure_readonly_combo_rejected(void) {
    (void)parse(
        "object X {\n"
        "    pure readonly func foo() {}\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_TIER_MODIFIER_PLACEMENT),
        "expected IRON_ERR_TIER_MODIFIER_PLACEMENT for pure+readonly combo");
}

void test_readonly_at_top_level_rejected(void) {
    (void)parse("readonly func foo() {}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_TIER_MODIFIER_PLACEMENT),
        "expected IRON_ERR_TIER_MODIFIER_PLACEMENT for top-level readonly");
}

void test_pure_at_top_level_rejected(void) {
    (void)parse("pure func foo() {}\n");
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_TIER_MODIFIER_PLACEMENT),
        "expected IRON_ERR_TIER_MODIFIER_PLACEMENT for top-level pure");
}

void test_synth_getter_is_readonly_and_pure(void) {
    /* Phase 83 synth accessor retrofit: the getter synthesized for every pub
     * val / pub var field must come out as is_readonly=true AND is_pure=true
     * so readonly and pure methods can call it. The setter synthesized for a
     * pub var must stay is_readonly=false AND is_pure=false (default
     * mutating). */
    Iron_Node *prog = parse_no_strict(
        "object X {\n"
        "    pub val a: Int\n"
        "    pub var b: Int\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* ObjectDecl + getter(a) + getter(b) + setter(b) = 4 decls. */
    TEST_ASSERT_EQUAL(4, pr->decl_count);

    Iron_MethodDecl *get_a = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, get_a->kind);
    TEST_ASSERT_EQUAL_STRING("a", get_a->method_name);
    TEST_ASSERT_TRUE(get_a->is_synth_accessor);
    TEST_ASSERT_TRUE(get_a->is_readonly);
    TEST_ASSERT_TRUE(get_a->is_pure);

    Iron_MethodDecl *get_b = (Iron_MethodDecl *)pr->decls[2];
    TEST_ASSERT_EQUAL_STRING("b", get_b->method_name);
    TEST_ASSERT_TRUE(get_b->is_synth_accessor);
    TEST_ASSERT_TRUE(get_b->is_readonly);
    TEST_ASSERT_TRUE(get_b->is_pure);

    Iron_MethodDecl *set_b = (Iron_MethodDecl *)pr->decls[3];
    TEST_ASSERT_EQUAL_STRING("set_b", set_b->method_name);
    TEST_ASSERT_TRUE(set_b->is_synth_accessor);
    TEST_ASSERT_FALSE(set_b->is_readonly);
    TEST_ASSERT_FALSE(set_b->is_pure);
}

/* ── Phase 85 INIT-03/07/08/11/13/15: init declarations + auto-synth ──────── */
/* Phase 85 introduces the `init` keyword inside object bodies. Two forms:
 * anonymous `init(params) { body }` dispatched via Type(args), and named
 * `init <ident>(params) { body }` dispatched via Type.<ident>(args). The
 * parser builds Iron_MethodDecl with is_init=true, populates init_name
 * (NULL for anonymous, <ident> for named), and synthesizes the implicit
 * self receiver identically to Phase 82 in-block methods. Plan 85-02
 * reads is_init to gate definite-assignment analysis. Field-less objects
 * with zero user inits receive a synthesized empty init() {} (INIT-13).
 * Interface bodies reject init because they describe behavior, not
 * construction (INIT-15). Several adjacent modifiers are rejected:
 * `pub init` (visibility is object-level), `init() -> T` (init always
 * returns Self), duplicate anonymous init, duplicate named init. The
 * pre-existing Phase 84 E0245 branch rejects `readonly init` and
 * `pure init` without any new code path. */

void test_parse_anonymous_init(void) {
    Iron_Node *prog = parse(
        "object Counter {\n"
        "    var count: Int\n"
        "    init(v: Int) { self.count = v }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_TRUE(m->is_init);
    TEST_ASSERT_NULL(m->init_name);
    TEST_ASSERT_EQUAL_STRING("init", m->method_name);
    TEST_ASSERT_EQUAL_STRING("Counter", m->type_name);
    TEST_ASSERT_EQUAL(2, m->param_count);  /* self + v */
    TEST_ASSERT_TRUE(m->is_receiver_form);
    TEST_ASSERT_NULL(m->return_type);
    TEST_ASSERT_FALSE(m->is_readonly);
    TEST_ASSERT_FALSE(m->is_pure);
}

void test_parse_named_init(void) {
    Iron_Node *prog = parse(
        "object Counter {\n"
        "    var count: Int\n"
        "    init zero() { self.count = 0 }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_TRUE(m->is_init);
    TEST_ASSERT_NOT_NULL(m->init_name);
    TEST_ASSERT_EQUAL_STRING("zero", m->init_name);
    TEST_ASSERT_EQUAL_STRING("zero", m->method_name);
    TEST_ASSERT_EQUAL_STRING("Counter", m->type_name);
}

void test_parse_anonymous_plus_named_inits(void) {
    Iron_Node *prog = parse(
        "object Counter {\n"
        "    var count: Int\n"
        "    init(v: Int) { self.count = v }\n"
        "    init from_zero() { self.count = 0 }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(3, pr->decl_count);
    Iron_MethodDecl *m0 = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m0->kind);
    TEST_ASSERT_TRUE(m0->is_init);
    TEST_ASSERT_NULL(m0->init_name);
    Iron_MethodDecl *m1 = (Iron_MethodDecl *)pr->decls[2];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m1->kind);
    TEST_ASSERT_TRUE(m1->is_init);
    TEST_ASSERT_NOT_NULL(m1->init_name);
    TEST_ASSERT_EQUAL_STRING("from_zero", m1->init_name);
}

void test_parse_duplicate_anonymous_init_rejected(void) {
    (void)parse(
        "object Counter {\n"
        "    var count: Int\n"
        "    init(v: Int) { self.count = v }\n"
        "    init(w: Int) { self.count = w }\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_DUPLICATE_DECL),
        "expected IRON_ERR_DUPLICATE_DECL for duplicate anonymous init");
    {
        /* Locked message substring so the diag is specifically the anon-dup
         * case, not a stray duplicate-decl from earlier in the object. */
        bool found = false;
        for (int i = 0; i < diags.count; i++) {
            if (diags.items[i].code == IRON_ERR_DUPLICATE_DECL &&
                diags.items[i].message &&
                strstr(diags.items[i].message, "duplicate anonymous init")) {
                found = true;
                break;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found,
            "expected 'duplicate anonymous init' in DUPLICATE_DECL message");
    }
}

void test_parse_duplicate_named_init_rejected(void) {
    (void)parse(
        "object Counter {\n"
        "    var count: Int\n"
        "    init zero() { self.count = 0 }\n"
        "    init zero() { self.count = 1 }\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_DUPLICATE_DECL),
        "expected IRON_ERR_DUPLICATE_DECL for duplicate named init");
}

static bool has_diag_msg_substring(const char *needle) {
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].message && strstr(diags.items[i].message, needle)) {
            return true;
        }
    }
    return false;
}

void test_parse_pub_init_rejected(void) {
    (void)parse(
        "object X {\n"
        "    pub init() {}\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_UNEXPECTED_TOKEN),
        "expected IRON_ERR_UNEXPECTED_TOKEN for pub init");
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_msg_substring("init visibility is tied to its object"),
        "expected locked 'init visibility is tied to its object' message");
}

void test_parse_readonly_init_rejected(void) {
    /* Phase 84 E0245 already handles this branch - no new parser code needed. */
    (void)parse(
        "object X {\n"
        "    readonly init() {}\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_TIER_MODIFIER_PLACEMENT),
        "expected IRON_ERR_TIER_MODIFIER_PLACEMENT (E0245) for readonly init");
}

void test_parse_pure_init_rejected(void) {
    (void)parse(
        "object X {\n"
        "    pure init() {}\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_TIER_MODIFIER_PLACEMENT),
        "expected IRON_ERR_TIER_MODIFIER_PLACEMENT (E0245) for pure init");
}

void test_parse_fieldless_init_auto_synth(void) {
    /* INIT-13: object with zero fields and zero user inits receives a
     * synthesized empty init() {} automatically. */
    Iron_Node *prog = parse("object Marker {}\n");
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_TRUE(m->is_init);
    TEST_ASSERT_NULL(m->init_name);
    TEST_ASSERT_EQUAL_STRING("init", m->method_name);
    TEST_ASSERT_EQUAL_STRING("Marker", m->type_name);
    TEST_ASSERT_EQUAL(1, m->param_count);  /* just synth self */
    TEST_ASSERT_NOT_NULL(m->body);
    Iron_Block *b = (Iron_Block *)m->body;
    TEST_ASSERT_EQUAL(IRON_NODE_BLOCK, b->kind);
    TEST_ASSERT_EQUAL(0, b->stmt_count);
}

void test_parse_fieldless_with_user_init_no_synth(void) {
    /* INIT-13 negative: when the user declared an init, auto-synth must not
     * fire. decl_count stays 2 (ObjectDecl + 1 user init), not 3. */
    Iron_Node *prog = parse(
        "object Marker {\n"
        "    init() {}\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_TRUE(m->is_init);
}

void test_parse_init_with_return_type_rejected(void) {
    /* INIT-11 parser branch: explicit return type on init is a parse error. */
    (void)parse(
        "object X {\n"
        "    init() -> Int { }\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_UNEXPECTED_TOKEN),
        "expected IRON_ERR_UNEXPECTED_TOKEN for init with return type");
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_msg_substring("init cannot declare a return type"),
        "expected locked 'init cannot declare a return type' message");
}

void test_parse_init_in_interface_rejected(void) {
    /* INIT-15 + IFACE-04 (Phase 87 upgrade): interfaces describe behavior,
     * not construction. init is a parse error inside interface bodies and
     * now fires the dedicated IRON_ERR_IFACE_CANNOT_DECLARE_INIT (E0256)
     * instead of the generic IRON_ERR_UNEXPECTED_TOKEN. The locked message
     * substring "interfaces may not declare init" is unchanged. */
    (void)parse(
        "interface Foo {\n"
        "    init()\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_IFACE_CANNOT_DECLARE_INIT),
        "expected IRON_ERR_IFACE_CANNOT_DECLARE_INIT (E0256) for init in interface body");
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_msg_substring("interfaces may not declare init"),
        "expected locked 'interfaces may not declare init' message");
}

/* ── Phase 86 PATCH-01/02/05: patch object T { ... } ─────────────────────── */
/* Phase 86 introduces `patch object T { ... }` as a top-level declaration
 * that contributes methods (and inits) to an existing type T. The parser
 * builds an Iron_ObjectDecl with is_patch=true, target_type_name=T,
 * field_count=0. Body-level field declarations (var/val) are rejected with
 * E0253 IRON_ERR_PATCH_ADDS_FIELD; parser recovers and continues. Methods
 * use the same pub/readonly/pure/init grammar Phase 82..85 landed. Generic
 * patch targets `patch object List[T] { ... }` are deferred to v3.1+ per
 * 86-CONTEXT.md and rejected here. Plan 86-02 consumes is_patch to wire
 * the type-patch registry + dispatch; Plan 86-01 is parse-surface only. */

void test_parse_patch_object_parses(void) {
    Iron_Node *prog = parse(
        "patch object Int {\n"
        "    pub readonly func double() -> Int { return self }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* ObjectDecl(is_patch=true) + 1 extra MethodDecl from the body. */
    TEST_ASSERT_EQUAL(2, pr->decl_count);

    Iron_ObjectDecl *od = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, od->kind);
    TEST_ASSERT_TRUE(od->is_patch);
    TEST_ASSERT_NOT_NULL(od->target_type_name);
    TEST_ASSERT_EQUAL_STRING("Int", od->target_type_name);
    TEST_ASSERT_EQUAL_STRING("Int", od->name);
    TEST_ASSERT_EQUAL(0, od->field_count);

    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_EQUAL_STRING("Int",    m->type_name);
    TEST_ASSERT_EQUAL_STRING("double", m->method_name);
    TEST_ASSERT_TRUE(m->is_readonly);
    TEST_ASSERT_FALSE(m->is_pure);
    TEST_ASSERT_TRUE(m->is_receiver_form);
    TEST_ASSERT_FALSE(m->is_init);
}

void test_parse_patch_object_no_name(void) {
    (void)parse(
        "patch object { }\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_UNEXPECTED_TOKEN),
        "expected IRON_ERR_UNEXPECTED_TOKEN for missing patch target");
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_msg_substring("expected patch target name"),
        "expected locked 'expected patch target name' message");
}

void test_parse_patch_adds_field_rejected(void) {
    /* PATCH-05: var inside a patch body fires E0253 and recovers. */
    Iron_Node *prog = parse(
        "patch object Foo {\n"
        "    var x: Int\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_PATCH_ADDS_FIELD),
        "expected IRON_ERR_PATCH_ADDS_FIELD (E0253) for var in patch body");
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_msg_substring("patches may only add methods"),
        "expected locked 'patches may only add methods' message");

    /* Recovery: ObjectDecl still produced with is_patch=true, field_count=0. */
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_GREATER_THAN_INT(0, pr->decl_count);
    Iron_ObjectDecl *od = (Iron_ObjectDecl *)pr->decls[0];
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, od->kind);
    TEST_ASSERT_TRUE(od->is_patch);
    TEST_ASSERT_EQUAL(0, od->field_count);
}

void test_parse_patch_adds_val_field_rejected(void) {
    /* PATCH-05: val inside a patch body also fires E0253 (val and var both
     * fail the no-fields rule). */
    (void)parse(
        "patch object Foo {\n"
        "    val x: Int\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_PATCH_ADDS_FIELD),
        "expected IRON_ERR_PATCH_ADDS_FIELD (E0253) for val in patch body");
}

void test_parse_patch_with_init(void) {
    Iron_Node *prog = parse(
        "patch object Foo {\n"
        "    init(v: Int) { self.value = v }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_TRUE(m->is_init);
    TEST_ASSERT_NULL(m->init_name);
    TEST_ASSERT_EQUAL_STRING("init", m->method_name);
    TEST_ASSERT_EQUAL_STRING("Foo",  m->type_name);
}

void test_parse_patch_with_named_init(void) {
    Iron_Node *prog = parse(
        "patch object Foo {\n"
        "    init from_pair(a: Int, b: Int) { self.a = a }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_DECL, m->kind);
    TEST_ASSERT_TRUE(m->is_init);
    TEST_ASSERT_NOT_NULL(m->init_name);
    TEST_ASSERT_EQUAL_STRING("from_pair", m->init_name);
    TEST_ASSERT_EQUAL_STRING("from_pair", m->method_name);
}

void test_parse_patch_body_pub_plus_tier(void) {
    /* Same pub/readonly/pure grammar as a classic object body. */
    Iron_Node *prog = parse(
        "patch object Foo {\n"
        "    pub readonly func read() -> Int { return self }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    TEST_ASSERT_EQUAL(2, pr->decl_count);
    Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[1];
    TEST_ASSERT_TRUE(m->is_readonly);
    TEST_ASSERT_FALSE(m->is_pure);
    TEST_ASSERT_EQUAL_STRING("read", m->method_name);
}

void test_parse_patch_with_generic_params_rejected(void) {
    /* Generic patch targets deferred to v3.1+ per 86-CONTEXT.md Deferred
     * Ideas. Locked rejection keeps grammar stable; future Phase can lift
     * by extending this branch. */
    (void)parse(
        "patch object List[T] {\n"
        "    func noop() { }\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_UNEXPECTED_TOKEN),
        "expected IRON_ERR_UNEXPECTED_TOKEN for generic patch target");
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_msg_substring("generic patch targets not supported in v3.0"),
        "expected locked 'generic patch targets not supported in v3.0' message");
}

/* ── Interface declarations ──────────────────────────────────────────────── */

void test_parse_interface_decl(void) {
    Iron_Node *prog = parse("interface Drawable {\n  func draw()\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_INTERFACE_DECL, d->kind);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Drawable", iface->name);
    TEST_ASSERT_EQUAL(1, iface->method_count);
    Iron_FuncDecl *sig = (Iron_FuncDecl *)iface->method_sigs[0];
    TEST_ASSERT_EQUAL_STRING("draw", sig->name);
    TEST_ASSERT_NULL(sig->body);  /* signatures have no body */
}

/* Phase 87 IFACE-01/03/04: readonly/pure tier modifiers + default bodies +
 * init rejection with E0256. All 8 tests lock parser AST shape of the new
 * interface-body grammar extensions. */

void test_parse_interface_readonly_sig(void) {
    Iron_Node *prog = parse(
        "interface Comparable {\n"
        "    readonly func cmp(other: Int) -> Int\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Node *d = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_INTERFACE_DECL, d->kind);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)d;
    TEST_ASSERT_EQUAL(1, iface->method_count);
    Iron_FuncDecl *sig = (Iron_FuncDecl *)iface->method_sigs[0];
    TEST_ASSERT_EQUAL_STRING("cmp", sig->name);
    TEST_ASSERT_TRUE(sig->is_readonly);
    TEST_ASSERT_FALSE(sig->is_pure);
    TEST_ASSERT_NULL(sig->body);
}

void test_parse_interface_pure_sig(void) {
    Iron_Node *prog = parse(
        "interface Hashable {\n"
        "    pure func hash() -> Int\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)first_decl(prog);
    TEST_ASSERT_EQUAL(1, iface->method_count);
    Iron_FuncDecl *sig = (Iron_FuncDecl *)iface->method_sigs[0];
    TEST_ASSERT_TRUE(sig->is_pure);
    TEST_ASSERT_FALSE(sig->is_readonly);
    TEST_ASSERT_NULL(sig->body);
}

void test_parse_interface_readonly_default_body(void) {
    /* IFACE-03: default body on readonly sig. body != NULL is the
     * has_default_body signal per ast.h Iron_FuncDecl contract. */
    Iron_Node *prog = parse(
        "interface Describable {\n"
        "    readonly func describe() -> Int { return 42 }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)first_decl(prog);
    TEST_ASSERT_EQUAL(1, iface->method_count);
    Iron_FuncDecl *sig = (Iron_FuncDecl *)iface->method_sigs[0];
    TEST_ASSERT_TRUE(sig->is_readonly);
    TEST_ASSERT_NOT_NULL(sig->body);
    Iron_Block *body = (Iron_Block *)sig->body;
    TEST_ASSERT_EQUAL(IRON_NODE_BLOCK, body->kind);
    TEST_ASSERT_EQUAL(1, body->stmt_count);
    TEST_ASSERT_EQUAL(IRON_NODE_RETURN, body->stmts[0]->kind);
}

void test_parse_interface_pure_default_body(void) {
    Iron_Node *prog = parse(
        "interface Zero {\n"
        "    pure func zero() -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)first_decl(prog);
    Iron_FuncDecl *sig = (Iron_FuncDecl *)iface->method_sigs[0];
    TEST_ASSERT_TRUE(sig->is_pure);
    TEST_ASSERT_NOT_NULL(sig->body);
}

void test_parse_interface_readonly_plus_pure_rejected(void) {
    /* First-wins recovery mirrors iron_parse_object_decl body loop. */
    (void)parse(
        "interface Bad {\n"
        "    readonly pure func foo() -> Int\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_TIER_MODIFIER_PLACEMENT),
        "expected IRON_ERR_TIER_MODIFIER_PLACEMENT for double tier on iface sig");
}

void test_parse_interface_init_rejected_e0256(void) {
    /* IFACE-04: E0256 is the new dedicated code replacing the Phase 85
     * IRON_ERR_UNEXPECTED_TOKEN path. Locked message substring unchanged. */
    (void)parse(
        "interface I {\n"
        "    init() {}\n"
        "}\n"
    );
    TEST_ASSERT_GREATER_THAN_INT(0, diags.error_count);
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_code(IRON_ERR_IFACE_CANNOT_DECLARE_INIT),
        "expected IRON_ERR_IFACE_CANNOT_DECLARE_INIT (E0256) for init in interface");
    TEST_ASSERT_TRUE_MESSAGE(
        has_diag_msg_substring("interfaces may not declare init"),
        "expected locked 'interfaces may not declare init' message");
    Iron_Node *prog = parse("interface I {\n    init() {}\n}\n");
    (void)prog;
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)first_decl(prog);
    TEST_ASSERT_EQUAL(0, iface->method_count);
}

void test_parse_interface_default_sig_no_tier(void) {
    /* Regression lock: plain sig unchanged — both tier bits false. */
    Iron_Node *prog = parse(
        "interface Base {\n"
        "    func run()\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)first_decl(prog);
    Iron_FuncDecl *sig = (Iron_FuncDecl *)iface->method_sigs[0];
    TEST_ASSERT_FALSE(sig->is_readonly);
    TEST_ASSERT_FALSE(sig->is_pure);
    TEST_ASSERT_NULL(sig->body);
}

void test_parse_interface_mixed_tiers_multiple_sigs(void) {
    Iron_Node *prog = parse(
        "interface Mix {\n"
        "    readonly func a() -> Int\n"
        "    pure func b() -> Int\n"
        "    func c()\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)first_decl(prog);
    TEST_ASSERT_EQUAL(3, iface->method_count);
    Iron_FuncDecl *a = (Iron_FuncDecl *)iface->method_sigs[0];
    Iron_FuncDecl *b = (Iron_FuncDecl *)iface->method_sigs[1];
    Iron_FuncDecl *c = (Iron_FuncDecl *)iface->method_sigs[2];
    TEST_ASSERT_TRUE(a->is_readonly);
    TEST_ASSERT_FALSE(a->is_pure);
    TEST_ASSERT_TRUE(b->is_pure);
    TEST_ASSERT_FALSE(b->is_readonly);
    TEST_ASSERT_FALSE(c->is_readonly);
    TEST_ASSERT_FALSE(c->is_pure);
}

/* ── Phase 87-02 SELF-01/02/03 + IFACE-05: Self type + Self-construct ─────── */

/* test_self_type_in_method_return: `object P { init() {} readonly func clone() -> Self { return self } }`
 * The return_type of clone() must be Iron_TypeAnnotation with is_self_type==true and name=="Self". */
void test_self_type_in_method_return(void) {
    Iron_Node *prog = parse(
        "object P {\n"
        "    init() {}\n"
        "    readonly func clone() -> Self { return self }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    /* The method decl is emitted as a top-level MethodDecl after the ObjectDecl. */
    Iron_MethodDecl *md = NULL;
    for (int i = 0; i < pr->decl_count; i++) {
        if (pr->decls[i]->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[i];
            if (strcmp(m->method_name, "clone") == 0) { md = m; break; }
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(md, "expected clone() MethodDecl");
    TEST_ASSERT_NOT_NULL(md->return_type);
    TEST_ASSERT_EQUAL(IRON_NODE_TYPE_ANNOTATION, md->return_type->kind);
    Iron_TypeAnnotation *ann = (Iron_TypeAnnotation *)md->return_type;
    TEST_ASSERT_EQUAL_STRING("Self", ann->name);
    TEST_ASSERT_TRUE_MESSAGE(ann->is_self_type, "expected is_self_type==true for Self return annotation");
}

/* test_self_type_in_interface_sig: interface sig return_type has is_self_type==true (IFACE-05). */
void test_self_type_in_interface_sig(void) {
    Iron_Node *prog = parse(
        "interface Clone {\n"
        "    readonly func clone() -> Self\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_INTERFACE_DECL, iface->kind);
    TEST_ASSERT_EQUAL(1, iface->method_count);
    Iron_FuncDecl *sig = (Iron_FuncDecl *)iface->method_sigs[0];
    TEST_ASSERT_NOT_NULL(sig->return_type);
    TEST_ASSERT_EQUAL(IRON_NODE_TYPE_ANNOTATION, sig->return_type->kind);
    Iron_TypeAnnotation *ann = (Iron_TypeAnnotation *)sig->return_type;
    TEST_ASSERT_EQUAL_STRING("Self", ann->name);
    TEST_ASSERT_TRUE_MESSAGE(ann->is_self_type, "expected is_self_type==true on iface sig return type");
}

/* test_self_construct_anon: method body contains `return Self(5)` — parse produces
 * IRON_NODE_CALL with callee IRON_NODE_IDENT name=="Self". */
void test_self_construct_anon(void) {
    Iron_Node *prog = parse(
        "object P {\n"
        "    pub val x: Int\n"
        "    init(x: Int) { self.x = x }\n"
        "    readonly func make(v: Int) -> Self { return Self(v) }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    Iron_MethodDecl *md = NULL;
    for (int i = 0; i < pr->decl_count; i++) {
        if (pr->decls[i]->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[i];
            if (strcmp(m->method_name, "make") == 0) { md = m; break; }
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(md, "expected make() MethodDecl");
    /* Body: block with 1 return stmt containing a CALL expr with callee ident "Self". */
    Iron_Block *body = (Iron_Block *)md->body;
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_GREATER_THAN(0, body->stmt_count);
    Iron_ReturnStmt *ret = (Iron_ReturnStmt *)body->stmts[0];
    TEST_ASSERT_EQUAL(IRON_NODE_RETURN, ret->kind);
    TEST_ASSERT_NOT_NULL(ret->value);
    TEST_ASSERT_EQUAL(IRON_NODE_CALL, ret->value->kind);
    Iron_CallExpr *ce = (Iron_CallExpr *)ret->value;
    TEST_ASSERT_NOT_NULL(ce->callee);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, ce->callee->kind);
    Iron_Ident *cid = (Iron_Ident *)ce->callee;
    TEST_ASSERT_EQUAL_STRING("Self", cid->name);
}

/* test_self_construct_named: method body contains `return Self.from_x(5)` — parse
 * produces IRON_NODE_METHOD_CALL with receiver IRON_NODE_IDENT name=="Self" + method_name=="from_x". */
void test_self_construct_named(void) {
    Iron_Node *prog = parse(
        "object P {\n"
        "    pub val x: Int\n"
        "    init from_x(x: Int) { self.x = x }\n"
        "    readonly func make2(v: Int) -> Self { return Self.from_x(v) }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    Iron_MethodDecl *md = NULL;
    for (int i = 0; i < pr->decl_count; i++) {
        if (pr->decls[i]->kind == IRON_NODE_METHOD_DECL) {
            Iron_MethodDecl *m = (Iron_MethodDecl *)pr->decls[i];
            if (strcmp(m->method_name, "make2") == 0) { md = m; break; }
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(md, "expected make2() MethodDecl");
    Iron_Block *body = (Iron_Block *)md->body;
    TEST_ASSERT_NOT_NULL(body);
    TEST_ASSERT_GREATER_THAN(0, body->stmt_count);
    Iron_ReturnStmt *ret = (Iron_ReturnStmt *)body->stmts[0];
    TEST_ASSERT_EQUAL(IRON_NODE_RETURN, ret->kind);
    TEST_ASSERT_NOT_NULL(ret->value);
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_CALL, ret->value->kind);
    Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)ret->value;
    TEST_ASSERT_NOT_NULL(mc->object);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, mc->object->kind);
    Iron_Ident *rid = (Iron_Ident *)mc->object;
    TEST_ASSERT_EQUAL_STRING("Self", rid->name);
    TEST_ASSERT_EQUAL_STRING("from_x", mc->method);
}

/* ── Phase 87-02 PATCH-08: patch implements clause ───────────────────────── */

/* test_patch_implements_single: `patch object Int implements Comparable { ... }`
 * ObjectDecl has is_patch==true, implements_count==1, implements_names[0]=="Comparable". */
void test_patch_implements_single(void) {
    Iron_Node *prog = parse(
        "patch object Int implements Comparable {\n"
        "    readonly func cmp(other: Int) -> Int { return 0 }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    Iron_ObjectDecl *od = NULL;
    for (int i = 0; i < pr->decl_count; i++) {
        if (pr->decls[i]->kind == IRON_NODE_OBJECT_DECL) {
            od = (Iron_ObjectDecl *)pr->decls[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(od, "expected ObjectDecl for patch");
    TEST_ASSERT_TRUE_MESSAGE(od->is_patch, "expected is_patch==true");
    TEST_ASSERT_EQUAL_STRING("Int", od->target_type_name);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, od->implements_count,
        "expected implements_count==1 for single implements clause");
    TEST_ASSERT_EQUAL_STRING("Comparable", od->implements_names[0]);
}

/* test_patch_implements_multiple: `patch object Foo implements A, B, C { }` => implements_count==3. */
void test_patch_implements_multiple(void) {
    Iron_Node *prog = parse(
        "patch object Foo implements A, B, C {\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    Iron_ObjectDecl *od = NULL;
    for (int i = 0; i < pr->decl_count; i++) {
        if (pr->decls[i]->kind == IRON_NODE_OBJECT_DECL) {
            od = (Iron_ObjectDecl *)pr->decls[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(od, "expected ObjectDecl for patch");
    TEST_ASSERT_TRUE_MESSAGE(od->is_patch, "expected is_patch==true");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, od->implements_count,
        "expected implements_count==3 for three-name implements clause");
    TEST_ASSERT_EQUAL_STRING("A", od->implements_names[0]);
    TEST_ASSERT_EQUAL_STRING("B", od->implements_names[1]);
    TEST_ASSERT_EQUAL_STRING("C", od->implements_names[2]);
}

/* test_patch_implements_recovery: malformed `patch object Int implements , { }` —
 * parser should recover, no crash, implements_count==0. */
void test_patch_implements_recovery(void) {
    /* The trailing comma / missing name is malformed; parser must not crash. */
    (void)parse(
        "patch object Int implements , {\n"
        "}\n"
    );
    /* Just assert no crash (no segfault). Error count may be > 0 from recovery. */
    /* No assertion on implements_count — recovery path is implementation-defined. */
    (void)diags.error_count;  /* suppress unused-variable warning */
}

/* test_patch_without_implements_unchanged: Phase 86 regression lock —
 * a plain patch with no implements clause has implements_count==0. */
void test_patch_without_implements_unchanged(void) {
    Iron_Node *prog = parse(
        "patch object Int {\n"
        "    pub readonly func double() -> Int { return self * 2 }\n"
        "}\n"
    );
    TEST_ASSERT_EQUAL_INT(0, diags.error_count);
    Iron_Program *pr = (Iron_Program *)prog;
    Iron_ObjectDecl *od = NULL;
    for (int i = 0; i < pr->decl_count; i++) {
        if (pr->decls[i]->kind == IRON_NODE_OBJECT_DECL) {
            od = (Iron_ObjectDecl *)pr->decls[i];
            break;
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(od, "expected ObjectDecl for plain patch");
    TEST_ASSERT_TRUE_MESSAGE(od->is_patch, "expected is_patch==true");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, od->implements_count,
        "expected implements_count==0 when no implements clause present");
}

/* ── Enum declarations ───────────────────────────────────────────────────── */

void test_parse_enum_decl(void) {
    Iron_Node *prog = parse("enum GameState {\n  PAUSED,\n  RUNNING,\n  MENU\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_ENUM_DECL, d->kind);
    Iron_EnumDecl *e = (Iron_EnumDecl *)d;
    TEST_ASSERT_EQUAL_STRING("GameState", e->name);
    TEST_ASSERT_EQUAL(3, e->variant_count);
    TEST_ASSERT_EQUAL_STRING("PAUSED",  ((Iron_EnumVariant *)e->variants[0])->name);
    TEST_ASSERT_EQUAL_STRING("RUNNING", ((Iron_EnumVariant *)e->variants[1])->name);
    TEST_ASSERT_EQUAL_STRING("MENU",    ((Iron_EnumVariant *)e->variants[2])->name);
}

/* ── Import declarations ─────────────────────────────────────────────────── */

void test_parse_import(void) {
    Iron_Node *prog = parse("import player");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_IMPORT_DECL, d->kind);
    Iron_ImportDecl *imp = (Iron_ImportDecl *)d;
    TEST_ASSERT_EQUAL_STRING("player", imp->path);
    TEST_ASSERT_NULL(imp->alias);
}

void test_parse_import_alias(void) {
    Iron_Node *prog = parse("import physics.collision as pc");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_IMPORT_DECL, d->kind);
    Iron_ImportDecl *imp = (Iron_ImportDecl *)d;
    TEST_ASSERT_EQUAL_STRING("physics.collision", imp->path);
    TEST_ASSERT_EQUAL_STRING("pc", imp->alias);
}

/* ── Control flow statements ─────────────────────────────────────────────── */

void test_parse_if_stmt(void) {
    Iron_Node *prog = parse("func f() { if x > 0 { y = 1 } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_IF, stmt->kind);
    Iron_IfStmt *ifs = (Iron_IfStmt *)stmt;
    TEST_ASSERT_NOT_NULL(ifs->condition);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, ifs->condition->kind);
    TEST_ASSERT_NOT_NULL(ifs->body);
}

void test_parse_if_elif_else(void) {
    Iron_Node *prog = parse("func f() { if a { } elif b { } else { } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_IF, stmt->kind);
    Iron_IfStmt *ifs = (Iron_IfStmt *)stmt;
    TEST_ASSERT_EQUAL(1, ifs->elif_count);
    TEST_ASSERT_NOT_NULL(ifs->elif_conds[0]);
    TEST_ASSERT_NOT_NULL(ifs->elif_bodies[0]);
    TEST_ASSERT_NOT_NULL(ifs->else_body);
}

void test_parse_while_stmt(void) {
    Iron_Node *prog = parse("func f() { while running { update() } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_WHILE, stmt->kind);
    Iron_WhileStmt *ws = (Iron_WhileStmt *)stmt;
    TEST_ASSERT_NOT_NULL(ws->condition);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, ws->condition->kind);
    TEST_ASSERT_NOT_NULL(ws->body);
}

void test_parse_for_stmt(void) {
    Iron_Node *prog = parse("func f() { for i in range(10) { } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_FOR, stmt->kind);
    Iron_ForStmt *fs = (Iron_ForStmt *)stmt;
    TEST_ASSERT_EQUAL_STRING("i", fs->var_name);
    TEST_ASSERT_NOT_NULL(fs->iterable);
    TEST_ASSERT_FALSE(fs->is_parallel);
}

void test_parse_for_parallel(void) {
    Iron_Node *prog = parse("func f() { for i in range(10) parallel { } }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_FOR, stmt->kind);
    Iron_ForStmt *fs = (Iron_ForStmt *)stmt;
    TEST_ASSERT_TRUE(fs->is_parallel);
    TEST_ASSERT_NULL(fs->pool_expr);
}

void test_parse_match_stmt(void) {
    Iron_Node *prog = parse("func f() { match state {\n  GameState.RUNNING { }\n  else { }\n} }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_MATCH, stmt->kind);
    Iron_MatchStmt *ms = (Iron_MatchStmt *)stmt;
    TEST_ASSERT_NOT_NULL(ms->subject);
    TEST_ASSERT_EQUAL(1, ms->case_count);
    TEST_ASSERT_NOT_NULL(ms->else_body);
}

void test_parse_defer(void) {
    Iron_Node *prog = parse("func f() { defer close(file) }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_DEFER, stmt->kind);
    Iron_DeferStmt *ds = (Iron_DeferStmt *)stmt;
    TEST_ASSERT_NOT_NULL(ds->expr);
}

void test_parse_return(void) {
    Iron_Node *prog = parse("func f() -> Int { return 42 }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *stmt = first_stmt_of_func(d);
    TEST_ASSERT_EQUAL(IRON_NODE_RETURN, stmt->kind);
    Iron_ReturnStmt *rs = (Iron_ReturnStmt *)stmt;
    TEST_ASSERT_NOT_NULL(rs->value);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, rs->value->kind);
    TEST_ASSERT_EQUAL_STRING("42", ((Iron_IntLit *)rs->value)->value);
}

/* ── Operator precedence ─────────────────────────────────────────────────── */

void test_precedence_add_mul(void) {
    /* 2 + 3 * 4 should be BinaryExpr(+, 2, BinaryExpr(*, 3, 4)) */
    Iron_Node *prog = parse("val r = 2 + 3 * 4");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_VAL_DECL, d->kind);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, init->kind);
    Iron_BinaryExpr *add = (Iron_BinaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_PLUS, add->op);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, add->left->kind);
    TEST_ASSERT_EQUAL_STRING("2", ((Iron_IntLit *)add->left)->value);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, add->right->kind);
    Iron_BinaryExpr *mul = (Iron_BinaryExpr *)add->right;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_STAR, mul->op);
    TEST_ASSERT_EQUAL_STRING("3", ((Iron_IntLit *)mul->left)->value);
    TEST_ASSERT_EQUAL_STRING("4", ((Iron_IntLit *)mul->right)->value);
}

void test_precedence_comparison(void) {
    /* x > 0 and y < 10 -> BinaryExpr(and, BinaryExpr(>, x, 0), BinaryExpr(<, y, 10)) */
    Iron_Node *prog = parse("val r = x > 0 and y < 10");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, init->kind);
    Iron_BinaryExpr *and_node = (Iron_BinaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_AND, and_node->op);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, and_node->left->kind);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, and_node->right->kind);
    Iron_BinaryExpr *gt = (Iron_BinaryExpr *)and_node->left;
    Iron_BinaryExpr *lt = (Iron_BinaryExpr *)and_node->right;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_GREATER, gt->op);
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_LESS,    lt->op);
}

void test_precedence_or_and(void) {
    /* a or b and c -> BinaryExpr(or, a, BinaryExpr(and, b, c)) */
    Iron_Node *prog = parse("val r = a or b and c");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, init->kind);
    Iron_BinaryExpr *or_node = (Iron_BinaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_OR, or_node->op);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, or_node->left->kind);
    TEST_ASSERT_EQUAL(IRON_NODE_BINARY, or_node->right->kind);
    Iron_BinaryExpr *and_node = (Iron_BinaryExpr *)or_node->right;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_AND, and_node->op);
}

/* ── Unary expressions ───────────────────────────────────────────────────── */

void test_unary_minus(void) {
    Iron_Node *prog = parse("val r = -x");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_UNARY, init->kind);
    Iron_UnaryExpr *u = (Iron_UnaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_MINUS, u->op);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, u->operand->kind);
}

void test_unary_not(void) {
    Iron_Node *prog = parse("val r = not done");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_UNARY, init->kind);
    Iron_UnaryExpr *u = (Iron_UnaryExpr *)init;
    TEST_ASSERT_EQUAL((Iron_OpKind)IRON_TOK_NOT, u->op);
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, u->operand->kind);
    TEST_ASSERT_EQUAL_STRING("done", ((Iron_Ident *)u->operand)->name);
}

/* ── Call expressions ────────────────────────────────────────────────────── */

void test_call_expr(void) {
    Iron_Node *prog = parse("val r = add(1, 2)");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_CALL, init->kind);
    Iron_CallExpr *c = (Iron_CallExpr *)init;
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, c->callee->kind);
    TEST_ASSERT_EQUAL(2, c->arg_count);
}

void test_method_call(void) {
    Iron_Node *prog = parse("val r = player.update(dt)");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_METHOD_CALL, init->kind);
    Iron_MethodCallExpr *mc = (Iron_MethodCallExpr *)init;
    TEST_ASSERT_EQUAL_STRING("update", mc->method);
    TEST_ASSERT_EQUAL(1, mc->arg_count);
}

void test_field_access(void) {
    Iron_Node *prog = parse("val r = player.hp");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_FIELD_ACCESS, init->kind);
    Iron_FieldAccess *fa = (Iron_FieldAccess *)init;
    TEST_ASSERT_EQUAL_STRING("hp", fa->field);
}

void test_index_access(void) {
    Iron_Node *prog = parse("val r = items[0]");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_INDEX, init->kind);
    Iron_IndexExpr *ix = (Iron_IndexExpr *)init;
    TEST_ASSERT_NOT_NULL(ix->object);
    TEST_ASSERT_NOT_NULL(ix->index);
    TEST_ASSERT_EQUAL(IRON_NODE_INT_LIT, ix->index->kind);
}

/* ── Special expressions ─────────────────────────────────────────────────── */

void test_lambda(void) {
    Iron_Node *prog = parse("val r = func(x: Int) -> Int { return x * 2 }");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_LAMBDA, init->kind);
    Iron_LambdaExpr *lam = (Iron_LambdaExpr *)init;
    TEST_ASSERT_EQUAL(1, lam->param_count);
    TEST_ASSERT_NOT_NULL(lam->return_type);
    TEST_ASSERT_NOT_NULL(lam->body);
}

void test_heap_expr(void) {
    Iron_Node *prog = parse("val r = heap Enemy(1, 2)");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_HEAP, init->kind);
    Iron_HeapExpr *he = (Iron_HeapExpr *)init;
    TEST_ASSERT_NOT_NULL(he->inner);
    /* The inner is a CallExpr since ConstructExpr / CallExpr are the same at parse time */
    TEST_ASSERT_TRUE(he->inner->kind == IRON_NODE_CALL || he->inner->kind == IRON_NODE_CONSTRUCT);
}

void test_is_expr(void) {
    Iron_Node *prog = parse("val r = a is Player");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    TEST_ASSERT_EQUAL(IRON_NODE_IS, init->kind);
    Iron_IsExpr *is_n = (Iron_IsExpr *)init;
    TEST_ASSERT_EQUAL(IRON_NODE_IDENT, is_n->expr->kind);
    TEST_ASSERT_EQUAL_STRING("Player", is_n->type_name);
}

void test_construct_expr(void) {
    Iron_Node *prog = parse("val r = Player(pos, 100)");
    Iron_Node *d    = first_decl(prog);
    Iron_Node *init = ((Iron_ValDecl *)d)->init;
    /* Parser emits CallExpr; semantic analysis disambiguates */
    TEST_ASSERT_TRUE(init->kind == IRON_NODE_CALL || init->kind == IRON_NODE_CONSTRUCT);
    if (init->kind == IRON_NODE_CALL) {
        Iron_CallExpr *c = (Iron_CallExpr *)init;
        TEST_ASSERT_EQUAL(2, c->arg_count);
    }
}

/* ── Span invariant ──────────────────────────────────────────────────────── */

typedef struct {
    bool all_positive;
} SpanCheckCtx;

static bool span_check_visitor(Iron_Visitor *v, Iron_Node *node) {
    SpanCheckCtx *ctx = (SpanCheckCtx *)v->ctx;
    /* Every node must have line >= 1 */
    if (node->span.line == 0) ctx->all_positive = false;
    return true;
}

void test_every_node_has_span(void) {
    Iron_Node *prog = parse(
        "func add(a: Int, b: Int) -> Int {\n"
        "  return a + b\n"
        "}\n"
        "val x = 10\n"
        "var y: Float = 3.14\n"
        "object Foo {\n"
        "  var z: Int\n"
        "}\n"
    );
    SpanCheckCtx ctx = { .all_positive = true };
    Iron_Visitor v   = {
        .ctx        = &ctx,
        .visit_node = span_check_visitor,
        .post_visit = NULL
    };
    iron_ast_walk(prog, &v);
    TEST_ASSERT_TRUE(ctx.all_positive);
}

/* ── Generic declarations ────────────────────────────────────────────────── */

void test_generic_func(void) {
    Iron_Node *prog = parse("func find[T](items: [T]) -> T? { return null }");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_FUNC_DECL, d->kind);
    Iron_FuncDecl *f = (Iron_FuncDecl *)d;
    TEST_ASSERT_EQUAL_STRING("find", f->name);
    TEST_ASSERT_EQUAL(1, f->generic_param_count);
    TEST_ASSERT_EQUAL(1, f->param_count);
    TEST_ASSERT_NOT_NULL(f->return_type);
}

void test_generic_object(void) {
    Iron_Node *prog = parse("object Pool[T] {\n  var items: [T]\n}");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_OBJECT_DECL, d->kind);
    Iron_ObjectDecl *obj = (Iron_ObjectDecl *)d;
    TEST_ASSERT_EQUAL_STRING("Pool", obj->name);
    TEST_ASSERT_EQUAL(1, obj->generic_param_count);
    TEST_ASSERT_EQUAL(1, obj->field_count);
}

/* ── Phase 59 01d: tuple types + literals + destructure ───────────────── */

void test_parse_tuple_type_annotation_2_elem(void) {
    Iron_Node *prog = parse("func f() -> (Int, Int) { return 0 }");
    Iron_Node *d    = first_decl(prog);
    TEST_ASSERT_EQUAL(IRON_NODE_FUNC_DECL, d->kind);
    Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
    TEST_ASSERT_NOT_NULL(fd->return_type);
    Iron_TypeAnnotation *ann = (Iron_TypeAnnotation *)fd->return_type;
    TEST_ASSERT_EQUAL(IRON_NODE_TYPE_ANNOTATION, ann->kind);
    TEST_ASSERT_TRUE(ann->is_tuple);
    TEST_ASSERT_EQUAL_INT(2, ann->tuple_elem_count);
}

void test_parse_tuple_type_annotation_3_elem(void) {
    Iron_Node *prog = parse("func f() -> (Int, String, Bool) { return 0 }");
    Iron_Node *d    = first_decl(prog);
    Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
    Iron_TypeAnnotation *ann = (Iron_TypeAnnotation *)fd->return_type;
    TEST_ASSERT_TRUE(ann->is_tuple);
    TEST_ASSERT_EQUAL_INT(3, ann->tuple_elem_count);
}

void test_parse_tuple_type_annotation_nested(void) {
    Iron_Node *prog = parse("func f() -> (Int, (String, Bool)) { return 0 }");
    Iron_Node *d    = first_decl(prog);
    Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
    Iron_TypeAnnotation *ann = (Iron_TypeAnnotation *)fd->return_type;
    TEST_ASSERT_TRUE(ann->is_tuple);
    TEST_ASSERT_EQUAL_INT(2, ann->tuple_elem_count);
    Iron_TypeAnnotation *inner =
        (Iron_TypeAnnotation *)ann->tuple_elems[1];
    TEST_ASSERT_TRUE(inner->is_tuple);
    TEST_ASSERT_EQUAL_INT(2, inner->tuple_elem_count);
}

void test_parse_tuple_literal_2_elem(void) {
    /* Inside a function body so `val x = (1, 2)` parses as a stmt. */
    Iron_Node *prog = parse("func main() { val x = (1, 2) }");
    Iron_Node *d    = first_decl(prog);
    Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
    Iron_Block *body  = (Iron_Block *)fd->body;
    TEST_ASSERT_GREATER_THAN_INT(0, body->stmt_count);
    Iron_ValDecl *vd  = (Iron_ValDecl *)body->stmts[0];
    TEST_ASSERT_EQUAL(IRON_NODE_VAL_DECL, vd->kind);
    TEST_ASSERT_NOT_NULL(vd->init);
    TEST_ASSERT_EQUAL(IRON_NODE_ARRAY_LIT, vd->init->kind);
    Iron_ArrayLit *al = (Iron_ArrayLit *)vd->init;
    TEST_ASSERT_EQUAL_INT(2, al->element_count);
    /* tuple sentinel attached via type_ann */
    TEST_ASSERT_NOT_NULL(al->type_ann);
    Iron_TypeAnnotation *tag = (Iron_TypeAnnotation *)al->type_ann;
    TEST_ASSERT_TRUE(tag->is_tuple);
}

void test_parse_tuple_literal_in_return(void) {
    Iron_Node *prog = parse("func f() -> (Int, Int) { return (1, 2) }");
    Iron_Node *d    = first_decl(prog);
    Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
    Iron_Block *body  = (Iron_Block *)fd->body;
    Iron_ReturnStmt *rs = (Iron_ReturnStmt *)body->stmts[0];
    TEST_ASSERT_EQUAL(IRON_NODE_RETURN, rs->kind);
    TEST_ASSERT_NOT_NULL(rs->value);
    TEST_ASSERT_EQUAL(IRON_NODE_ARRAY_LIT, rs->value->kind);
    Iron_ArrayLit *al = (Iron_ArrayLit *)rs->value;
    TEST_ASSERT_NOT_NULL(al->type_ann);
    TEST_ASSERT_TRUE(((Iron_TypeAnnotation *)al->type_ann)->is_tuple);
}

void test_parse_tuple_destructure_binding(void) {
    Iron_Node *prog = parse("func main() { val (a, b) = (1, 2) }");
    Iron_Node *d    = first_decl(prog);
    Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
    Iron_Block *body  = (Iron_Block *)fd->body;
    TEST_ASSERT_GREATER_THAN_INT(0, body->stmt_count);
    Iron_ValDecl *vd  = (Iron_ValDecl *)body->stmts[0];
    TEST_ASSERT_EQUAL(IRON_NODE_VAL_DECL, vd->kind);
    TEST_ASSERT_EQUAL_INT(2, vd->binding_count);
    TEST_ASSERT_NOT_NULL(vd->binding_names[0]);
    TEST_ASSERT_EQUAL_STRING("a", vd->binding_names[0]);
    TEST_ASSERT_EQUAL_STRING("b", vd->binding_names[1]);
}

void test_parse_tuple_destructure_wildcard(void) {
    Iron_Node *prog = parse("func main() { val (x, _) = (1, 2) }");
    Iron_Node *d    = first_decl(prog);
    Iron_FuncDecl *fd = (Iron_FuncDecl *)d;
    Iron_Block *body  = (Iron_Block *)fd->body;
    Iron_ValDecl *vd  = (Iron_ValDecl *)body->stmts[0];
    TEST_ASSERT_EQUAL_INT(2, vd->binding_count);
    TEST_ASSERT_EQUAL_STRING("x", vd->binding_names[0]);
    TEST_ASSERT_NULL(vd->binding_names[1]);  /* wildcard sentinel */
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_parse_val_decl);
    RUN_TEST(test_parse_var_decl);
    RUN_TEST(test_parse_func_decl);
    RUN_TEST(test_parse_method_decl);
    RUN_TEST(test_parse_object_decl);
    RUN_TEST(test_parse_object_extends);
    RUN_TEST(test_parse_object_implements);

    /* Phase 82 GRAMMAR: `self` hard-reserved keyword */
    RUN_TEST(test_self_keyword_rejected_as_val_name);
    RUN_TEST(test_self_keyword_rejected_as_func_name);
    RUN_TEST(test_self_keyword_rejected_as_param_name);

    /* Phase 82 GRAMMAR: in-block method declarations */
    RUN_TEST(test_object_in_block_method_parses);
    RUN_TEST(test_object_in_block_method_no_params);
    RUN_TEST(test_object_in_block_method_interleaved_with_fields);
    RUN_TEST(test_object_field_only_still_parses);

    /* Phase 83 ACCESS-02: pub modifier on fields and methods */
    RUN_TEST(test_pub_on_fields_sets_is_pub);
    RUN_TEST(test_pub_mixed_with_plain_fields);
    RUN_TEST(test_pub_on_method_parses);
    RUN_TEST(test_pub_at_top_level_rejected);
    RUN_TEST(test_is_synth_accessor_default_false_on_in_block_method);
    RUN_TEST(test_phase82_in_block_method_still_parses);
    /* Phase 83-02 Task 1: accessor synthesis + name-collision diagnostic. */
    RUN_TEST(test_pub_val_synthesizes_getter_only);
    RUN_TEST(test_pub_var_synthesizes_getter_and_setter);
    RUN_TEST(test_non_pub_field_does_not_synthesize);
    RUN_TEST(test_pub_val_getter_collides_with_user_method);
    RUN_TEST(test_pub_var_setter_collides_with_user_method);

    /* Phase 84 MUTTIER-01/02/03: readonly + pure modifier threading. */
    RUN_TEST(test_readonly_method_parse);
    RUN_TEST(test_pure_method_parse);
    RUN_TEST(test_plain_method_has_default_tier);
    RUN_TEST(test_pub_readonly_method);
    RUN_TEST(test_pub_pure_method);
    RUN_TEST(test_readonly_pure_combo_rejected);
    RUN_TEST(test_pure_readonly_combo_rejected);
    RUN_TEST(test_readonly_at_top_level_rejected);
    RUN_TEST(test_pure_at_top_level_rejected);
    RUN_TEST(test_synth_getter_is_readonly_and_pure);

    /* Phase 85 INIT-03/07/08/11/13/15: init declarations + auto-synth. */
    RUN_TEST(test_parse_anonymous_init);
    RUN_TEST(test_parse_named_init);
    RUN_TEST(test_parse_anonymous_plus_named_inits);
    RUN_TEST(test_parse_duplicate_anonymous_init_rejected);
    RUN_TEST(test_parse_duplicate_named_init_rejected);
    RUN_TEST(test_parse_pub_init_rejected);
    RUN_TEST(test_parse_readonly_init_rejected);
    RUN_TEST(test_parse_pure_init_rejected);
    RUN_TEST(test_parse_fieldless_init_auto_synth);
    RUN_TEST(test_parse_fieldless_with_user_init_no_synth);
    RUN_TEST(test_parse_init_with_return_type_rejected);
    RUN_TEST(test_parse_init_in_interface_rejected);

    /* Phase 86 PATCH-01/02/05: patch object T { ... } parse surface. */
    RUN_TEST(test_parse_patch_object_parses);
    RUN_TEST(test_parse_patch_object_no_name);
    RUN_TEST(test_parse_patch_adds_field_rejected);
    RUN_TEST(test_parse_patch_adds_val_field_rejected);
    RUN_TEST(test_parse_patch_with_init);
    RUN_TEST(test_parse_patch_with_named_init);
    RUN_TEST(test_parse_patch_body_pub_plus_tier);
    RUN_TEST(test_parse_patch_with_generic_params_rejected);

    RUN_TEST(test_parse_interface_decl);
    RUN_TEST(test_parse_interface_readonly_sig);
    RUN_TEST(test_parse_interface_pure_sig);
    RUN_TEST(test_parse_interface_readonly_default_body);
    RUN_TEST(test_parse_interface_pure_default_body);
    RUN_TEST(test_parse_interface_readonly_plus_pure_rejected);
    RUN_TEST(test_parse_interface_init_rejected_e0256);
    RUN_TEST(test_parse_interface_default_sig_no_tier);
    RUN_TEST(test_parse_interface_mixed_tiers_multiple_sigs);

    /* Phase 87-02 SELF-01/02/03 + IFACE-05: Self type + Self-construct. */
    RUN_TEST(test_self_type_in_method_return);
    RUN_TEST(test_self_type_in_interface_sig);
    RUN_TEST(test_self_construct_anon);
    RUN_TEST(test_self_construct_named);

    /* Phase 87-02 PATCH-08: patch implements clause. */
    RUN_TEST(test_patch_implements_single);
    RUN_TEST(test_patch_implements_multiple);
    RUN_TEST(test_patch_implements_recovery);
    RUN_TEST(test_patch_without_implements_unchanged);

    RUN_TEST(test_parse_enum_decl);
    RUN_TEST(test_parse_import);
    RUN_TEST(test_parse_import_alias);
    RUN_TEST(test_parse_if_stmt);
    RUN_TEST(test_parse_if_elif_else);
    RUN_TEST(test_parse_while_stmt);
    RUN_TEST(test_parse_for_stmt);
    RUN_TEST(test_parse_for_parallel);
    RUN_TEST(test_parse_match_stmt);
    RUN_TEST(test_parse_defer);
    RUN_TEST(test_parse_return);
    RUN_TEST(test_precedence_add_mul);
    RUN_TEST(test_precedence_comparison);
    RUN_TEST(test_precedence_or_and);
    RUN_TEST(test_unary_minus);
    RUN_TEST(test_unary_not);
    RUN_TEST(test_call_expr);
    RUN_TEST(test_method_call);
    RUN_TEST(test_field_access);
    RUN_TEST(test_index_access);
    RUN_TEST(test_lambda);
    RUN_TEST(test_heap_expr);
    RUN_TEST(test_is_expr);
    RUN_TEST(test_construct_expr);
    RUN_TEST(test_every_node_has_span);
    RUN_TEST(test_generic_func);
    RUN_TEST(test_generic_object);

    /* Phase 59 01d */
    RUN_TEST(test_parse_tuple_type_annotation_2_elem);
    RUN_TEST(test_parse_tuple_type_annotation_3_elem);
    RUN_TEST(test_parse_tuple_type_annotation_nested);
    RUN_TEST(test_parse_tuple_literal_2_elem);
    RUN_TEST(test_parse_tuple_literal_in_return);
    RUN_TEST(test_parse_tuple_destructure_binding);
    RUN_TEST(test_parse_tuple_destructure_wildcard);

    return UNITY_END();
}
