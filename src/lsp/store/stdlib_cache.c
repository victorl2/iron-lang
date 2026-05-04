/* Phase 3 Plan 02 Task 02 (NAV-01, D-14) -- stdlib pre-parse cache.
 *
 * Process-lifetime, immutable, pthread_once-guarded cache of every
 * `.iron` surface file shipped in src/stdlib/ + src/runtime/.
 *
 * The cache intentionally leaks at process exit: it is the documented
 * "static-mutable-state exception" (obs/log.c precedent, Plan 06).
 * ilsp_stdlib_cache_destroy exists only so tests can rebuild a fresh
 * cache from a different iron_source_dir.
 *
 * Detection logic is library-linked (iron_detect_import lives in
 * iron_compiler as a static library TU).  This file does NOT invoke
 * any subprocess.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "lsp/store/stdlib_cache.h"

#include "cli/iron_import_detect.h"   /* iron_detect_import (library-linked) */
#include "diagnostics/diagnostics.h"
#include "lexer/lexer.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct IronLsp_StdlibCache {
    Iron_Arena arena;                                  /* owns every parsed module */
    struct { char *key; const Iron_Program *value; } *modules;  /* stb_ds shmap */
};

/* ── pthread_once singleton ──────────────────────────────────────────── */

static pthread_once_t       g_stdlib_once     = PTHREAD_ONCE_INIT;
static IronLsp_StdlibCache *g_stdlib_instance = NULL;
static const char          *g_init_source_dir = NULL; /* set before pthread_once */

/* Read a file into a malloc'd NUL-terminated buffer. */
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

/* Extract the stem ("math") from a filename ("math.iron"). Returns
 * arena-interned string. */
static const char *stem_of(const char *filename, Iron_Arena *arena) {
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    const char *dot  = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    return iron_arena_strdup(arena, base, len);
}

/* Parse one source buffer into an arena-allocated Iron_Program.  On
 * parse failure returns NULL; never aborts. */
static Iron_Program *parse_one(const char *source, const char *filename,
                                Iron_Arena *arena) {
    Iron_DiagList diags = iron_diaglist_create();
    Iron_Lexer lex = iron_lexer_create(source, filename, arena, &diags);
    Iron_Token *tokens = iron_lex_all(&lex);
    Iron_Program *prog = NULL;
    if (tokens) {
        int n = (int)arrlen(tokens);
        Iron_Parser p = iron_parser_create(tokens, n, source, filename,
                                            arena, &diags);
        iron_parser_set_mode(&p, IRON_ANALYSIS_MODE_LSP);
        Iron_Node *root = iron_parse(&p);
        arrfree(tokens);
        if (root && root->kind == IRON_NODE_PROGRAM) {
            prog = (Iron_Program *)root;
        }
    }
    iron_diaglist_free(&diags);
    return prog;
}

/* Populate the cache by walking `<source_dir>/<subdir>` for .iron files. */
static void load_subdir(IronLsp_StdlibCache *c, const char *source_dir,
                         const char *subdir) {
    char dirpath[PATH_MAX];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", source_dir, subdir);
    DIR *dp = opendir(dirpath);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        const char *name = de->d_name;
        size_t nl = strlen(name);
        if (nl < 6) continue;
        if (memcmp(name + nl - 5, ".iron", 5) != 0) continue;

        char fullpath[PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, name);
        struct stat st;
        if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        size_t src_len = 0;
        char *src = slurp_file(fullpath, &src_len);
        if (!src) continue;

        Iron_Program *prog = parse_one(src, fullpath, &c->arena);
        if (!prog) {
            free(src);
            continue;
        }
        /* Intern the source text into the arena so the Iron_Program's
         * Iron_Token value pointers remain valid. parse_one already
         * used arena-interning for token values, so the raw source can
         * be freed. */
        free(src);

        const char *stem = stem_of(name, &c->arena);
        shput(c->modules, stem, prog);
    }
    closedir(dp);
}

/* Resolve the iron source directory. */
static const char *resolve_source_dir(const char *explicit_dir) {
    if (explicit_dir && *explicit_dir) return explicit_dir;
    const char *env = getenv("IRON_SOURCE_DIR");
    if (env && *env) return env;
#ifdef IRON_SOURCE_DIR
    return IRON_SOURCE_DIR;
#else
    return "src";
#endif
}

/* pthread_once init routine.  Reads g_init_source_dir, builds the
 * singleton, assigns g_stdlib_instance. */
static void stdlib_once_init(void) {
    IronLsp_StdlibCache *c =
        (IronLsp_StdlibCache *)calloc(1, sizeof(*c));
    if (!c) return;
    c->arena   = iron_arena_create(2 * 1024 * 1024);
    c->modules = NULL;
    sh_new_arena(c->modules);

    const char *src_dir = resolve_source_dir(g_init_source_dir);
    load_subdir(c, src_dir, "stdlib");
    load_subdir(c, src_dir, "runtime");

    g_stdlib_instance = c;
}

/* ── Public API ─────────────────────────────────────────────────────── */

IronLsp_StdlibCache *ilsp_stdlib_cache_init(const char *iron_source_dir) {
    /* Publish the source dir before pthread_once runs (only the first
     * caller's dir actually influences the singleton). */
    if (iron_source_dir) g_init_source_dir = iron_source_dir;
    pthread_once(&g_stdlib_once, stdlib_once_init);
    return g_stdlib_instance;
}

const Iron_Program *ilsp_stdlib_cache_get(IronLsp_StdlibCache *cache,
                                           const char *module_name) {
    if (!cache || !module_name || !cache->modules) return NULL;
    ptrdiff_t idx = shgeti(cache->modules, module_name);
    if (idx < 0) return NULL;
    return cache->modules[idx].value;
}

size_t ilsp_stdlib_cache_size(IronLsp_StdlibCache *cache) {
    if (!cache || !cache->modules) return 0;
    return (size_t)shlen(cache->modules);
}

void ilsp_stdlib_cache_destroy(IronLsp_StdlibCache *cache) {
    if (!cache) return;
    if (cache->modules) shfree(cache->modules);
    iron_arena_free(&cache->arena);
    free(cache);
    /* Reset singleton state so tests can init again. */
    if (cache == g_stdlib_instance) {
        g_stdlib_instance = NULL;
        pthread_once_t zero = PTHREAD_ONCE_INIT;
        g_stdlib_once = zero;
        g_init_source_dir = NULL;
    }
}
