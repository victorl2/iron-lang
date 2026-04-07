#include "analyzer/iface_collect.h"
#include "vendor/stb_ds.h"
#include <string.h>
#include <stdlib.h>

/* ── Comparator for sorting implementors alphabetically ─────────────────── */

static int cmp_impl_by_name(const void *a, const void *b) {
    const Iron_IfaceImpl *ia = (const Iron_IfaceImpl *)a;
    const Iron_IfaceImpl *ib = (const Iron_IfaceImpl *)b;
    return strcmp(ia->type_name, ib->type_name);
}

/* ── Build the interface registry ───────────────────────────────────────── */

Iron_IfaceRegistry iron_iface_collect(Iron_Program *program, Iron_Arena *arena) {
    Iron_IfaceRegistry reg = { .map = NULL, .arena = arena };
    sh_new_arena(reg.map);

    /* Pass 1: register all interfaces */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (decl->kind != IRON_NODE_INTERFACE_DECL) continue;
        Iron_InterfaceDecl *iface = (Iron_InterfaceDecl *)decl;

        Iron_IfaceEntry entry = {
            .iface_name  = iface->name,
            .iface_decl  = iface,
            .iface_type  = NULL,
            .impls       = NULL,
            .impl_count  = 0,
            .alive_count = 0,
        };
        shput(reg.map, iface->name, entry);
    }

    /* Pass 2: scan all object declarations for `impl` clauses */
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *decl = program->decls[i];
        if (decl->kind != IRON_NODE_OBJECT_DECL) continue;
        Iron_ObjectDecl *obj = (Iron_ObjectDecl *)decl;

        for (int j = 0; j < obj->implements_count; j++) {
            const char *iface_name = obj->implements_names[j];
            int idx = shgeti(reg.map, iface_name);
            if (idx < 0) continue;  /* unknown interface — type checker will report */

            Iron_IfaceImpl impl = {
                .type_name = obj->name,
                .tag       = 0,  /* assigned after sorting */
                .decl      = obj,
                .type      = NULL,
                .is_alive  = true,  /* assume alive until dead elimination */
            };
            arrput(reg.map[idx].value.impls, impl);
            reg.map[idx].value.impl_count++;
        }
    }

    /* Pass 3: sort implementors alphabetically and assign canonical tags */
    for (int i = 0; i < shlen(reg.map); i++) {
        Iron_IfaceEntry *entry = &reg.map[i].value;
        if (entry->impl_count > 1) {
            qsort(entry->impls, (size_t)entry->impl_count,
                  sizeof(Iron_IfaceImpl), cmp_impl_by_name);
        }
        for (int j = 0; j < entry->impl_count; j++) {
            entry->impls[j].tag = j;
        }
        entry->alive_count = entry->impl_count;
    }

    return reg;
}

/* ── Lookup ─────────────────────────────────────────────────────────────── */

Iron_IfaceEntry *iron_iface_lookup(Iron_IfaceRegistry *reg, const char *iface_name) {
    int idx = shgeti(reg->map, iface_name);
    if (idx < 0) return NULL;
    return &reg->map[idx].value;
}

/* ── Dead implementor elimination ───────────────────────────────────────── */

typedef struct {
    const char *target;
    bool found;
} InstCheckCtx;

static bool inst_check_visit(Iron_Visitor *v, Iron_Node *node) {
    InstCheckCtx *ctx = (InstCheckCtx *)v->ctx;
    if (ctx->found) return false;

    if (node->kind == IRON_NODE_CONSTRUCT) {
        Iron_ConstructExpr *ce = (Iron_ConstructExpr *)node;
        if (strcmp(ce->type_name, ctx->target) == 0) {
            ctx->found = true;
            return false;
        }
    }
    return true;
}

static bool type_is_instantiated(Iron_Program *program, const char *type_name) {
    InstCheckCtx ctx = { .target = type_name, .found = false };
    Iron_Visitor vis = { .ctx = &ctx, .visit_node = inst_check_visit, .post_visit = NULL };
    iron_ast_walk((Iron_Node *)program, &vis);
    return ctx.found;
}

void iron_iface_eliminate_dead(Iron_IfaceRegistry *reg, Iron_Program *program) {
    for (int i = 0; i < shlen(reg->map); i++) {
        Iron_IfaceEntry *entry = &reg->map[i].value;
        int alive = 0;
        for (int j = 0; j < entry->impl_count; j++) {
            Iron_IfaceImpl *impl = &entry->impls[j];
            impl->is_alive = type_is_instantiated(program, impl->type_name);
            if (impl->is_alive) alive++;
        }
        entry->alive_count = alive;
    }
}

int iron_iface_alive_count(Iron_IfaceEntry *entry) {
    return entry->alive_count;
}

int iron_iface_count(Iron_IfaceRegistry *reg) {
    return (int)shlen(reg->map);
}
