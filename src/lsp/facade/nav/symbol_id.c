/* Phase 3 Plan 01 Task 04 (NAV-16) -- symbol identity triple.
 *
 * Pinned FNV-1a 64-bit constants (D-02 lock):
 *   offset basis = 0xcbf29ce484222325
 *   prime        = 0x100000001b3
 *
 * The triple is (canonical_path, name_path, kind); the 64-bit hash is
 * precomputed at derive time over the concatenation of the three
 * components separated by NUL bytes. Consumers use hash equality as a
 * fast-reject before the strict string compare. */

#include "lsp/facade/nav/symbol_id.h"

#include "analyzer/scope.h"
#include "parser/ast.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ── FNV-1a 64-bit (pinned) ──────────────────────────────────────────── */

#define FNV_OFFSET_BASIS UINT64_C(0xcbf29ce484222325)
#define FNV_PRIME        UINT64_C(0x100000001b3)

uint64_t ilsp_symbol_id_fnv1a64(const void *bytes, size_t len) {
    const uint8_t *p = (const uint8_t *)bytes;
    uint64_t h = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV_PRIME;
    }
    return h;
}

static uint64_t fnv1a_update(uint64_t h, const void *bytes, size_t len) {
    const uint8_t *p = (const uint8_t *)bytes;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* ── Module stem extraction ──────────────────────────────────────────── */

const char *ilsp_symbol_id_module_stem(const char *canonical_path,
                                         Iron_Arena *arena) {
    if (!canonical_path || !*canonical_path) return "";
    /* Strip known sentinel prefixes. */
    const char *p = canonical_path;
    if (strncmp(p, "stdlib://", 9) == 0) p += 9;
    else if (strncmp(p, "dep://", 6) == 0) p += 6;
    /* Walk from end; locate the last '/' (if any) and the trailing
     * ".iron" extension. */
    const char *end = p + strlen(p);
    /* Trim ".iron" suffix. */
    const char *iron_suffix = ".iron";
    size_t isl = strlen(iron_suffix);
    if ((size_t)(end - p) >= isl &&
        memcmp(end - isl, iron_suffix, isl) == 0) {
        end -= isl;
    }
    const char *slash = end;
    while (slash > p && slash[-1] != '/') slash--;
    size_t stem_len = (size_t)(end - slash);
    if (stem_len == 0) return "";
    if (!arena) return "";
    return iron_arena_strdup(arena, slash, stem_len);
}

/* ── Name-path assembly ──────────────────────────────────────────────── */

/* Join two or three dotted components into a fresh arena string.
 * Skips empty components so "" . "name" -> "name" (rather than ".name"). */
static const char *join_dotted(Iron_Arena *arena,
                                const char *a,
                                const char *b,
                                const char *c) {
    size_t la = a ? strlen(a) : 0;
    size_t lb = b ? strlen(b) : 0;
    size_t lc = c ? strlen(c) : 0;
    size_t total = la + lb + lc + 3;  /* dots + NUL slack */
    char *buf = (char *)iron_arena_alloc(arena, total, 1);
    if (!buf) return "";
    size_t o = 0;
    if (la > 0) { memcpy(buf + o, a, la); o += la; }
    if (lb > 0) {
        if (o > 0) buf[o++] = '.';
        memcpy(buf + o, b, lb); o += lb;
    }
    if (lc > 0) {
        if (o > 0) buf[o++] = '.';
        memcpy(buf + o, c, lc); o += lc;
    }
    buf[o] = '\0';
    return buf;
}

/* Walk program->decls looking for an object/interface/enum whose
 * methods[] or fields[] array contains the given decl_node. Returns
 * the owning decl's name, or NULL if not found. Used to build the
 * dotted name_path for methods/fields/enum-variants. */
static const char *find_owner_name(const Iron_Program *program,
                                    const Iron_Node    *decl_node) {
    if (!program || !decl_node) return NULL;
    for (int i = 0; i < program->decl_count; i++) {
        Iron_Node *d = program->decls[i];
        if (!d) continue;
        switch ((int)d->kind) {
            case IRON_NODE_OBJECT_DECL: {
                Iron_ObjectDecl *o = (Iron_ObjectDecl *)d;
                for (int j = 0; j < o->field_count; j++) {
                    if (o->fields[j] == decl_node) return o->name;
                }
                break;
            }
            case IRON_NODE_INTERFACE_DECL: {
                Iron_InterfaceDecl *ifc = (Iron_InterfaceDecl *)d;
                for (int j = 0; j < ifc->method_count; j++) {
                    if (ifc->method_sigs[j] == decl_node) return ifc->name;
                }
                break;
            }
            case IRON_NODE_ENUM_DECL: {
                Iron_EnumDecl *e = (Iron_EnumDecl *)d;
                for (int j = 0; j < e->variant_count; j++) {
                    if (e->variants[j] == decl_node) return e->name;
                }
                break;
            }
            default:
                break;
        }
    }
    return NULL;
}

/* Extract the simple name of a symbol's decl node. */
static const char *decl_node_name(const Iron_Node *decl) {
    if (!decl) return NULL;
    switch ((int)decl->kind) {
        case IRON_NODE_FUNC_DECL:      return ((const Iron_FuncDecl *)decl)->name;
        case IRON_NODE_METHOD_DECL:    return ((const Iron_MethodDecl *)decl)->method_name;
        case IRON_NODE_OBJECT_DECL:    return ((const Iron_ObjectDecl *)decl)->name;
        case IRON_NODE_INTERFACE_DECL: return ((const Iron_InterfaceDecl *)decl)->name;
        case IRON_NODE_ENUM_DECL:      return ((const Iron_EnumDecl *)decl)->name;
        case IRON_NODE_FIELD:          return ((const Iron_Field *)decl)->name;
        case IRON_NODE_ENUM_VARIANT:   return ((const Iron_EnumVariant *)decl)->name;
        case IRON_NODE_IMPORT_DECL:    return ((const Iron_ImportDecl *)decl)->path;
        default:                        return NULL;
    }
}

/* Derive the method's owning type name. For method decls with a
 * type_name field that's the direct answer; otherwise walk the program. */
static const char *method_owner(const Iron_Node *decl,
                                 const Iron_Program *program) {
    if (!decl) return NULL;
    if (decl->kind == IRON_NODE_METHOD_DECL) {
        const Iron_MethodDecl *m = (const Iron_MethodDecl *)decl;
        if (m->type_name) return m->type_name;
    }
    return find_owner_name(program, decl);
}

/* ── Public API ──────────────────────────────────────────────────────── */

IronLsp_SymbolId ilsp_symbol_id_derive(const Iron_Symbol         *sym,
                                        const char                *canonical_path,
                                        const Iron_Program        *program,
                                        Iron_Arena                *arena) {
    IronLsp_SymbolId id = { .canonical_path = "", .name_path = "",
                             .kind = IRON_SYM_VARIABLE, .hash = 0 };
    if (!sym || !arena) return id;

    const char *mod = ilsp_symbol_id_module_stem(canonical_path, arena);
    const char *owner = method_owner(sym->decl_node, program);
    const char *base = sym->name ? sym->name : decl_node_name(sym->decl_node);
    if (!base) base = "";

    const char *name_path = join_dotted(arena, mod, owner, base);

    /* Intern the canonical_path so the triple's strings live as long as
     * the arena (not the caller's stack). */
    const char *cp_interned = canonical_path
        ? iron_arena_strdup(arena, canonical_path, strlen(canonical_path))
        : "";
    if (!cp_interned) cp_interned = "";

    id.canonical_path = cp_interned;
    id.name_path      = name_path;
    id.kind           = sym->sym_kind;

    /* FNV-1a over canonical_path + \0 + name_path + \0 + kind-byte. */
    uint64_t h = FNV_OFFSET_BASIS;
    h = fnv1a_update(h, cp_interned, strlen(cp_interned));
    uint8_t sep = 0;
    h = fnv1a_update(h, &sep, 1);
    h = fnv1a_update(h, name_path, strlen(name_path));
    h = fnv1a_update(h, &sep, 1);
    uint8_t kind_byte = (uint8_t)id.kind;
    h = fnv1a_update(h, &kind_byte, 1);
    id.hash = h;

    return id;
}

bool ilsp_symbol_id_equal(IronLsp_SymbolId a, IronLsp_SymbolId b) {
    if (a.hash != b.hash) return false;  /* fast reject */
    if (a.kind != b.kind) return false;
    if (!a.canonical_path || !b.canonical_path) {
        if (a.canonical_path != b.canonical_path) return false;
    } else if (strcmp(a.canonical_path, b.canonical_path) != 0) {
        return false;
    }
    if (!a.name_path || !b.name_path) {
        if (a.name_path != b.name_path) return false;
    } else if (strcmp(a.name_path, b.name_path) != 0) {
        return false;
    }
    return true;
}

uint64_t ilsp_symbol_id_hash(IronLsp_SymbolId id) {
    return id.hash;
}
