/* Interface default-body monomorphisation. See iface_defaults.h. */

#include "analyzer/iface_defaults.h"
#include "parser/printer.h"
#include "parser/parser.h"
#include "lexer/lexer.h"
#include "util/strbuf.h"
#include "vendor/stb_ds.h"

#include <stdio.h>
#include <string.h>

static Iron_InterfaceDecl *find_interface(Iron_Program *program, int limit,
                                          const char *name) {
    for (int i = 0; i < limit; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_INTERFACE_DECL) continue;
        Iron_InterfaceDecl *idecl = (Iron_InterfaceDecl *)d;
        if (idecl->name && strcmp(idecl->name, name) == 0) return idecl;
    }
    return NULL;
}

static bool has_method(Iron_Program *program, const char *type_name,
                       const char *method_name) {
    for (int m = 0; m < program->decl_count; m++) {
        Iron_Node *d = program->decls[m];
        if (!d || d->kind != IRON_NODE_METHOD_DECL) continue;
        Iron_MethodDecl *md = (Iron_MethodDecl *)d;
        if (md->type_name && md->method_name &&
            strcmp(md->type_name, type_name) == 0 &&
            strcmp(md->method_name, method_name) == 0)
            return true;
    }
    return false;
}

/* Parse `patch object <type> { <method source> }` and append every decl the
 * parser produces (the patch decl plus its hoisted method decls) to the
 * program. Nodes live in the caller's arena, spans point into the snippet
 * (its filename names the interface and method for diagnostics). */
static void synthesize_one(Iron_Program *program, Iron_Arena *arena,
                           Iron_DiagList *diags, const char *type_name,
                           const char *iface_name, Iron_FuncDecl *sig) {
    char *method_src = iron_print_ast((Iron_Node *)sig, NULL, arena);
    if (!method_src) return;

    Iron_StrBuf sb = iron_strbuf_create(256 + strlen(method_src));
    iron_strbuf_appendf(&sb, "patch object %s {\n%s\n}\n", type_name, method_src);
    size_t len = sb.len;
    char *snippet = iron_arena_strdup(arena, iron_strbuf_get(&sb), len);
    iron_strbuf_free(&sb);
    if (!snippet) return;

    size_t fname_len = strlen(iface_name) + strlen(sig->name) + 32;
    char *fname = (char *)iron_arena_alloc(arena, fname_len, 1);
    if (!fname) return;
    snprintf(fname, fname_len, "<default %s.%s for %s>", iface_name, sig->name, type_name);

    Iron_Lexer lexer = iron_lexer_create(snippet, fname, arena, diags);
    Iron_Token *tokens = iron_lex_all(&lexer);
    int token_count = (int)arrlen(tokens);
    Iron_Parser parser = iron_parser_create(tokens, token_count, snippet, fname,
                                            arena, diags);
    Iron_Node *ast = iron_parse(&parser);
    arrfree(tokens);
    if (!ast || ast->kind != IRON_NODE_PROGRAM) return;

    Iron_Program *sub = (Iron_Program *)ast;
    for (int i = 0; i < sub->decl_count; i++) {
        Iron_Node *d = sub->decls[i];
        if (!d) continue;
        arrput(program->decls, d);
        program->decl_count++;
    }
}

void iron_iface_synthesize_defaults(Iron_Program *program, Iron_Arena *arena,
                                    Iron_DiagList *diags) {
    if (!program || !program->decls) return;
    /* Snapshot: only user-written declarations are candidates; the decls we
     * append are never implementors or interfaces themselves. */
    int limit = program->decl_count;
    for (int i = 0; i < limit; i++) {
        Iron_Node *d = program->decls[i];
        if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
        if (!od->name || od->implements_count == 0 || !od->implements_names) continue;
        for (int j = 0; j < od->implements_count; j++) {
            const char *iface_name = od->implements_names[j];
            if (!iface_name) continue;
            Iron_InterfaceDecl *idecl = find_interface(program, limit, iface_name);
            if (!idecl) continue;
            for (int k = 0; k < idecl->method_count; k++) {
                Iron_Node *sig_node = idecl->method_sigs[k];
                if (!sig_node || sig_node->kind != IRON_NODE_FUNC_DECL) continue;
                Iron_FuncDecl *sig = (Iron_FuncDecl *)sig_node;
                if (!sig->body || !sig->name) continue;
                if (has_method(program, od->name, sig->name)) continue;
                synthesize_one(program, arena, diags, od->name, iface_name, sig);
            }
        }
    }
}
