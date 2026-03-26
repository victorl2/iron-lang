#ifndef IRON_CODEGEN_H
#define IRON_CODEGEN_H

#include "parser/ast.h"
#include "analyzer/scope.h"
#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "util/strbuf.h"

/* stb_ds is used for defer_stacks and emitted_optionals arrays. */
#include "vendor/stb_ds.h"

typedef struct {
    Iron_Arena    *arena;
    Iron_DiagList *diags;
    Iron_Scope    *global_scope;
    Iron_Program  *program;       /* the program being compiled */

    /* Output buffers — one per emission section */
    Iron_StrBuf    includes;        /* #include directives */
    Iron_StrBuf    forward_decls;   /* typedef struct X X; */
    Iron_StrBuf    struct_bodies;   /* struct X { ... }; */
    Iron_StrBuf    enum_defs;       /* typedef enum { ... } X; */
    Iron_StrBuf    global_consts;   /* top-level val/var declarations */
    Iron_StrBuf    prototypes;      /* void X_foo(X* self, ...); */
    Iron_StrBuf    implementations; /* void X_foo(X* self, ...) { ... } */
    Iron_StrBuf    main_wrapper;    /* int main() { ... } */

    /* Defer stack — stb_ds array of (stb_ds array of Iron_Node*) */
    Iron_Node  ***defer_stacks;   /* outer array indexed by depth */
    int           defer_depth;
    int           function_scope_depth;

    /* Type tags for inheritance */
    int           next_type_tag;

    /* Indentation level */
    int           indent;

    /* Optional struct registry — track emitted Iron_Optional_T names */
    char        **emitted_optionals; /* stb_ds string array */

    /* Monomorphization registry — tracks emitted generic instantiations.
     * Maps mangled_name -> bool (true = already emitted). */
    struct { char *key; bool value; } *mono_registry;  /* stb_ds string hash map */

    /* Lambda counter — for unique naming of lifted lambda/spawn/parallel functions */
    int           lambda_counter;
    int           spawn_counter;
    int           parallel_counter;

    /* Current function name — for lambda naming (set during func/method emit) */
    const char   *current_func_name;

    /* Lifted functions — spawn bodies, parallel-for chunks, and lambdas */
    Iron_StrBuf   lifted_funcs;
} Iron_Codegen;

/* Generate C code for the given analyzed program.
 * Returns the complete C source as a string (arena-allocated).
 * Returns NULL if the program has semantic errors.
 */
const char *iron_codegen(Iron_Program *program, Iron_Scope *global_scope,
                         Iron_Arena *arena, Iron_DiagList *diags);

/* Collect outer-variable captures from a node subtree, excluding params.
 * Returns an stb_ds array of const char* names (caller must arrfree). */
const char **collect_captures(Iron_Node *body, Iron_Node **params,
                               int param_count);

/* --- Internal functions used across gen_*.c files --- */

/* Map Iron type to C type string. E.g., IRON_TYPE_INT -> "int64_t" */
const char *iron_type_to_c(const Iron_Type *t, Iron_Codegen *ctx);

/* Mangle an Iron name to C: "Player" -> "Iron_Player" */
const char *iron_mangle_name(const char *name, Iron_Arena *arena);

/* Mangle a method name: ("Player", "update") -> "Iron_Player_update" */
const char *iron_mangle_method(const char *type_name, const char *method_name,
                                Iron_Arena *arena);

/* Emit indentation to the given buffer */
void codegen_indent(Iron_StrBuf *sb, int level);

/* Emit expression to buffer */
void emit_expr(Iron_StrBuf *sb, Iron_Node *node, Iron_Codegen *ctx);

/* Emit statement to buffer */
void emit_stmt(Iron_StrBuf *sb, Iron_Node *node, Iron_Codegen *ctx);

/* Emit block body (statements within braces) */
void emit_block(Iron_StrBuf *sb, Iron_Block *block, Iron_Codegen *ctx);

/* Emit defer drain from current depth down to target_depth */
void emit_defers(Iron_StrBuf *sb, Iron_Codegen *ctx, int target_depth);

/* Ensure Iron_Optional_T struct is emitted for the given type */
void ensure_optional_type(Iron_Codegen *ctx, const Iron_Type *inner);

/* Emit a lambda expression, lifting the body to ctx->lifted_funcs.
 * enclosing_name is the name of the enclosing function (for naming).
 * Writes to sb the expression that produces the callable at the use site. */
void emit_lambda(Iron_StrBuf *sb, Iron_Node *node, Iron_Codegen *ctx,
                 const char *enclosing_name);

/* Emit a function prototype to prototypes buffer */
void emit_func_prototype(Iron_Codegen *ctx, Iron_FuncDecl *fd);

/* Emit a method prototype to prototypes buffer */
void emit_method_prototype(Iron_Codegen *ctx, Iron_MethodDecl *md);

/* Emit a function implementation to implementations buffer */
void emit_func_impl(Iron_Codegen *ctx, Iron_FuncDecl *fd);

/* Emit a method implementation to implementations buffer */
void emit_method_impl(Iron_Codegen *ctx, Iron_MethodDecl *md);

/* Emit interface vtable struct and ref type for the given interface.
 * E.g., Iron_Drawable_vtable { void (*draw)(void* self); } and Iron_Drawable_ref */
void emit_interface_vtable_struct(Iron_Codegen *ctx,
                                  Iron_InterfaceDecl *iface);

/* Emit a static vtable instance for a given (type, interface) pair.
 * Emitted into implementations so function names are already prototyped.
 * E.g., static Iron_Drawable_vtable Iron_Player_Drawable_vtable = { .draw = ... } */
void emit_vtable_instance(Iron_Codegen *ctx, const char *type_name,
                           Iron_InterfaceDecl *iface);

/* Compute the mangled name for a generic instantiation.
 * E.g., "List" + [Iron_Enemy] -> "Iron_List_Iron_Enemy" (arena-allocated) */
const char *mangle_generic(const char *base, Iron_Type **args, int count,
                            Iron_Arena *a);

/* Ensure a monomorphized generic type is emitted.
 * Checks mono_registry for duplicates. Emits stub struct if new.
 * Returns the mangled name (arena-allocated). */
const char *ensure_monomorphized_type(Iron_Codegen *ctx, const char *base_name,
                                       Iron_Type **args, int count);

#endif /* IRON_CODEGEN_H */
