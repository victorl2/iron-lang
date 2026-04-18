/* Phase 3 Plan 02 Task 01 (NAV-01, D-01) -- workspace index core.
 *
 * Per-file cache keyed on canonical_path.  Extends HARD-06 per-request
 * arena to a per-file entry arena: invalidate = iron_arena_free + shdel.
 *
 * The ONLY iron_analyze_buffer call site reachable from this TU is via
 * ilsp_facade_compile_pure (single-call-site invariant).  Warm-seed uses
 * parse-only primitives (iron_lexer_create/iron_lex_all/iron_parse) --
 * those are public compiler APIs, NOT iron_analyze_buffer, so the
 * invariant holds.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "lsp/store/workspace_index.h"

#include "analyzer/analyzer.h"
#include "diagnostics/diagnostics.h"
#include "lexer/lexer.h"
#include "lsp/facade/compile.h"
#include "lsp/store/document.h"
#include "lsp/store/line_index.h"
#include "lsp/store/sha256.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "runtime/iron_runtime.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ── Private helpers ─────────────────────────────────────────────────── */

static uint64_t hash_prefix_from_bytes(const char *data, size_t len) {
    uint8_t digest[32];
    ilsp_sha256((const uint8_t *)data, len, digest);
    uint64_t h = 0;
    for (int i = 0; i < 8; i++) {
        h = (h << 8) | (uint64_t)digest[i];
    }
    return h;
}

/* Read a file into a freshly malloc'd buffer. Returns NULL on error.
 * On success *out_len receives the byte count (no NUL terminator
 * counted but the buffer has one appended for safety). */
static char *slurp_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (n != (size_t)sz) { free(buf); return NULL; }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

/* realpath wrapper: returns malloc'd canonical path or strdup(path) if
 * realpath fails (the file might not exist yet). */
static char *canonicalize(const char *path) {
    if (!path) return NULL;
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) {
        return strdup(resolved);
    }
    return strdup(path);
}

static int str_ends_with(const char *s, const char *suf) {
    size_t sl = strlen(s);
    size_t fl = strlen(suf);
    if (fl > sl) return 0;
    return memcmp(s + sl - fl, suf, fl) == 0;
}

/* Exclude directories we never want to walk into. */
static int is_excluded_dir(const char *name) {
    if (!name || name[0] == '.') return 1;            /* .git, .hidden, etc */
    if (strcmp(name, "build")        == 0) return 1;
    if (strcmp(name, "node_modules") == 0) return 1;
    if (strcmp(name, "target")       == 0) return 1;
    return 0;
}

/* Parse-only pipeline. Returns NULL on OOM or missing source; otherwise
 * returns the root Iron_Program (allocated in `arena`). `diags` is
 * caller-owned. */
static Iron_Program *parse_only(const char *source, size_t len,
                                 const char *filename,
                                 Iron_Arena *arena,
                                 Iron_DiagList *diags,
                                 const _Atomic bool *cancel) {
    if (!source || !arena || !diags) return NULL;
    Iron_Lexer lex = iron_lexer_create(source, filename ? filename : "<index>",
                                        arena, diags);
    iron_lexer_set_cancel_flag(&lex, cancel);
    (void)len;  /* lexer reads until NUL; slurp_file appends one */
    Iron_Token *tokens = iron_lex_all(&lex);
    if (!tokens) return NULL;
    int tok_count = (int)arrlen(tokens);
    Iron_Parser p = iron_parser_create(tokens, tok_count, source,
                                        filename ? filename : "<index>",
                                        arena, diags);
    iron_parser_set_mode(&p, IRON_ANALYSIS_MODE_LSP);
    iron_parser_set_cancel_flag(&p, cancel);
    Iron_Node *root = iron_parse(&p);
    arrfree(tokens);
    if (!root || root->kind != IRON_NODE_PROGRAM) return NULL;
    return (Iron_Program *)root;
}

/* Free one entry's heap resources. Removes it from the map under the
 * assumption the caller already holds wi->lock. The `canonical_path`
 * pointer is freed by stb_ds via shdel's key-duplication discipline. */
static void free_entry(IronLsp_IndexEntry *e) {
    if (!e) return;
    if (e->arena) {
        iron_arena_free(e->arena);
        free(e->arena);
    }
    ilsp_line_index_destroy(&e->line_idx);
    if (e->canonical_path) free(e->canonical_path);
    if (e->source_bytes)   free(e->source_bytes);
    free(e);
}

/* Walk wi->entries looking for the lowest last_used_tick among analyzed
 * entries. Returns the key (path) of the LRU or NULL when none. Caller
 * must hold wi->lock. */
static const char *find_lru_analyzed_path(IronLsp_WorkspaceIndex *wi) {
    const char *lru_key = NULL;
    int64_t     lru_tick = INT64_MAX;
    for (ptrdiff_t i = 0; i < shlen(wi->entries); i++) {
        IronLsp_IndexEntry *e = wi->entries[i].value;
        if (!e || !e->analyzed) continue;
        if (e->last_used_tick < lru_tick) {
            lru_tick = e->last_used_tick;
            lru_key  = wi->entries[i].key;
        }
    }
    return lru_key;
}

/* Detach an entry from the map and free it. Caller holds wi->lock. */
static void invalidate_path_locked(IronLsp_WorkspaceIndex *wi,
                                    const char *canonical_path) {
    if (!wi || !wi->entries || !canonical_path) return;
    ptrdiff_t idx = shgeti(wi->entries, canonical_path);
    if (idx < 0) return;
    IronLsp_IndexEntry *e = wi->entries[idx].value;
    bool was_analyzed = e ? e->analyzed : false;
    free_entry(e);
    shdel(wi->entries, canonical_path);
    if (was_analyzed && wi->analyzed_count > 0) wi->analyzed_count--;
}

/* Check whether a parsed program imports a given module name.  NULL
 * name means "any import at all". */
static bool program_imports(const Iron_Program *p, const char *dep_name) {
    if (!p) return false;
    for (int i = 0; i < p->decl_count; i++) {
        Iron_Node *n = p->decls[i];
        if (!n || n->kind != IRON_NODE_IMPORT_DECL) continue;
        if (!dep_name) return true;
        Iron_ImportDecl *imp = (Iron_ImportDecl *)n;
        const char *path = imp->path;
        if (!path) continue;
        /* Accept exact match or last-path-component match so "foo.bar"
         * and "bar" both hit when the user invalidates "bar". */
        if (strcmp(path, dep_name) == 0) return true;
        const char *dot = strrchr(path, '.');
        if (dot && strcmp(dot + 1, dep_name) == 0) return true;
    }
    return false;
}

/* ── Warm-seed walker ───────────────────────────────────────────────── */

typedef struct {
    IronLsp_WorkspaceIndex *wi;
    _Atomic bool           *cancel;
    int                     files_seen;     /* for T-03-04 cap */
    bool                    cap_hit;
} WalkCtx;

/* Insert or replace an entry for `full_path` with freshly-parsed
 * contents. Takes wi->lock internally. */
static void add_or_replace_entry(IronLsp_WorkspaceIndex *wi,
                                  const char *full_path) {
    /* 1. realpath */
    char *canon = canonicalize(full_path);
    if (!canon) return;

    /* 2. slurp source */
    size_t src_len = 0;
    char *src = slurp_file(canon, &src_len);
    if (!src) { free(canon); return; }

    /* 3. Allocate heap arena (entry owns it). */
    Iron_Arena *arena = (Iron_Arena *)malloc(sizeof(Iron_Arena));
    if (!arena) { free(src); free(canon); return; }
    *arena = iron_arena_create(64 * 1024);

    /* 4. Throw-away diaglist for parse-only; we don't emit to the client
     *    at warm-seed time (D-01 leaves analyze for on-demand). */
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Program *prog  = parse_only(src, src_len, canon, arena, &diags, NULL);
    iron_diaglist_free(&diags);
    if (!prog) {
        iron_arena_free(arena);
        free(arena);
        free(src);
        free(canon);
        return;
    }

    /* 5. LineIndex for span translation. */
    IronLsp_IndexEntry *e = (IronLsp_IndexEntry *)calloc(1, sizeof(*e));
    if (!e) {
        iron_arena_free(arena);
        free(arena);
        free(src);
        free(canon);
        return;
    }
    ilsp_line_index_init(&e->line_idx);
    ilsp_line_index_rebuild(&e->line_idx, src, src_len);

    e->canonical_path   = canon;
    e->source_bytes     = src;
    e->source_len       = src_len;
    e->arena            = arena;
    e->program          = prog;
    e->content_hash     = hash_prefix_from_bytes(src, src_len);
    e->analyzed         = false;
    e->last_used_tick   = atomic_fetch_add(&wi->tick, 1);

    /* 6. Insert under mutex; replace if already present. */
    IRON_MUTEX_LOCK(wi->lock);
    invalidate_path_locked(wi, e->canonical_path);
    shput(wi->entries, e->canonical_path, e);
    IRON_MUTEX_UNLOCK(wi->lock);
}

/* Recursive readdir walker. Follows directories (skipping excluded
 * names) and registers `*.iron` files into the index. */
static void walk_dir(WalkCtx *ctx, const char *dir_path) {
    if (!ctx || ctx->cap_hit) return;
    if (ctx->cancel && atomic_load(ctx->cancel)) return;

    DIR *dp = opendir(dir_path);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (ctx->cancel && atomic_load(ctx->cancel)) break;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", dir_path, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(child)) continue;

        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (is_excluded_dir(de->d_name)) continue;
            walk_dir(ctx, child);
        } else if (S_ISREG(st.st_mode)) {
            if (!str_ends_with(de->d_name, ".iron")) continue;
            if (ctx->files_seen >= ILSP_WORKSPACE_INDEX_WARMSEED_CAP) {
                ctx->cap_hit = true;
                break;
            }
            add_or_replace_entry(ctx->wi, child);
            ctx->files_seen++;
        }
    }
    closedir(dp);
}

/* ── Public API ─────────────────────────────────────────────────────── */

IronLsp_WorkspaceIndex *ilsp_workspace_index_create(const char *workspace_root) {
    IronLsp_WorkspaceIndex *wi =
        (IronLsp_WorkspaceIndex *)calloc(1, sizeof(*wi));
    if (!wi) return NULL;
    wi->workspace_root = workspace_root ? canonicalize(workspace_root) : NULL;
    wi->entries        = NULL;
    sh_new_strdup(wi->entries);
    IRON_MUTEX_INIT(wi->lock);
    atomic_store(&wi->tick, 0);
    wi->analyzed_count = 0;
    wi->stdlib = NULL;
    wi->deps   = NULL;
    return wi;
}

void ilsp_workspace_index_destroy(IronLsp_WorkspaceIndex *wi) {
    if (!wi) return;
    IRON_MUTEX_LOCK(wi->lock);
    if (wi->entries) {
        for (ptrdiff_t i = 0; i < shlen(wi->entries); i++) {
            free_entry(wi->entries[i].value);
        }
        shfree(wi->entries);
    }
    IRON_MUTEX_UNLOCK(wi->lock);
    IRON_MUTEX_DESTROY(wi->lock);
    if (wi->workspace_root) free(wi->workspace_root);
    free(wi);
}

void ilsp_workspace_index_warm_seed(IronLsp_WorkspaceIndex *wi,
                                     _Atomic bool          *cancel) {
    if (!wi || !wi->workspace_root) return;
    WalkCtx ctx = { .wi = wi, .cancel = cancel, .files_seen = 0, .cap_hit = false };
    walk_dir(&ctx, wi->workspace_root);
}

IronLsp_IndexEntry *ilsp_workspace_index_lookup(IronLsp_WorkspaceIndex *wi,
                                                 const char *canonical_path) {
    if (!wi || !canonical_path || !wi->entries) return NULL;
    IRON_MUTEX_LOCK(wi->lock);
    ptrdiff_t idx = shgeti(wi->entries, canonical_path);
    IronLsp_IndexEntry *e = (idx >= 0) ? wi->entries[idx].value : NULL;
    if (e) {
        e->last_used_tick = atomic_fetch_add(&wi->tick, 1);
    }
    IRON_MUTEX_UNLOCK(wi->lock);
    return e;
}

void ilsp_workspace_index_invalidate_path(IronLsp_WorkspaceIndex *wi,
                                           const char *canonical_path) {
    if (!wi || !canonical_path) return;
    IRON_MUTEX_LOCK(wi->lock);
    invalidate_path_locked(wi, canonical_path);
    IRON_MUTEX_UNLOCK(wi->lock);
}

void ilsp_workspace_index_invalidate_dep(IronLsp_WorkspaceIndex *wi,
                                          const char *dep_name) {
    if (!wi || !wi->entries) return;
    /* Collect victim paths first so we can mutate the map safely. */
    char **victims = NULL;
    IRON_MUTEX_LOCK(wi->lock);
    for (ptrdiff_t i = 0; i < shlen(wi->entries); i++) {
        IronLsp_IndexEntry *e = wi->entries[i].value;
        if (!e) continue;
        if (!dep_name || program_imports(e->program, dep_name)) {
            /* strdup the key since invalidate frees the original slot. */
            char *dup = strdup(wi->entries[i].key);
            if (dup) arrput(victims, dup);
        }
    }
    for (ptrdiff_t i = 0; i < arrlen(victims); i++) {
        invalidate_path_locked(wi, victims[i]);
    }
    IRON_MUTEX_UNLOCK(wi->lock);
    if (victims) {
        for (ptrdiff_t i = 0; i < arrlen(victims); i++) free(victims[i]);
        arrfree(victims);
    }
}

Iron_Program *ilsp_workspace_index_analyze_lazy(IronLsp_WorkspaceIndex *wi,
                                                 IronLsp_IndexEntry *entry,
                                                 _Atomic bool *cancel) {
    if (!wi || !entry) return NULL;
    if (entry->analyzed) return entry->program;

    /* Build a minimal IronLsp_Document-shaped input for the facade seam.
     * ilsp_facade_compile_pure reads doc->text / doc->text_len /
     * doc->uri only -- we synthesise a stack document. */
    IronLsp_Document doc = {0};
    doc.text     = entry->source_bytes;
    doc.text_len = entry->source_len;
    doc.uri      = entry->canonical_path;

    IronLsp_CompileRequest req = { .version = 0, .cancel_flag = cancel };

    Iron_Arena analyze_arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags      = iron_diaglist_create();
    ilsp_facade_compile_pure(&doc, &req, &analyze_arena, &diags);
    /* Facade runs iron_analyze_buffer on its own arena; we capture nothing
     * from its result here -- the sealed Iron_Program pointer we keep is
     * the parse-only one from warm-seed, which is all nav-endpoints need
     * for span lookup.  Resolved-symbol walks (Plan 03+) will read the
     * analyzer annotations on the fresh program. */
    iron_diaglist_free(&diags);
    iron_arena_free(&analyze_arena);

    IRON_MUTEX_LOCK(wi->lock);
    if (!entry->analyzed) {
        entry->analyzed = true;
        wi->analyzed_count++;
        /* LRU eviction: if we've overshot the cap, evict the oldest
         * analyzed entry other than the one we just analyzed. */
        while (wi->analyzed_count > ILSP_WORKSPACE_INDEX_ANALYZE_CAP) {
            const char *victim = find_lru_analyzed_path(wi);
            if (!victim || victim == entry->canonical_path) break;
            /* Collapse the "analyzed" bit on the victim without
             * evicting its parsed form so subsequent lookups still
             * get a valid program (and re-analyze on demand). */
            ptrdiff_t vi = shgeti(wi->entries, victim);
            if (vi < 0) break;
            IronLsp_IndexEntry *ve = wi->entries[vi].value;
            if (ve && ve->analyzed) {
                ve->analyzed = false;
                wi->analyzed_count--;
            } else {
                break;
            }
        }
    }
    entry->last_used_tick = atomic_fetch_add(&wi->tick, 1);
    IRON_MUTEX_UNLOCK(wi->lock);
    return entry->program;
}

int ilsp_workspace_index_analyzed_count(const IronLsp_WorkspaceIndex *wi) {
    return wi ? wi->analyzed_count : 0;
}

size_t ilsp_workspace_index_entry_count(const IronLsp_WorkspaceIndex *wi) {
    if (!wi || !wi->entries) return 0;
    return (size_t)shlen(wi->entries);
}
