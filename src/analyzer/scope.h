#ifndef IRON_SCOPE_H
#define IRON_SCOPE_H

#include "analyzer/types.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include <stdbool.h>

/* Forward declare Iron_Node to avoid pulling in all of ast.h. */
struct Iron_Node;

/* ── Scope kind ──────────────────────────────────────────────────────────── */

typedef enum {
    IRON_SCOPE_GLOBAL,
    IRON_SCOPE_MODULE,
    IRON_SCOPE_FUNCTION,
    IRON_SCOPE_BLOCK
} Iron_ScopeKind;

/* ── Symbol kind ─────────────────────────────────────────────────────────── */

typedef enum {
    IRON_SYM_VARIABLE,
    IRON_SYM_FUNCTION,
    IRON_SYM_METHOD,
    IRON_SYM_TYPE,
    IRON_SYM_ENUM,
    IRON_SYM_ENUM_VARIANT,
    IRON_SYM_INTERFACE,
    IRON_SYM_PARAM,
    IRON_SYM_FIELD
} Iron_SymbolKind;

/* ── Symbol ──────────────────────────────────────────────────────────────── */

typedef struct {
    const char        *name;
    Iron_SymbolKind    sym_kind;
    Iron_Type         *type;        /* resolved type; NULL until type-checked */
    struct Iron_Node  *decl_node;   /* back-pointer to AST declaration node */
    Iron_Span          span;        /* declaration location */
    bool               is_mutable;  /* true for var, false for val */
    bool               is_private;
} Iron_Symbol;

/* ── stb_ds hash map entry ───────────────────────────────────────────────── */

typedef struct {
    char         *key;    /* stb_ds string key (strdup-managed) */
    Iron_Symbol  *value;
} Iron_SymbolEntry;

/* ── Scope ───────────────────────────────────────────────────────────────── */

typedef struct Iron_Scope {
    struct Iron_Scope  *parent;
    Iron_SymbolEntry   *symbols;    /* stb_ds string-keyed hash map */
    Iron_ScopeKind      kind;
    const char         *owner_name; /* for debugging: function/type name or NULL */
} Iron_Scope;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Create a new scope with the given parent (NULL for the root/global scope). */
Iron_Scope *iron_scope_create(Iron_Arena *a, Iron_Scope *parent, Iron_ScopeKind kind);

/* Look up a symbol by name, walking the parent chain.
 * Returns NULL if not found. */
Iron_Symbol *iron_scope_lookup(Iron_Scope *s, const char *name);

/* Look up a symbol in the current scope only (does not walk parents).
 * Returns NULL if not found. */
Iron_Symbol *iron_scope_lookup_local(Iron_Scope *s, const char *name);

/* Define a symbol in the given scope.
 * Returns false (and does not insert) if the name is already defined in
 * this scope (duplicate detection). */
bool iron_scope_define(Iron_Scope *s, Iron_Arena *a, Iron_Symbol *sym);

/* Allocate and initialize an Iron_Symbol. */
Iron_Symbol *iron_symbol_create(Iron_Arena *a,
                                 const char *name,
                                 Iron_SymbolKind kind,
                                 struct Iron_Node *decl,
                                 Iron_Span span);

#endif /* IRON_SCOPE_H */
