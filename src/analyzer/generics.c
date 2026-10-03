/* User generics by monomorphisation. See generics.h. */

#include "analyzer/generics.h"
#include "hir/stdlib_origin.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/printer.h"
#include "util/strbuf.h"
#include "vendor/stb_ds.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Iron_Node   *decl;          /* template FuncDecl / ObjectDecl */
    const char  *mangled;
    const char **arg_texts;     /* stb_ds: source spelling per type argument */
    Iron_Type  **arg_types;     /* arena copy of the type arguments */
    int          arg_count;
    bool         materialized;
} GenInst;

/* Per thread: the language server analyzes documents on several worker
 * threads at once, and each analysis owns its instance table from reset to
 * drop_templates (a shared table raced and one analysis reset another's). */
static _Thread_local GenInst *g_insts;        /* stb_ds */

void iron_generics_reset(void) {
    for (ptrdiff_t i = 0; i < arrlen(g_insts); i++) arrfree(g_insts[i].arg_texts);
    arrfree(g_insts);
    g_insts = NULL;
}

static const char *decl_name(Iron_Node *d) {
    if (!d) return NULL;
    if (d->kind == IRON_NODE_FUNC_DECL) return ((Iron_FuncDecl *)d)->name;
    if (d->kind == IRON_NODE_OBJECT_DECL) return ((Iron_ObjectDecl *)d)->name;
    if (d->kind == IRON_NODE_METHOD_DECL) return ((Iron_MethodDecl *)d)->method_name;
    return NULL;
}

static int decl_generic_count(Iron_Node *d) {
    if (!d) return 0;
    if (d->kind == IRON_NODE_FUNC_DECL) return ((Iron_FuncDecl *)d)->generic_param_count;
    if (d->kind == IRON_NODE_OBJECT_DECL) return ((Iron_ObjectDecl *)d)->generic_param_count;
    if (d->kind == IRON_NODE_METHOD_DECL) return ((Iron_MethodDecl *)d)->generic_param_count;
    return 0;
}

static Iron_Node **decl_generic_params(Iron_Node *d) {
    if (!d) return NULL;
    if (d->kind == IRON_NODE_FUNC_DECL) return ((Iron_FuncDecl *)d)->generic_params;
    if (d->kind == IRON_NODE_OBJECT_DECL) return ((Iron_ObjectDecl *)d)->generic_params;
    if (d->kind == IRON_NODE_METHOD_DECL) return ((Iron_MethodDecl *)d)->generic_params;
    return NULL;
}

static bool is_user_decl(Iron_Node *d) {
    if (!d) return false;
    /* The `rc [T]` wrapper is a stdlib template instantiated like a user
     * generic (its instances are ordinary objects). */
    if (d->kind == IRON_NODE_OBJECT_DECL) {
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
        if (od->name && strcmp(od->name, "__RcList") == 0) return true;
    }
    return d->span.filename &&
           iron_stdlib_origin_classify(d->span.filename) != 1;
}

static Iron_ObjectDecl *find_object(Iron_Program *program, const char *name) {
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
        if (!od->is_patch && od->name && strcmp(od->name, name) == 0) return od;
    }
    return NULL;
}

bool iron_generics_is_template(Iron_Program *program, Iron_Node *decl) {
    if (!decl || !is_user_decl(decl)) return false;
    if (decl->kind == IRON_NODE_FUNC_DECL) {
        Iron_FuncDecl *fd = (Iron_FuncDecl *)decl;
        return fd->generic_param_count > 0 && !fd->is_extern;
    }
    if (decl->kind == IRON_NODE_OBJECT_DECL) {
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)decl;
        if (od->generic_param_count > 0 && !od->is_patch) return true;
        if (od->is_patch && od->target_type_name) {
            Iron_ObjectDecl *t = find_object(program, od->target_type_name);
            return t && t->generic_param_count > 0 && is_user_decl((Iron_Node *)t);
        }
        return false;
    }
    if (decl->kind == IRON_NODE_METHOD_DECL) {
        Iron_MethodDecl *md = (Iron_MethodDecl *)decl;
        if (md->generic_param_count > 0 && !md->is_array_extension) return true;
        if (!md->type_name) return false;
        Iron_ObjectDecl *t = find_object(program, md->type_name);
        return t && t->generic_param_count > 0 && is_user_decl((Iron_Node *)t);
    }
    return false;
}

/* Identifier-safe spelling of a type's source text. */
static void mangle_append(Iron_StrBuf *sb, const char *text) {
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            char ch[2] = { (char)c, '\0' };
            iron_strbuf_appendf(sb, "%s", ch);
        } else if (c == '[') {
            iron_strbuf_appendf(sb, "L");
        } else if (c == '?') {
            iron_strbuf_appendf(sb, "N");
        } else if (c == ']' || c == ' ') {
            /* dropped */
        } else {
            iron_strbuf_appendf(sb, "_");
        }
    }
}

const char *iron_generics_request(Iron_Node *decl, Iron_Type **args, int argc,
                                  Iron_Arena *arena) {
    const char *name = decl_name(decl);
    if (!name || argc != decl_generic_count(decl) || argc <= 0) return NULL;
    const char **texts = NULL;
    Iron_StrBuf sb = iron_strbuf_create(64);
    iron_strbuf_appendf(&sb, "%s", name);
    for (int i = 0; i < argc; i++) {
        Iron_Type *t = args[i];
        if (!t || t->kind == IRON_TYPE_ERROR || t->kind == IRON_TYPE_GENERIC_PARAM ||
            t->kind == IRON_TYPE_NULL || t->kind == IRON_TYPE_VOID) {
            arrfree(texts);
            iron_strbuf_free(&sb);
            return NULL;
        }
        const char *txt = iron_type_to_string(t, arena);
        if (!txt) { arrfree(texts); iron_strbuf_free(&sb); return NULL; }
        arrput(texts, txt);
        iron_strbuf_appendf(&sb, "__");
        mangle_append(&sb, txt);
    }
    const char *mangled = iron_arena_strdup(arena, iron_strbuf_get(&sb), sb.len);
    iron_strbuf_free(&sb);
    if (!mangled) { arrfree(texts); return NULL; }
    for (ptrdiff_t i = 0; i < arrlen(g_insts); i++) {
        if (g_insts[i].decl == decl && strcmp(g_insts[i].mangled, mangled) == 0) {
            arrfree(texts);
            return g_insts[i].mangled;
        }
    }
    Iron_Type **copy = (Iron_Type **)iron_arena_alloc(arena, sizeof(Iron_Type *) * (size_t)argc,
                                                      _Alignof(Iron_Type *));
    if (copy) memcpy(copy, args, sizeof(Iron_Type *) * (size_t)argc);
    GenInst gi = { decl, mangled, texts, copy, copy ? argc : 0, false };
    arrput(g_insts, gi);
    return mangled;
}

Iron_Node *iron_generics_instance_of(const char *mangled, Iron_Type ***out_args, int *out_argc) {
    if (!mangled) return NULL;
    for (ptrdiff_t i = 0; i < arrlen(g_insts); i++) {
        if (strcmp(g_insts[i].mangled, mangled) != 0) continue;
        if (out_args) *out_args = g_insts[i].arg_types;
        if (out_argc) *out_argc = g_insts[i].arg_count;
        return g_insts[i].decl;
    }
    return NULL;
}

/* ── Cloning ────────────────────────────────────────────────────────────── */

static bool is_param_name(Iron_Node **gps, int n, const char *v, int *idx) {
    for (int i = 0; i < n; i++) {
        Iron_Ident *gp = (Iron_Ident *)gps[i];
        if (gp && gp->name && v && strcmp(gp->name, v) == 0) { *idx = i; return true; }
    }
    return false;
}

/* Rewrite the printed template: the declaration's own name (the first
 * identifier after `object` / `func`) becomes the mangled name and loses
 * its `[...]` parameter list; every other identifier token naming a type
 * parameter becomes that argument's source text. */
static char *rewrite_template(const char *src, GenInst *gi, Iron_Arena *arena) {
    Iron_DiagList scratch = iron_diaglist_create();
    Iron_Lexer lexer = iron_lexer_create(src, "<generic>", arena, &scratch);
    Iron_Token *toks = iron_lex_all(&lexer);
    int nt = (int)arrlen(toks);

    size_t srclen = strlen(src);
    size_t *line_start = NULL;
    arrput(line_start, 0);
    for (size_t i = 0; i < srclen; i++)
        if (src[i] == '\n') arrput(line_start, i + 1);

    const char *name = decl_name(gi->decl);
    Iron_Node **gps = decl_generic_params(gi->decl);
    int ngp = decl_generic_count(gi->decl);
    bool header_done = false;

    Iron_StrBuf out = iron_strbuf_create(srclen + 256);
    size_t cursor = 0;
    for (int i = 0; i < nt; i++) {
        Iron_Token *t = &toks[i];
        if (t->kind != IRON_TOK_IDENTIFIER || !t->value) continue;
        if (t->line == 0 || (ptrdiff_t)t->line > arrlen(line_start)) continue;
        size_t off = line_start[t->line - 1] + (t->col > 0 ? t->col - 1 : 0);
        size_t vlen = strlen(t->value);
        if (off + vlen > srclen || strncmp(src + off, t->value, vlen) != 0) continue;

        int pidx = -1;
        if (!header_done && strcmp(t->value, name) == 0 && i > 0 &&
            (toks[i - 1].kind == IRON_TOK_OBJECT || toks[i - 1].kind == IRON_TOK_FUNC)) {
            header_done = true;
            iron_strbuf_appendf(&out, "%.*s%s", (int)(off - cursor), src + cursor, gi->mangled);
            cursor = off + vlen;
            if (i + 1 < nt && toks[i + 1].kind == IRON_TOK_LBRACKET) {
                int depth = 0, j = i + 1;
                for (; j < nt; j++) {
                    if (toks[j].kind == IRON_TOK_LBRACKET) depth++;
                    else if (toks[j].kind == IRON_TOK_RBRACKET && --depth == 0) break;
                }
                if (j < nt && toks[j].line > 0 && (ptrdiff_t)toks[j].line <= arrlen(line_start)) {
                    size_t rb = line_start[toks[j].line - 1] +
                                (toks[j].col > 0 ? toks[j].col - 1 : 0);
                    if (rb < srclen && src[rb] == ']') cursor = rb + 1;
                    i = j;
                }
            }
            continue;
        }
        if (is_param_name(gps, ngp, t->value, &pidx)) {
            iron_strbuf_appendf(&out, "%.*s%s", (int)(off - cursor), src + cursor,
                                gi->arg_texts[pidx]);
            cursor = off + vlen;
        }
    }
    iron_strbuf_appendf(&out, "%s", src + cursor);
    char *result = iron_arena_strdup(arena, iron_strbuf_get(&out), out.len);
    iron_strbuf_free(&out);
    arrfree(line_start);
    arrfree(toks);
    iron_diaglist_free(&scratch);
    return result;
}

static int materialize_one(Iron_Program *program, GenInst *gi, Iron_Arena *arena,
                           Iron_DiagList *diags) {
    /* Print the template (for an object, together with its methods and
     * patches, which the printer folds back into the object body). */
    Iron_Program tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.kind = IRON_NODE_PROGRAM;
    Iron_Node **decls = NULL;
    arrput(decls, gi->decl);
    if (gi->decl->kind == IRON_NODE_OBJECT_DECL) {
        const char *oname = ((Iron_ObjectDecl *)gi->decl)->name;
        for (int i = 0; i < program->decl_count; i++) {
            Iron_Node *d = program->decls[i];
            if (!d || d == gi->decl || d->kind != IRON_NODE_METHOD_DECL) continue;
            Iron_MethodDecl *md = (Iron_MethodDecl *)d;
            if (md->type_name && strcmp(md->type_name, oname) == 0) arrput(decls, d);
        }
    }
    if (gi->decl->kind == IRON_NODE_METHOD_DECL) {
        /* A generic method is printed inside `patch object T { ... }` so
         * the instance is a method of the same type. */
        Iron_ObjectDecl *patch = ARENA_ALLOC(arena, Iron_ObjectDecl);
        if (!patch) { arrfree(decls); return 0; }
        memset(patch, 0, sizeof(*patch));
        patch->kind = IRON_NODE_OBJECT_DECL;
        patch->span = gi->decl->span;
        patch->is_patch = true;
        patch->name = ((Iron_MethodDecl *)gi->decl)->type_name;
        patch->target_type_name = patch->name;
        arrfree(decls);
        decls = NULL;
        arrput(decls, (Iron_Node *)patch);
        arrput(decls, gi->decl);
    }
    tmp.decls = decls;
    tmp.decl_count = (int)arrlen(decls);
    char *printed = iron_print_ast((Iron_Node *)&tmp, NULL, arena);
    arrfree(decls);
    if (!printed) return 0;

    char *body = rewrite_template(printed, gi, arena);
    if (!body) return 0;

    /* Instances keep the template's file so visibility and stdlib
     * classification treat them as that file's code. */
    const char *file = gi->decl->span.filename ? gi->decl->span.filename : "<generic>";
    Iron_StrBuf sb = iron_strbuf_create(strlen(body) + 128);
    iron_strbuf_appendf(&sb, "-- @file: \"%s\" @line: %u\n%s\n", file,
                        gi->decl->span.line > 0 ? gi->decl->span.line : 1, body);
    char *snippet = iron_arena_strdup(arena, iron_strbuf_get(&sb), sb.len);
    iron_strbuf_free(&sb);
    if (!snippet) return 0;

    Iron_Lexer lexer = iron_lexer_create(snippet, file, arena, diags);
    Iron_Token *tokens = iron_lex_all(&lexer);
    int token_count = (int)arrlen(tokens);
    Iron_Parser parser = iron_parser_create(tokens, token_count, snippet, file,
                                            arena, diags);
    Iron_Node *ast = iron_parse(&parser);
    arrfree(tokens);
    if (!ast || ast->kind != IRON_NODE_PROGRAM) return 0;
    Iron_Program *sub = (Iron_Program *)ast;
    for (int i = 0; i < sub->decl_count; i++) {
        Iron_Node *d = sub->decls[i];
        if (!d) continue;
        arrput(program->decls, d);
        program->decl_count++;
    }
    return 1;
}

int iron_generics_materialize(Iron_Program *program, Iron_Arena *arena,
                              Iron_DiagList *diags) {
    int added = 0;
    for (ptrdiff_t i = 0; i < arrlen(g_insts); i++) {
        if (g_insts[i].materialized) continue;
        g_insts[i].materialized = true;
        added += materialize_one(program, &g_insts[i], arena, diags);
    }
    return added;
}

void iron_generics_drop_templates(Iron_Program *program) {
    /* Mark first: a method is a template through its object, which must
     * still be in the program when the method is classified. */
    bool *drop = (bool *)calloc((size_t)(program->decl_count > 0 ? program->decl_count : 1),
                                sizeof(bool));
    if (!drop) return;
    for (int i = 0; i < program->decl_count; i++)
        drop[i] = program->decls[i] && iron_generics_is_template(program, program->decls[i]);
    int w = 0;
    for (int i = 0; i < program->decl_count; i++) {
        if (drop[i]) continue;
        program->decls[w++] = program->decls[i];
    }
    free(drop);
    program->decl_count = w;
    if (program->decls) arrsetlen(program->decls, w);
}
