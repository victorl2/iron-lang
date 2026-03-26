#include "analyzer/scope.h"
#include "vendor/stb_ds.h"
#include <string.h>
#include <stddef.h>

/* ── Scope creation ──────────────────────────────────────────────────────── */

Iron_Scope *iron_scope_create(Iron_Arena *a, Iron_Scope *parent, Iron_ScopeKind kind) {
    Iron_Scope *s = ARENA_ALLOC(a, Iron_Scope);
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->parent     = parent;
    s->kind       = kind;
    s->owner_name = NULL;
    s->symbols    = NULL;
    /* Initialize stb_ds string-keyed hash map with strdup key management. */
    sh_new_strdup(s->symbols);
    return s;
}

/* ── Symbol lookup ───────────────────────────────────────────────────────── */

Iron_Symbol *iron_scope_lookup_local(Iron_Scope *s, const char *name) {
    if (!s || !name) return NULL;
    ptrdiff_t idx = shgeti(s->symbols, name);
    if (idx < 0) return NULL;
    return s->symbols[idx].value;
}

Iron_Symbol *iron_scope_lookup(Iron_Scope *s, const char *name) {
    Iron_Scope *cur = s;
    while (cur) {
        Iron_Symbol *found = iron_scope_lookup_local(cur, name);
        if (found) return found;
        cur = cur->parent;
    }
    return NULL;
}

/* ── Symbol definition ───────────────────────────────────────────────────── */

bool iron_scope_define(Iron_Scope *s, Iron_Arena *a, Iron_Symbol *sym) {
    (void)a; /* arena reserved for future use */
    if (!s || !sym || !sym->name) return false;
    /* Reject duplicate definitions in the same scope */
    if (shgeti(s->symbols, sym->name) >= 0) return false;
    shput(s->symbols, sym->name, sym);
    return true;
}

/* ── Symbol creation ─────────────────────────────────────────────────────── */

Iron_Symbol *iron_symbol_create(Iron_Arena *a,
                                 const char *name,
                                 Iron_SymbolKind kind,
                                 struct Iron_Node *decl,
                                 Iron_Span span) {
    Iron_Symbol *sym = ARENA_ALLOC(a, Iron_Symbol);
    if (!sym) return NULL;
    memset(sym, 0, sizeof(*sym));
    sym->name       = name;
    sym->sym_kind   = kind;
    sym->type       = NULL;
    sym->decl_node  = decl;
    sym->span       = span;
    sym->is_mutable = false;
    sym->is_private = false;
    return sym;
}
