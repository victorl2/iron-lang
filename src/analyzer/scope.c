#include "analyzer/scope.h"
#include "vendor/stb_ds.h"
#include <string.h>
#include <stddef.h>

/* ── Scope creation ──────────────────────────────────────────────────────── */

Iron_Scope *iron_scope_create(Iron_Arena *a, Iron_Scope *parent, Iron_ScopeKind kind) {
    Iron_Scope *s = ARENA_ALLOC(a, Iron_Scope);
    /* HARD-09 REPLACE (CR-03, scope.c:iron_scope_create): return NULL on
     * arena OOM. Callers (iron_resolve and push_scope) fall back to the
     * parent scope so resolution can keep emitting diagnostics in-place
     * rather than aborting the whole compilation. */
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->parent     = parent;
    s->kind       = kind;
    s->owner_name = NULL;
    s->symbols    = NULL;
    /* FIX-03 / AUDIT-04 §3: SAFETY — initialize stb_ds string-keyed hash map
     * with strdup key management. The map's backing buffer is heap-managed
     * and is NEVER explicitly freed; it leaks to process exit when the
     * analyzer arena is freed. This is a deliberate, bounded tradeoff:
     *   (a) there is no iron_scope_free entry point in the entire codebase
     *       (grep `iron_scope_free` src/ → 0 hits); scopes are only ever
     *       allocated via iron_scope_create above, never destroyed
     *       individually.
     *   (b) the scope's containing arena (`a`) is the compilation-unit
     *       arena, and the scope's lifetime is coupled to that arena by
     *       construction — when the arena is freed (at batch-compile exit
     *       via iron_arena_free), the stb_ds shmap leaks along with every
     *       other non-arena-tracked heap block in the compiler. Total
     *       leak per compile is O(symbol_count * key_length) — bounded by
     *       source size.
     *   (c) migrating the stb_ds map to arena storage would require every
     *       shput/shgeti call (grep `s->symbols` src/analyzer → ~12 sites)
     *       to switch to a hand-rolled arena-keyed hashmap. Out of Phase 67
     *       scope per REQUIREMENTS.md (see "rewriting arena allocator to a
     *       tracked/ref-counted model" out-of-scope item). */
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
    /* HARD-09 REPLACE (CR-03, scope.c:iron_symbol_create): return NULL on
     * arena OOM. Callers (resolve.c:define_sym, resolve.c top-level
     * collect_decl, and builtin registration) already have a degraded
     * but safe path — symbol not inserted into scope, subsequent lookups
     * see "undefined identifier" which is semantically fine for OOM. */
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
