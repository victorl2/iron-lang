/* Phase 3 Plan 02 Task 03 (NAV-01) -- iron.toml dep symbol map.
 *
 * Wraps src/pkg/resolver.c as a library-linked call (NO subprocess).
 * Applies a path-escape defense on each returned dep before accepting
 * it into the map.
 *
 * Invalidation is per-dep (ilsp_dep_map_invalidate(dm, "name")) or
 * wholesale (NULL).  Each entry owns its own Iron_Arena that is freed
 * on drop.
 *
 * A test seam (ilsp_dep_map_inject_for_test) lets unit tests exercise
 * lookup / invalidation semantics without triggering the real
 * resolver (which hits GitHub for dep SHA resolution).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "lsp/store/dep_map.h"

#include "cli/toml.h"
#include "diagnostics/diagnostics.h"
#include "lexer/lexer.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "pkg/resolver.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <dirent.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Subprocess guard ──────────────────────────────────────────────────
 * The resolver's fetcher.c path calls `spawn_and_wait` to shell out to
 * curl / tar for fetching deps that are not yet in ~/.iron/cache.  In
 * the LSP context we want STRICT no-subprocess behaviour: fetching is
 * a build-time concern owned by `iron build`.  We provide a stub that
 * always fails (prints a warning, returns 1) so the resolver degrades
 * gracefully -- if a dep is not already cached, it's simply not added
 * to the dep map and the LSP works on resolvable deps only.
 *
 * This keeps T-03-05 + the plan's "NO subprocess invocation" guarantee
 * intact while still library-linking the resolver.
 */
int spawn_and_wait(const char *prog, char *const argv[]);
int spawn_and_wait(const char *prog, char *const argv[]) {
    (void)argv;
    fprintf(stderr,
            "ironls: refusing to spawn '%s' from LSP context (run 'iron build' "
            "to fetch deps).\n",
            prog ? prog : "<null>");
    return 1;
}

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct IronLsp_DepMap {
    char                                                  *workspace_root;   /* realpath-normalised */
    struct { char *key; IronLsp_DepEntry *value; }        *entries;          /* stb_ds shmap */
};

/* ── Helpers ─────────────────────────────────────────────────────────── */

static char *canonicalize(const char *path) {
    if (!path) return NULL;
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) return strdup(resolved);
    return strdup(path);
}

static bool has_prefix(const char *s, const char *p) {
    if (!s || !p) return false;
    size_t sl = strlen(s), pl = strlen(p);
    if (pl > sl) return false;
    return memcmp(s, p, pl) == 0;
}

/* Compute the iron cache path (~/.iron/cache).  Returns malloc'd or NULL. */
static char *iron_cache_path(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    size_t len = strlen(home) + strlen("/.iron/cache") + 1;
    char *p = (char *)malloc(len);
    if (!p) return NULL;
    snprintf(p, len, "%s/.iron/cache", home);
    return p;
}

/* T-03-05: validate that dep_root sits under workspace_root or iron cache. */
static bool path_is_safe(const char *workspace_root, const char *canon_dep) {
    if (!canon_dep) return false;
    if (workspace_root && has_prefix(canon_dep, workspace_root)) return true;
    char *cache = iron_cache_path();
    if (cache) {
        char *canon_cache = canonicalize(cache);
        free(cache);
        if (canon_cache) {
            bool ok = has_prefix(canon_dep, canon_cache);
            free(canon_cache);
            if (ok) return true;
        }
    }
    /* Accept absolute /tmp paths only when they share the workspace_root's
     * parent -- covers mktemp fixtures.  Conservative default: reject. */
    return false;
}

static void free_entry(IronLsp_DepEntry *e) {
    if (!e) return;
    if (e->exported_symbols) arrfree(e->exported_symbols);
    if (e->arena) {
        iron_arena_free(e->arena);
        free(e->arena);
    }
    if (e->canonical_path) free(e->canonical_path);
    if (e->dep_name) free(e->dep_name);
    free(e);
}

/* Walk dep_root for .iron files and harvest top-level decl names into
 * `out_symbols` (stb_ds array, arena-interned strings). */
static void harvest_exported_symbols(const char *dep_root,
                                      Iron_Arena *arena,
                                      const char ***out_symbols) {
    DIR *dp = opendir(dep_root);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        const char *name = de->d_name;
        size_t nl = strlen(name);
        if (nl < 6) continue;
        if (memcmp(name + nl - 5, ".iron", 5) != 0) continue;

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dep_root, name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        FILE *fp = fopen(full, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0) { fclose(fp); continue; }
        char *src = (char *)malloc((size_t)sz + 1);
        if (!src) { fclose(fp); continue; }
        size_t n = fread(src, 1, (size_t)sz, fp);
        fclose(fp);
        src[n] = '\0';

        Iron_DiagList diags = iron_diaglist_create();
        Iron_Lexer lex = iron_lexer_create(src, full, arena, &diags);
        Iron_Token *tokens = iron_lex_all(&lex);
        if (tokens) {
            int tc = (int)arrlen(tokens);
            Iron_Parser p = iron_parser_create(tokens, tc, src, full,
                                                arena, &diags);
            iron_parser_set_mode(&p, IRON_ANALYSIS_MODE_LSP);
            Iron_Node *root = iron_parse(&p);
            arrfree(tokens);
            if (root && root->kind == IRON_NODE_PROGRAM) {
                Iron_Program *prog = (Iron_Program *)root;
                for (int i = 0; i < prog->decl_count; i++) {
                    Iron_Node *d = prog->decls[i];
                    if (!d) continue;
                    const char *nm = NULL;
                    switch ((int)d->kind) {
                        case IRON_NODE_OBJECT_DECL:
                            nm = ((Iron_ObjectDecl *)d)->name; break;
                        case IRON_NODE_INTERFACE_DECL:
                            nm = ((Iron_InterfaceDecl *)d)->name; break;
                        case IRON_NODE_FUNC_DECL:
                            nm = ((Iron_FuncDecl *)d)->name; break;
                        case IRON_NODE_ENUM_DECL:
                            nm = ((Iron_EnumDecl *)d)->name; break;
                        default: break;
                    }
                    if (nm) {
                        const char *interned = iron_arena_strdup(arena, nm, strlen(nm));
                        arrput(*out_symbols, interned);
                    }
                }
            }
        }
        iron_diaglist_free(&diags);
        free(src);
    }
    closedir(dp);
}

/* Add an entry to the map after it has passed the path-escape check.
 * Takes ownership of everything passed in (canon copy is strdup'd). */
static void insert_entry(IronLsp_DepMap *dm, const char *dep_name,
                          const char *canon_path) {
    IronLsp_DepEntry *e = (IronLsp_DepEntry *)calloc(1, sizeof(*e));
    if (!e) return;
    e->dep_name        = strdup(dep_name);
    e->canonical_path  = strdup(canon_path);
    e->arena           = (Iron_Arena *)malloc(sizeof(Iron_Arena));
    if (!e->arena) { free_entry(e); return; }
    *e->arena          = iron_arena_create(64 * 1024);
    e->exported_symbols = NULL;
    harvest_exported_symbols(canon_path, e->arena, &e->exported_symbols);
    shput(dm->entries, dep_name, e);
}

/* ── Public API ─────────────────────────────────────────────────────── */

IronLsp_DepMap *ilsp_dep_map_create(const char *workspace_root) {
    IronLsp_DepMap *dm = (IronLsp_DepMap *)calloc(1, sizeof(*dm));
    if (!dm) return NULL;
    dm->workspace_root = workspace_root ? canonicalize(workspace_root) : NULL;
    dm->entries = NULL;
    sh_new_strdup(dm->entries);
    return dm;
}

void ilsp_dep_map_destroy(IronLsp_DepMap *dm) {
    if (!dm) return;
    if (dm->entries) {
        for (ptrdiff_t i = 0; i < shlen(dm->entries); i++) {
            free_entry(dm->entries[i].value);
        }
        shfree(dm->entries);
    }
    if (dm->workspace_root) free(dm->workspace_root);
    free(dm);
}

int ilsp_dep_map_resolve(IronLsp_DepMap *dm, _Atomic bool *cancel) {
    if (!dm || !dm->workspace_root) return -1;

    /* Load iron.toml from workspace root. If absent, succeed with 0 deps. */
    char toml_path[PATH_MAX];
    snprintf(toml_path, sizeof(toml_path), "%s/iron.toml", dm->workspace_root);
    struct stat st;
    if (stat(toml_path, &st) != 0) return 0;  /* no manifest; bare workspace ok */

    IronProject *proj = iron_toml_parse(toml_path);
    if (!proj) return -1;
    if (proj->dep_count == 0) {
        iron_toml_free(proj);
        return 0;
    }

    ResolvedDeps resolved = {0};
    int ret = resolve_dependencies(proj, dm->workspace_root,
                                    /* colors = */ false, &resolved);
    iron_toml_free(proj);
    if (ret != 0) {
        resolved_deps_free(&resolved);
        return ret;
    }

    for (int i = 0; i < resolved.count; i++) {
        if (cancel && atomic_load(cancel)) break;
        IronDep *d = &resolved.deps[i];
        if (!d->name || !d->cache_path) continue;
        char *canon = canonicalize(d->cache_path);
        if (!canon) continue;
        if (!path_is_safe(dm->workspace_root, canon)) {
            fprintf(stderr,
                    "ironls: dep '%s' path '%s' failed safety check; skipping\n",
                    d->name, canon);
            free(canon);
            continue;
        }
        insert_entry(dm, d->name, canon);
        free(canon);
    }

    resolved_deps_free(&resolved);
    return 0;
}

IronLsp_DepEntry *ilsp_dep_map_lookup(IronLsp_DepMap *dm,
                                       const char *dep_name) {
    if (!dm || !dep_name || !dm->entries) return NULL;
    ptrdiff_t idx = shgeti(dm->entries, dep_name);
    if (idx < 0) return NULL;
    return dm->entries[idx].value;
}

void ilsp_dep_map_invalidate(IronLsp_DepMap *dm, const char *dep_name) {
    if (!dm || !dm->entries) return;
    if (dep_name) {
        ptrdiff_t idx = shgeti(dm->entries, dep_name);
        if (idx >= 0) {
            free_entry(dm->entries[idx].value);
            shdel(dm->entries, dep_name);
        }
        return;
    }
    /* Drop-all. */
    for (ptrdiff_t i = 0; i < shlen(dm->entries); i++) {
        free_entry(dm->entries[i].value);
    }
    shfree(dm->entries);
    dm->entries = NULL;
    sh_new_strdup(dm->entries);
}

size_t ilsp_dep_map_size(const IronLsp_DepMap *dm) {
    if (!dm || !dm->entries) return 0;
    return (size_t)shlen(dm->entries);
}

bool ilsp_dep_map_inject_for_test(IronLsp_DepMap *dm,
                                    const char *dep_name,
                                    const char *dep_root,
                                    const char **exported_symbols_arr) {
    if (!dm || !dep_name || !dep_root) return false;
    char *canon = canonicalize(dep_root);
    if (!canon) return false;
    if (!path_is_safe(dm->workspace_root, canon)) {
        free(canon);
        if (exported_symbols_arr) arrfree(exported_symbols_arr);
        return false;
    }
    IronLsp_DepEntry *e = (IronLsp_DepEntry *)calloc(1, sizeof(*e));
    if (!e) { free(canon); return false; }
    e->dep_name         = strdup(dep_name);
    e->canonical_path   = canon;
    e->arena            = (Iron_Arena *)malloc(sizeof(Iron_Arena));
    if (!e->arena)      { free_entry(e); return false; }
    *e->arena           = iron_arena_create(16 * 1024);
    e->exported_symbols = exported_symbols_arr;   /* takes ownership */
    shput(dm->entries, dep_name, e);
    return true;
}
