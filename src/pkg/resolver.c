/*
 * iron dependency resolver: DFS graph traversal with cycle detection,
 * diamond dedup, version conflict detection, and topological ordering.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "pkg/resolver.h"
#include "pkg/fetcher.h"
#include "pkg/lockfile.h"
#include "pkg/color.h"
#include "cli/toml.h"

/* ── String set for visited/path tracking ──────────────────────────────── */

typedef struct {
    char **items;
    int    count;
    int    capacity;
} StringSet;

static void string_set_init(StringSet *s) {
    s->capacity = 16;
    s->count = 0;
    s->items = (char **)calloc((size_t)s->capacity, sizeof(char *));
}

static int string_set_contains(StringSet *s, const char *key) {
    for (int i = 0; i < s->count; i++) {
        if (strcmp(s->items[i], key) == 0) return 1;
    }
    return 0;
}

static void string_set_add(StringSet *s, const char *key) {
    if (s->count >= s->capacity) {
        s->capacity *= 2;
        s->items = (char **)realloc(s->items,
                                     sizeof(char *) * (size_t)s->capacity);
    }
    s->items[s->count++] = strdup(key);
}

static void string_set_remove_last(StringSet *s) {
    if (s->count > 0) {
        s->count--;
        free(s->items[s->count]);
        s->items[s->count] = NULL;
    }
}

static void string_set_free(StringSet *s) {
    for (int i = 0; i < s->count; i++) {
        free(s->items[i]);
    }
    free(s->items);
    s->items = NULL;
    s->count = 0;
    s->capacity = 0;
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Return malloc'd lowercase copy of a string. */
static char *lowercase_git(const char *git) {
    size_t len = strlen(git);
    char *lower = (char *)malloc(len + 1);
    if (!lower) return NULL;
    for (size_t i = 0; i < len; i++) {
        lower[i] = (char)tolower((unsigned char)git[i]);
    }
    lower[len] = '\0';
    return lower;
}

/* Add a resolved dep to the result set. All fields are strdup'd. */
static void resolved_add(ResolvedDeps *r, const char *name, const char *version,
                         const char *git, const char *sha,
                         const char *cache_path) {
    if (r->count >= r->capacity) {
        int new_cap = r->capacity == 0 ? 8 : r->capacity * 2;
        IronDep *new_deps = (IronDep *)realloc(r->deps,
                                                sizeof(IronDep) * (size_t)new_cap);
        if (!new_deps) return;
        r->deps = new_deps;
        r->capacity = new_cap;
    }
    IronDep *d = &r->deps[r->count];
    memset(d, 0, sizeof(IronDep));
    d->name = strdup(name);
    d->version = version ? strdup(version) : strdup("");
    d->git = strdup(git);
    d->sha = strdup(sha);
    d->cache_path = strdup(cache_path);
    r->count++;
}

/* ── DFS resolver ──────────────────────────────────────────────────────── */

static int resolve_recursive(IronDep *deps, int dep_count,
                             IronLockEntry *lock_entries, int lock_count,
                             const char *token, bool colors,
                             ResolvedDeps *result,
                             StringSet *visited, StringSet *path) {
    for (int i = 0; i < dep_count; i++) {
        IronDep *dep = &deps[i];
        if (!dep->git) continue;

        char *key = lowercase_git(dep->git);
        if (!key) return -1;

        /* Cycle detection: check if key is in current path */
        if (string_set_contains(path, key)) {
            iron_print_error(colors, "circular dependency detected");
            for (int j = 0; j < path->count; j++) {
                fprintf(stderr, "  -> %s\n", path->items[j]);
            }
            fprintf(stderr, "  -> %s  (cycle)\n", key);
            fprintf(stderr, "  hint: remove the circular dependency from one of the packages\n");
            free(key);
            return -1;
        }

        /* Diamond dedup: already fully resolved */
        if (string_set_contains(visited, key)) {
            free(key);
            continue;
        }

        /* Resolve SHA: try lockfile first, then GitHub API */
        char *sha = NULL;
        IronLockEntry *locked = lockfile_find(lock_entries, lock_count, dep->git);
        if (locked && locked->sha) {
            sha = strdup(locked->sha);
        } else {
            sha = resolve_tag_to_sha(dep->git, dep->version, token, colors);
            if (!sha) {
                free(key);
                return -1; /* error already printed by resolve_tag_to_sha */
            }
        }

        /* Build cache path and fetch */
        char cache_dir[2048];
        dep_cache_path(cache_dir, sizeof(cache_dir), dep->git, sha);
        int fetch_ret = fetch_dependency(dep->name, dep->version, dep->git,
                                         sha, cache_dir, token, colors);
        if (fetch_ret != 0) {
            free(sha);
            free(key);
            return -1;
        }

        /* Push to path for cycle detection */
        string_set_add(path, key);

        /* Recurse into transitive deps: parse dep's iron.toml from cache */
        char dep_toml_path[2048];
        snprintf(dep_toml_path, sizeof(dep_toml_path), "%s/iron.toml", cache_dir);
        IronProject *dep_proj = iron_toml_parse(dep_toml_path);

        /* CRITICAL: missing iron.toml is an error (per user decision). */
        if (!dep_proj) {
            char errbuf[2200];
            snprintf(errbuf, sizeof(errbuf),
                     "%s has no iron.toml at %s", dep->name, dep_toml_path);
            iron_print_error(colors, errbuf);
            fprintf(stderr, "  hint: the dependency may not be an Iron package\n");
            string_set_remove_last(path);
            free(sha);
            free(key);
            return -1;
        }

        if (dep_proj->dep_count > 0) {
            /* Check for version conflicts before recursing */
            for (int t = 0; t < dep_proj->dep_count; t++) {
                IronDep *tdep = &dep_proj->deps[t];
                if (!tdep->git) continue;

                char *tkey = lowercase_git(tdep->git);
                for (int e = 0; e < result->count; e++) {
                    char *ekey = lowercase_git(result->deps[e].git);
                    if (strcmp(tkey, ekey) == 0 &&
                        result->deps[e].version && tdep->version &&
                        strcmp(result->deps[e].version, tdep->version) != 0) {
                        char msg[1024];
                        snprintf(msg, sizeof(msg),
                                 "version conflict for %s", tdep->git);
                        iron_print_error(colors, msg);
                        fprintf(stderr, "  %s requires v%s\n",
                                result->deps[e].name, result->deps[e].version);
                        fprintf(stderr, "  %s requires v%s\n",
                                dep->name, tdep->version);
                        fprintf(stderr, "  hint: align dependency versions in the respective iron.toml files\n");
                        free(ekey);
                        free(tkey);
                        iron_toml_free(dep_proj);
                        string_set_remove_last(path);
                        free(sha);
                        free(key);
                        return -1;
                    }
                    free(ekey);
                }
                free(tkey);
            }

            int rec_ret = resolve_recursive(dep_proj->deps, dep_proj->dep_count,
                                            lock_entries, lock_count, token,
                                            colors, result, visited, path);
            if (rec_ret != 0) {
                iron_toml_free(dep_proj);
                string_set_remove_last(path);
                free(sha);
                free(key);
                return -1;
            }
        }

        iron_toml_free(dep_proj);

        /* Pop from path */
        string_set_remove_last(path);

        /* Mark as visited */
        string_set_add(visited, key);

        /* Add to result (leaf deps already added via recursion above) */
        resolved_add(result, dep->name, dep->version ? dep->version : "",
                     dep->git, sha, cache_dir);

        free(sha);
        free(key);
    }

    return 0;
}

/* ── Public API ────────────────────────────────────────────────────────── */

int resolve_dependencies(IronProject *proj, const char *proj_dir,
                         bool colors, ResolvedDeps *result) {
    result->deps = NULL;
    result->count = 0;
    result->capacity = 0;

    if (proj->dep_count == 0) return 0;

    /* Read iron.lock */
    char lock_path[2048];
    snprintf(lock_path, sizeof(lock_path), "%s/iron.lock", proj_dir);
    IronLockEntry *lock_entries = NULL;
    int lock_count = lockfile_read(lock_path, &lock_entries);
    if (lock_count == -1) {
        /* Parse error in lockfile — warn and continue with empty lock */
        fprintf(stderr, "warning: could not parse iron.lock, re-resolving all dependencies\n");
        lock_entries = NULL;
        lock_count = 0;
    }

    /* Get GitHub token */
    char *token = get_github_token();

    /* DFS resolution */
    StringSet visited, path_stack;
    string_set_init(&visited);
    string_set_init(&path_stack);

    int ret = resolve_recursive(proj->deps, proj->dep_count,
                                lock_entries, lock_count, token, colors,
                                result, &visited, &path_stack);

    /* On success: write updated iron.lock from resolved deps */
    if (ret == 0 && result->count > 0) {
        IronLockEntry *new_lock = (IronLockEntry *)calloc(
            (size_t)result->count, sizeof(IronLockEntry));
        if (new_lock) {
            for (int i = 0; i < result->count; i++) {
                new_lock[i].name = result->deps[i].name ? strdup(result->deps[i].name) : NULL;
                new_lock[i].version = result->deps[i].version ? strdup(result->deps[i].version) : NULL;
                new_lock[i].git = result->deps[i].git ? strdup(result->deps[i].git) : NULL;
                new_lock[i].sha = result->deps[i].sha ? strdup(result->deps[i].sha) : NULL;
            }
            lockfile_write(lock_path, new_lock, result->count);
            lockfile_free(new_lock, result->count);
        }
    }

    /* Cleanup */
    lockfile_free(lock_entries, lock_count);
    free(token);
    string_set_free(&visited);
    string_set_free(&path_stack);

    return ret;
}

void resolved_deps_free(ResolvedDeps *result) {
    if (!result || !result->deps) return;
    for (int i = 0; i < result->count; i++) {
        free(result->deps[i].name);
        free(result->deps[i].version);
        free(result->deps[i].git);
        free(result->deps[i].sha);
        free(result->deps[i].cache_path);
    }
    free(result->deps);
    result->deps = NULL;
    result->count = 0;
    result->capacity = 0;
}
