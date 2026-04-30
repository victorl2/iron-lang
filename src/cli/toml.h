#ifndef IRON_CLI_TOML_H
#define IRON_CLI_TOML_H

#include <stdbool.h>
#include "cli/web_config.h"

/* Parsed representation of a single dependency from [dependencies]. */
typedef struct {
    char *name;       /* key name in [dependencies] */
    char *git;        /* git = "owner/repo" */
    char *version;    /* version = "X.Y.Z" */
    /* Phase 94 LIB-03: local-path dependency. Set when [dependencies]
     * inline-table contains `path = "..."`. NULL for git-source deps.
     * Resolver dispatches on this field: non-NULL -> path-source handler;
     * NULL -> existing git-source handler. The string is heap-allocated
     * (strdup'd by extract_inline_field) and freed in iron_toml_free. */
    char *path;
    /* Filled in by resolver: */
    char *sha;        /* 40-char commit SHA (from iron.lock or GitHub API) */
    char *cache_path; /* absolute path: ~/.iron/cache/{owner}/{repo}@{sha}/
                       * Repurposed for path-deps: holds the absolute lib project dir. */
} IronDep;

/* Parsed representation of an iron.toml project file. */
typedef struct {
    /* [package] fields (was [project]) */
    char *name;        /* name = "..." (required) */
    char *version;     /* version = "..." (required) */
    char *entry;       /* entry = "..." (preserved for backward compat, ignored by iron) */
    char *type;        /* type = "bin" or "lib" (default "bin") */
    char *description; /* description = "..." (optional) */

    /* [dependencies] */
    bool   raylib;       /* raylib = true (backward compat) */
    IronDep *deps;       /* heap array of parsed inline-table deps */
    int    dep_count;
    int    dep_capacity;
    /* [web] section (parsed in toml.c section==3 branch, Plan 02) */
    IronWebConfig web;

    /* Directory containing the iron.toml file passed to iron_toml_parse.
     * Populated by iron_toml_parse. Never NULL on a successful parse —
     * falls back to "." when path has no directory component (bare filename).
     * Freed by iron_toml_free.
     * Used by web builds to resolve [web].assets paths relative to iron.toml's
     * location rather than the shell's cwd (WEB-ASSET-04). */
    char *toml_dir;
} IronProject;

/* Parse iron.toml at the given path.
 * Returns a heap-allocated IronProject on success, NULL on error.
 * Caller must free with iron_toml_free(). */
IronProject *iron_toml_parse(const char *path);

/* Free an IronProject returned by iron_toml_parse(). */
void iron_toml_free(IronProject *proj);

#endif /* IRON_CLI_TOML_H */
