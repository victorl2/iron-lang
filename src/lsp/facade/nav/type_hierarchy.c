/* Phase 3 Plan 05 Task 02 (NAV-11, D-08) -- typeHierarchy facade.
 *
 * Three entry points:
 *
 *   prepare    -- cursor on object / interface decl (or identifier
 *                  resolving to one) -> single TypeHierarchyItem with
 *                  the identity triple embedded in `data`.
 *
 *   supertypes -- for an object: walk Iron_ObjectDecl.extends_name +
 *                 each implements_names[] entry; one level eager
 *                 (D-08 LOCKED).  Iron interfaces have no `extends`
 *                 field today -> always empty for interfaces.
 *
 *   subtypes   -- for an interface: query ilsp_iface_ws_query_implementors
 *                 and convert each impl to a TypeHierarchyItem.  For
 *                 an object: scan the workspace index for objects
 *                 whose extends_name equals the target's name.  BOTH
 *                 branches poll cancel_flag between workspace entries
 *                 per D-16 (iteration-boundary cancellation).  Partial
 *                 results acceptable on cancel (W-1).
 *
 * One-level eager: no recursion; clients page via repeat calls (D-08). */

#include "lsp/facade/nav/type_hierarchy.h"

#include "lsp/facade/nav/iface_workspace.h"
#include "lsp/facade/nav/nav_common.h"
#include "lsp/facade/nav/node_at.h"
#include "lsp/facade/nav/patch_lookup.h"
#include "lsp/facade/nav/symbol_id.h"
#include "lsp/facade/compile.h"
#include "lsp/facade/span.h"
#include "lsp/server/server.h"
#include "lsp/store/document.h"
#include "lsp/store/workspace_index.h"
#include "analyzer/scope.h"
#include "parser/ast.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* LSP SymbolKind constants we emit in TypeHierarchyItem.kind. */
#define LSP_SYMBOL_KIND_CLASS     5
#define LSP_SYMBOL_KIND_INTERFACE 11

/* ── Helpers ─────────────────────────────────────────────────────── */

static IronLsp_SymbolId triple_for(const char *canonical_path,
                                     const char *decl_name,
                                     Iron_SymbolKind kind,
                                     Iron_Arena *arena) {
    Iron_Symbol fake = {0};
    fake.name = decl_name;
    fake.sym_kind = kind;
    fake.decl_node = NULL;
    return ilsp_symbol_id_derive(&fake, canonical_path, NULL, arena);
}

static IronLsp_TypeHierarchyItem item_from_object(
    const Iron_ObjectDecl      *od,
    const char                 *canonical_path,
    IronLsp_Document           *origin_doc,
    IronLsp_WorkspaceIndex     *wi,
    IronLsp_PositionEncoding    enc,
    Iron_Arena                 *arena) {
    IronLsp_TypeHierarchyItem it = {0};
    it.kind = LSP_SYMBOL_KIND_CLASS;
    it.name = od->name
        ? iron_arena_strdup(arena, od->name, strlen(od->name))
        : "";
    const char *cp = canonical_path ? canonical_path : "";
    it.triple = triple_for(cp, od->name ? od->name : "", IRON_SYM_TYPE, arena);
    it.uri = ilsp_nav_path_to_uri(cp, arena);
    if (!it.uri) it.uri = "";

    /* Range conversion: prefer the workspace_index entry's line index
     * when the target is not the open doc; otherwise use origin_doc. */
    IronLsp_IndexEntry *entry = wi
        ? ilsp_workspace_index_lookup(wi, cp) : NULL;
    if (entry) {
        it.range           = ilsp_nav_entry_span_to_range(entry, od->span, enc);
        it.selection_range = ilsp_nav_entry_span_to_range(entry, od->span, enc);
    } else if (origin_doc) {
        it.range           = ilsp_span_to_lsp_range(od->span, origin_doc, enc);
        it.selection_range = ilsp_span_to_lsp_range(od->span, origin_doc, enc);
    }
    return it;
}

static IronLsp_TypeHierarchyItem item_from_interface(
    const Iron_InterfaceDecl   *ifc,
    const char                 *canonical_path,
    IronLsp_Document           *origin_doc,
    IronLsp_WorkspaceIndex     *wi,
    IronLsp_PositionEncoding    enc,
    Iron_Arena                 *arena) {
    IronLsp_TypeHierarchyItem it = {0};
    it.kind = LSP_SYMBOL_KIND_INTERFACE;
    it.name = ifc->name
        ? iron_arena_strdup(arena, ifc->name, strlen(ifc->name))
        : "";
    const char *cp = canonical_path ? canonical_path : "";
    it.triple = triple_for(cp, ifc->name ? ifc->name : "",
                             IRON_SYM_TYPE, arena);
    it.uri = ilsp_nav_path_to_uri(cp, arena);
    if (!it.uri) it.uri = "";
    IronLsp_IndexEntry *entry = wi
        ? ilsp_workspace_index_lookup(wi, cp) : NULL;
    if (entry) {
        it.range           = ilsp_nav_entry_span_to_range(entry, ifc->span, enc);
        it.selection_range = ilsp_nav_entry_span_to_range(entry, ifc->span, enc);
    } else if (origin_doc) {
        it.range           = ilsp_span_to_lsp_range(ifc->span, origin_doc, enc);
        it.selection_range = ilsp_span_to_lsp_range(ifc->span, origin_doc, enc);
    }
    return it;
}

/* Search `program` for an object or interface decl with matching name.
 * Returns via out_obj or out_ifc; exactly one is set when found. */
static bool find_decl_by_name(const Iron_Program   *program,
                                const char           *name,
                                Iron_ObjectDecl     **out_obj,
                                Iron_InterfaceDecl  **out_ifc) {
    if (out_obj) *out_obj = NULL;
    if (out_ifc) *out_ifc = NULL;
    if (!program || !name || !*name) return false;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        if (d->kind == IRON_NODE_OBJECT_DECL) {
            Iron_ObjectDecl *od = (Iron_ObjectDecl *)d;
            /* PATCH-02 (Plan 11-02): patches are NOT returned as native
             * subtypes via this find-by-name path; they are surfaced as
             * virtual Method entries via ilsp_patch_for_each_method in
             * the subtypes-resolver below. Continue past patches here so
             * they don't pollute the native-subtype index. */
            if (od->is_patch) continue;
            if (od->name && strcmp(od->name, name) == 0) {
                if (out_obj) *out_obj = od;
                return true;
            }
        } else if (d->kind == IRON_NODE_INTERFACE_DECL) {
            Iron_InterfaceDecl *ifc = (Iron_InterfaceDecl *)d;
            if (ifc->name && strcmp(ifc->name, name) == 0) {
                if (out_ifc) *out_ifc = ifc;
                return true;
            }
        }
    }
    return false;
}

/* ── prepareTypeHierarchy ────────────────────────────────────────── */

void ilsp_facade_nav_prepare_type_hierarchy(
    IronLsp_Server              *server,
    IronLsp_Document            *doc,
    IronLsp_Position             pos,
    _Atomic bool                *cancel,
    Iron_Arena                  *arena,
    IronLsp_TypeHierarchyItem  **out,
    size_t                      *out_n) {
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!server || !doc || !arena || !out || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;

    Iron_Arena    walk_arena = iron_arena_create(64 * 1024);
    Iron_DiagList walk_diags = iron_diaglist_create();
    IronLsp_CompileRequest req = { .version = doc->version,
                                    .cancel_flag = cancel };
    Iron_Program *program = ilsp_facade_compile_for_nav(
        doc, &req, &walk_arena, &walk_diags);
    if (!program) goto done;
    if (cancel && atomic_load(cancel)) goto done;

    Iron_Node *node = ilsp_nav_node_at(doc, program, pos, enc);
    if (!node) goto done;

    const char *doc_key = doc->uri ? doc->uri : "";

    IronLsp_TypeHierarchyItem *arr =
        (IronLsp_TypeHierarchyItem *)iron_arena_alloc(
            arena, sizeof(*arr), _Alignof(IronLsp_TypeHierarchyItem));
    if (!arr) goto done;

    if (node->kind == IRON_NODE_OBJECT_DECL) {
        arr[0] = item_from_object((const Iron_ObjectDecl *)node,
                                    doc_key, doc, server->workspace_index,
                                    enc, arena);
        *out = arr;
        *out_n = 1;
    } else if (node->kind == IRON_NODE_INTERFACE_DECL) {
        arr[0] = item_from_interface((const Iron_InterfaceDecl *)node,
                                       doc_key, doc, server->workspace_index,
                                       enc, arena);
        *out = arr;
        *out_n = 1;
    } else if (node->kind == IRON_NODE_IDENT) {
        /* Ident resolving to object or interface type. */
        Iron_Ident *id = (Iron_Ident *)node;
        if (id->resolved_sym && id->resolved_sym->decl_node) {
            Iron_Node *d = id->resolved_sym->decl_node;
            if (d->kind == IRON_NODE_OBJECT_DECL) {
                arr[0] = item_from_object((const Iron_ObjectDecl *)d,
                                            doc_key, doc, server->workspace_index,
                                            enc, arena);
                *out = arr;
                *out_n = 1;
            } else if (d->kind == IRON_NODE_INTERFACE_DECL) {
                arr[0] = item_from_interface((const Iron_InterfaceDecl *)d,
                                               doc_key, doc, server->workspace_index,
                                               enc, arena);
                *out = arr;
                *out_n = 1;
            }
        }
    }

done:
    iron_diaglist_free(&walk_diags);
    iron_arena_free(&walk_arena);
}

/* ── typeHierarchy/supertypes ─────────────────────────────────────── */

void ilsp_facade_nav_type_hierarchy_supertypes(
    IronLsp_Server              *server,
    IronLsp_SymbolId             item_triple,
    _Atomic bool                *cancel,
    Iron_Arena                  *arena,
    IronLsp_TypeHierarchyItem  **out,
    size_t                      *out_n) {
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!server || !arena || !out || !out_n) return;
    (void)cancel;

    IronLsp_PositionEncoding enc = server->position_encoding;
    IronLsp_WorkspaceIndex *wi = server->workspace_index;

    /* Look up the entry owning this triple by canonical_path. */
    if (!item_triple.canonical_path) return;
    IronLsp_IndexEntry *entry = wi
        ? ilsp_workspace_index_lookup(wi, item_triple.canonical_path)
        : NULL;
    if (!entry || !entry->program) return;

    if (entry->analyzed == false && wi) {
        (void)ilsp_workspace_index_analyze_lazy(wi, entry, cancel);
    }

    /* Find the decl matching the triple's simple name (the name_path
     * ends with the decl name; strip any prefix). */
    const char *simple_name = item_triple.name_path ? item_triple.name_path : "";
    const char *dot = strrchr(simple_name, '.');
    if (dot) simple_name = dot + 1;

    Iron_ObjectDecl    *od = NULL;
    Iron_InterfaceDecl *ifc = NULL;
    if (!find_decl_by_name(entry->program, simple_name, &od, &ifc)) return;

    if (ifc) {
        /* Iron interfaces have no `extends` field today (D-08). */
        return;
    }
    if (!od) return;

    /* Object: walk extends_name + each implements_names[] entry. */
    IronLsp_TypeHierarchyItem *buf = NULL;  /* stb_ds dynamic */

    /* Extends chain (one-level: emit direct parent only -- clients
     * call supertypes again to step further up). */
    if (od->extends_name && *od->extends_name) {
        Iron_ObjectDecl    *pod = NULL;
        Iron_InterfaceDecl *pifc = NULL;
        if (find_decl_by_name(entry->program, od->extends_name, &pod, &pifc)) {
            if (pod) {
                IronLsp_TypeHierarchyItem it = item_from_object(
                    pod, item_triple.canonical_path, NULL, wi, enc, arena);
                arrput(buf, it);
            } else if (pifc) {
                IronLsp_TypeHierarchyItem it = item_from_interface(
                    pifc, item_triple.canonical_path, NULL, wi, enc, arena);
                arrput(buf, it);
            }
        }
    }

    /* Implements list. */
    for (int i = 0; i < od->implements_count; i++) {
        const char *name = od->implements_names[i];
        if (!name || !*name) continue;
        Iron_ObjectDecl    *pod = NULL;
        Iron_InterfaceDecl *pifc = NULL;
        if (find_decl_by_name(entry->program, name, &pod, &pifc)) {
            if (pifc) {
                IronLsp_TypeHierarchyItem it = item_from_interface(
                    pifc, item_triple.canonical_path, NULL, wi, enc, arena);
                arrput(buf, it);
            } else if (pod) {
                IronLsp_TypeHierarchyItem it = item_from_object(
                    pod, item_triple.canonical_path, NULL, wi, enc, arena);
                arrput(buf, it);
            }
        }
    }

    ptrdiff_t n = arrlen(buf);
    if (n <= 0) { arrfree(buf); return; }

    IronLsp_TypeHierarchyItem *arr = (IronLsp_TypeHierarchyItem *)
        iron_arena_alloc(arena, (size_t)n * sizeof(*arr),
                          _Alignof(IronLsp_TypeHierarchyItem));
    if (!arr) { arrfree(buf); return; }
    for (ptrdiff_t i = 0; i < n; i++) arr[i] = buf[i];
    arrfree(buf);
    *out = arr;
    *out_n = (size_t)n;
}

/* ── typeHierarchy/subtypes ───────────────────────────────────────── */

/* PATCH-02 (Plan 11-02): build the "[patch from <relpath>]" detail string
 * for a patch-method virtual entry. Strips wi->workspace_root prefix when
 * present so the rendered path tracks the project root the user opened.
 * Falls back to the absolute filename when no prefix match. */
static const char *build_patch_detail(const Iron_ObjectDecl   *patch_od,
                                        IronLsp_WorkspaceIndex  *wi,
                                        Iron_Arena              *arena) {
    const char *od_path = (patch_od && patch_od->span.filename)
        ? patch_od->span.filename : "<unknown>";
    const char *rel = od_path;
    if (wi && wi->workspace_root && *wi->workspace_root) {
        size_t root_len = strlen(wi->workspace_root);
        if (root_len > 0 && strncmp(od_path, wi->workspace_root, root_len) == 0) {
            rel = od_path + root_len;
            if (*rel == '/') rel++;
        }
    }
    size_t need = strlen("[patch from ") + strlen(rel) + 2;  /* ']' + NUL */
    char *buf = (char *)iron_arena_alloc(arena, need, 1);
    if (!buf) return NULL;
    snprintf(buf, need, "[patch from %s]", rel);
    return buf;
}

/* PATCH-02 (Plan 11-02): visitor state for the patch-method walk; closes
 * over the result accumulator + position-encoding context so each yielded
 * (Iron_MethodDecl, Iron_ObjectDecl) pair becomes a TypeHierarchyItem
 * with kind=SymbolKind.Method=6 and detail="[patch from <relpath>]". */
struct th_patch_emit_state {
    IronLsp_TypeHierarchyItem **buf;          /* stb_ds dynamic */
    Iron_Arena                 *arena;
    IronLsp_WorkspaceIndex     *wi;
    IronLsp_PositionEncoding    enc;
};

static bool th_patch_emit_visit(Iron_MethodDecl *m,
                                  Iron_ObjectDecl *p,
                                  void           *ud) {
    struct th_patch_emit_state *st = (struct th_patch_emit_state *)ud;
    if (!st || !m || !p || !m->method_name) return true;

    const char *od_path = p->span.filename ? p->span.filename : "";
    IronLsp_IndexEntry *patch_entry = (st->wi && *od_path)
        ? ilsp_workspace_index_lookup(st->wi, od_path) : NULL;

    IronLsp_TypeHierarchyItem it = {0};
    it.name = iron_arena_strdup(st->arena, m->method_name,
                                 strlen(m->method_name));
    it.kind = 6;   /* LSP SymbolKind.Method per CONTEXT D-06 */
    it.detail = build_patch_detail(p, st->wi, st->arena);
    it.uri = ilsp_nav_path_to_uri(od_path, st->arena);
    if (!it.uri) it.uri = "";
    /* Range from the method span: prefer workspace_index entry's line
     * index when available so the editor jumps to the correct line on
     * click. */
    if (patch_entry) {
        it.range           = ilsp_nav_entry_span_to_range(
            patch_entry, m->span, st->enc);
        it.selection_range = ilsp_nav_entry_span_to_range(
            patch_entry, m->span, st->enc);
    }
    /* Triple keyed on the patched method identity. The owner_sym route
     * matches symbol_id.c's existing patch path; if owner_sym is NULL
     * (defensive) fall back to a name-based triple keyed on the patch
     * target. Either way the data round-trips through prepare->
     * subtypes-resolver if a future caller follows the click. */
    if (m->owner_sym) {
        it.triple = ilsp_symbol_id_derive(m->owner_sym, od_path, NULL,
                                            st->arena);
    } else {
        Iron_Symbol fake = {0};
        fake.name = m->method_name;
        fake.sym_kind = IRON_SYM_METHOD;
        fake.decl_node = (Iron_Node *)m;
        it.triple = ilsp_symbol_id_derive(&fake, od_path, NULL, st->arena);
    }
    arrput(*st->buf, it);
    return true;
}

/* PATCH-02 (Plan 11-02): qsort comparator — sort interleaved children
 * (real subtypes + patch methods) by range.start.line ascending so the
 * editor's tree-view ordering tracks declaration order in the workspace
 * (CONTEXT D-08 + RESEARCH Discretion). */
static int th_item_cmp_by_line(const void *a, const void *b) {
    const IronLsp_TypeHierarchyItem *ia = (const IronLsp_TypeHierarchyItem *)a;
    const IronLsp_TypeHierarchyItem *ib = (const IronLsp_TypeHierarchyItem *)b;
    uint32_t la = ia->range.start.line;
    uint32_t lb = ib->range.start.line;
    if (la < lb) return -1;
    if (la > lb) return  1;
    /* Tie-break on character so ordering is stable when two children
     * land on the same line (unlikely but defensive). */
    uint32_t ca = ia->range.start.character;
    uint32_t cb = ib->range.start.character;
    if (ca < cb) return -1;
    if (ca > cb) return  1;
    return 0;
}

void ilsp_facade_nav_type_hierarchy_subtypes(
    IronLsp_Server              *server,
    IronLsp_SymbolId             item_triple,
    _Atomic bool                *cancel,
    Iron_Arena                  *arena,
    IronLsp_TypeHierarchyItem  **out,
    size_t                      *out_n) {
    if (out) *out = NULL;
    if (out_n) *out_n = 0;
    if (!server || !arena || !out || !out_n) return;

    IronLsp_PositionEncoding enc = server->position_encoding;
    IronLsp_WorkspaceIndex *wi = server->workspace_index;
    if (!wi) return;

    /* Look up the entry owning this triple to determine if target is
     * an object or interface (which path to take). */
    if (!item_triple.canonical_path) return;
    IronLsp_IndexEntry *entry = ilsp_workspace_index_lookup(
        wi, item_triple.canonical_path);
    if (!entry || !entry->program) return;
    if (entry->analyzed == false) {
        (void)ilsp_workspace_index_analyze_lazy(wi, entry, cancel);
    }

    const char *simple_name = item_triple.name_path ? item_triple.name_path : "";
    const char *dot = strrchr(simple_name, '.');
    if (dot) simple_name = dot + 1;

    Iron_ObjectDecl    *od = NULL;
    Iron_InterfaceDecl *ifc = NULL;
    if (!find_decl_by_name(entry->program, simple_name, &od, &ifc)) return;

    IronLsp_TypeHierarchyItem *buf = NULL;   /* stb_ds dynamic */

    if (ifc) {
        /* Interface path: query iface_workspace for implementors. */
        IronLsp_IfaceWorkspace *iws = ilsp_workspace_index_iface_ws(wi);
        if (!iws) return;
        IronLsp_ImplEntry *impls = NULL;
        size_t impls_n = 0;
        ilsp_iface_ws_query_implementors(iws, item_triple, arena,
                                           &impls, &impls_n);
        for (size_t i = 0; i < impls_n; i++) {
            /* D-16: poll cancel between per-iteration boundaries. */
            if (cancel && atomic_load(cancel)) break;
            const IronLsp_ImplEntry *ie = &impls[i];
            /* Resolve the implementor's decl via workspace_index. */
            IronLsp_IndexEntry *impl_entry = ilsp_workspace_index_lookup(
                wi, ie->canonical_path);
            Iron_ObjectDecl *iod = NULL;
            Iron_InterfaceDecl *iifc = NULL;
            if (impl_entry && impl_entry->program) {
                find_decl_by_name(impl_entry->program,
                                    ie->object_name ? ie->object_name : "",
                                    &iod, &iifc);
            }
            if (iod) {
                IronLsp_TypeHierarchyItem it = item_from_object(
                    iod, ie->canonical_path, NULL, wi, enc, arena);
                arrput(buf, it);
            } else {
                /* Fallback: synthesize from the impl entry spans. */
                IronLsp_TypeHierarchyItem it = {0};
                it.name = ie->object_name ? ie->object_name : "";
                it.kind = LSP_SYMBOL_KIND_CLASS;
                it.uri = ilsp_nav_path_to_uri(ie->canonical_path, arena);
                if (!it.uri) it.uri = "";
                it.triple = ie->object_triple;
                if (impl_entry) {
                    it.range = ilsp_nav_entry_span_to_range(
                        impl_entry, ie->object_decl_span, enc);
                    it.selection_range = ilsp_nav_entry_span_to_range(
                        impl_entry, ie->object_ident_span, enc);
                }
                arrput(buf, it);
            }
        }
    } else if (od) {
        /* Object path: scan the workspace_index for objects whose
         * extends_name matches the target's name.  D-16 + W-1: poll
         * cancel between workspace_index entries; break on cancel
         * and return partial results. */
        size_t n = 0;
        char **paths = ilsp_workspace_index_snapshot_paths(wi, &n);
        if (paths) {
            for (size_t i = 0; i < n; i++) {
                /* D-16 iteration-boundary cancel poll (W-1). */
                if (cancel && atomic_load(cancel)) break;
                if (!paths[i]) continue;
                IronLsp_IndexEntry *ee = ilsp_workspace_index_lookup(wi, paths[i]);
                if (!ee || !ee->program) continue;
                /* analyze_lazy is a no-op if already analyzed. */
                if (!ee->analyzed) {
                    (void)ilsp_workspace_index_analyze_lazy(wi, ee, cancel);
                    if (cancel && atomic_load(cancel)) break;
                }
                Iron_Program *p = ee->program;
                if (!p) continue;
                for (int j = 0; j < p->decl_count; j++) {
                    Iron_Node *d = p->decls[j];
                    if (!d || d->kind != IRON_NODE_OBJECT_DECL) continue;
                    Iron_ObjectDecl *ood = (Iron_ObjectDecl *)d;
                    /* PATCH-02 (Plan 11-02): see comment at line ~138.
                     * Patches are surfaced separately via
                     * ilsp_patch_for_each_method below. */
                    if (ood->is_patch) continue;
                    if (!ood->extends_name ||
                        strcmp(ood->extends_name,
                               od->name ? od->name : "") != 0) continue;
                    IronLsp_TypeHierarchyItem it = item_from_object(
                        ood, paths[i], NULL, wi, enc, arena);
                    arrput(buf, it);
                }
            }
            for (size_t i = 0; i < n; i++) free(paths[i]);
            free(paths);
        }
    }

    /* PATCH-02 (Plan 11-02): emit virtual TypeHierarchyItem entries for
     * patched methods on the requested type. One item per patch method;
     * kind=SymbolKind.Method (6); detail="[patch from <relpath>]".
     * For an interface target this returns nothing (primitives do not
     * implement interfaces today and patches of user objects do not show
     * up under typeHierarchy/subtypes of the interface itself — that's
     * what textDocument/implementation handles in PATCH-01). For an
     * object target (including a future primitive synthesis path per
     * RESEARCH Pitfall 2) the walker yields each (md, patch_od) pair;
     * visibility filter applied internally on the enclosing patch
     * ObjectDecl per Plan 11-01 helper internals. */
    if (od && simple_name && *simple_name) {
        struct th_patch_emit_state pstate = {
            .buf   = &buf,
            .arena = arena,
            .wi    = wi,
            .enc   = enc,
        };
        ilsp_patch_for_each_method(
            entry->program, wi, simple_name,
            item_triple.canonical_path, th_patch_emit_visit, &pstate, cancel);
    }

    ptrdiff_t nb = arrlen(buf);
    if (nb <= 0) { arrfree(buf); return; }

    IronLsp_TypeHierarchyItem *arr = (IronLsp_TypeHierarchyItem *)
        iron_arena_alloc(arena, (size_t)nb * sizeof(*arr),
                          _Alignof(IronLsp_TypeHierarchyItem));
    if (!arr) { arrfree(buf); return; }
    for (ptrdiff_t i = 0; i < nb; i++) arr[i] = buf[i];
    arrfree(buf);

    /* PATCH-02 (Plan 11-02) D-08 + RESEARCH Discretion: sort the
     * combined result (native subtypes + patch methods) by
     * range.start.line ascending so the editor's tree-view ordering
     * tracks declaration order in the workspace. Stable on ties via the
     * character-column tie-breaker inside the comparator. */
    if (nb > 1) {
        qsort(arr, (size_t)nb, sizeof(*arr), th_item_cmp_by_line);
    }
    *out = arr;
    *out_n = (size_t)nb;
}
