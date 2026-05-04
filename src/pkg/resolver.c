/*
 * iron dependency resolver: DFS graph traversal with cycle detection,
 * diamond dedup, version conflict detection, and topological ordering.
 *
 * Phase 94 LIB-03 adds local-path dispatch: when an IronDep carries a
 * non-NULL `path` field, the resolver bypasses the git/SHA/cache pipeline
 * and instead reads the lib's iron.toml, freshness-checks the archive
 * against source files (and iron.toml), spawns a recursive `iron build`
 * when stale, and records the absolute lib project dir as cache_path.
 * The path field is preserved on the resolved entry so pkg_build can
 * branch on it when assembling combined.iron.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#  include <process.h>
#  include <windows.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#  include <sys/wait.h>
#endif

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

/* ── Phase 94 LIB-03: local-path dependency dispatch ───────────────────── */

/* Return file mtime as a long, or -1 if stat fails (file missing). */
static long file_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_mtime;
}

/* Determine if the static archive at `archive_path` is stale relative to
 * the lib's source dir and iron.toml. Returns true when:
 *   - the archive does not exist, OR
 *   - iron.toml is newer than the archive, OR
 *   - any .iron file under <src_dir> is newer than the archive.
 * iron.toml is included in the freshness check so a lib version bump
 * (without source change) still triggers a rebuild — the .iron-stub's
 * header carries the version string. */
static bool archive_is_stale(const char *archive_path, const char *src_dir,
                             const char *toml_path) {
    long arc_mtime = file_mtime(archive_path);
    if (arc_mtime < 0) return true;  /* missing archive == stale */
    long toml_m = file_mtime(toml_path);
    if (toml_m > arc_mtime) return true;
#ifndef _WIN32
    DIR *d = opendir(src_dir);
    if (!d) return true;  /* no src dir == treat as stale to surface error */
    struct dirent *ent;
    bool stale = false;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".iron") != 0) continue;
        char src_file[4096];
        snprintf(src_file, sizeof(src_file), "%s/%s", src_dir, ent->d_name);
        long src_m = file_mtime(src_file);
        if (src_m > arc_mtime) { stale = true; break; }
    }
    closedir(d);
    return stale;
#else
    /* Windows: scan src_dir\*.iron via FindFirstFileA. */
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s\\*.iron", src_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return true;
    bool stale = false;
    do {
        char src_file[4096];
        snprintf(src_file, sizeof(src_file), "%s\\%s", src_dir, fd.cFileName);
        long src_m = file_mtime(src_file);
        if (src_m > arc_mtime) { stale = true; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return stale;
#endif
}

/* Spawn `iron build` in `lib_dir` and wait for completion.
 * Returns 0 on success, non-zero on failure (child exit != 0 or fork/exec
 * failure). The recursive build relies on `iron` being on PATH (the parent
 * was invoked as `iron build`). Working directory is restored via fork+chdir
 * pattern so the parent's cwd is unaffected. */
static int spawn_recursive_iron_build(const char *lib_dir) {
#ifndef _WIN32
    pid_t pid = fork();
    if (pid == 0) {
        if (chdir(lib_dir) != 0) _exit(127);
        execlp("iron", "iron", "build", (char *)NULL);
        _exit(127);
    } else if (pid < 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
#else
    /* Windows: chdir + spawn + restore cwd. spawnlp is synchronous. */
    char prev_cwd[4096];
    if (_getcwd(prev_cwd, sizeof(prev_cwd)) == NULL) return -1;
    if (_chdir(lib_dir) != 0) return -1;
    intptr_t rc = _spawnlp(_P_WAIT, "iron", "iron", "build", NULL);
    _chdir(prev_cwd);
    return (rc == 0) ? 0 : -1;
#endif
}

/* Resolve a single local-path dependency: read the lib's iron.toml,
 * confirm type=lib, freshness-check the archive, recursively build when
 * stale, and append a ResolvedDep entry whose cache_path is the absolute
 * lib project dir and whose path field is preserved.
 *
 * Returns 0 on success, non-zero on failure (lib_proj missing, type
 * mismatch, recursive build failure, archive missing post-build, or
 * transitive path-deps detected — out of scope for v3.2 LIB-03). */
static int handle_path_dep(IronDep *dep, const char *consumer_dir,
                           bool colors, ResolvedDeps *result) {
    /* 1. Resolve dep->path against consumer_dir, then canonicalize. */
    char abs_lib_dir[4096];
    if (dep->path[0] == '/') {
        snprintf(abs_lib_dir, sizeof(abs_lib_dir), "%s", dep->path);
    } else {
        snprintf(abs_lib_dir, sizeof(abs_lib_dir), "%s/%s",
                 consumer_dir, dep->path);
    }
#ifndef _WIN32
    char canonical[4096];
    if (realpath(abs_lib_dir, canonical) != NULL) {
        snprintf(abs_lib_dir, sizeof(abs_lib_dir), "%s", canonical);
    }
#endif

    /* 2. Read the lib's iron.toml and assert type == "lib". */
    char lib_toml_path[4096];
    snprintf(lib_toml_path, sizeof(lib_toml_path),
             "%s/iron.toml", abs_lib_dir);
    IronProject *lib_proj = iron_toml_parse(lib_toml_path);
    if (!lib_proj || !lib_proj->name) {
        char errbuf[2048];
        snprintf(errbuf, sizeof(errbuf),
                 "path dep '%s' has no readable iron.toml at %s",
                 dep->name, lib_toml_path);
        iron_print_error(colors, errbuf);
        if (lib_proj) iron_toml_free(lib_proj);
        return -1;
    }
    if (!lib_proj->type || strcmp(lib_proj->type, "lib") != 0) {
        char errbuf[2048];
        snprintf(errbuf, sizeof(errbuf),
                 "path dep '%s' at %s is not a type=\"lib\" package",
                 dep->name, abs_lib_dir);
        iron_print_error(colors, errbuf);
        fprintf(stderr,
                "  hint: only `type = \"lib\"` packages can be used as path-deps\n");
        iron_toml_free(lib_proj);
        return -1;
    }

    /* 3. v3.2 LIB-03: transitive path-deps deferred. Reject if the lib's
     *    iron.toml itself declares any path-form deps. Git-form deps stay
     *    parsed-but-runtime-ignored per the v3.2 REGISTRY-01 deferral, so
     *    they pass through silently here. */
    for (int t = 0; t < lib_proj->dep_count; t++) {
        if (lib_proj->deps[t].path) {
            char errbuf[2048];
            snprintf(errbuf, sizeof(errbuf),
                     "transitive path-dep '%s' inside lib '%s' is not supported in v3.2",
                     lib_proj->deps[t].name, dep->name);
            iron_print_error(colors, errbuf);
            fprintf(stderr,
                    "  hint: v3.2 supports a single direct path-dep depth (consumer -> lib).\n");
            iron_toml_free(lib_proj);
            return -1;
        }
    }

    /* 4. Freshness check + recursive iron build if stale. */
    char archive_path[4096];
    snprintf(archive_path, sizeof(archive_path),
             "%s/target/lib%s.a", abs_lib_dir, lib_proj->name);
    char src_dir[4096];
    snprintf(src_dir, sizeof(src_dir), "%s/src", abs_lib_dir);
    if (archive_is_stale(archive_path, src_dir, lib_toml_path)) {
        int build_rc = spawn_recursive_iron_build(abs_lib_dir);
        if (build_rc != 0) {
            char errbuf[2048];
            snprintf(errbuf, sizeof(errbuf),
                     "recursive `iron build` failed for path dep '%s' at %s",
                     dep->name, abs_lib_dir);
            iron_print_error(colors, errbuf);
            iron_toml_free(lib_proj);
            return -1;
        }
        if (file_mtime(archive_path) < 0) {
            char errbuf[2048];
            snprintf(errbuf, sizeof(errbuf),
                     "recursive build of '%s' did not produce %s",
                     dep->name, archive_path);
            iron_print_error(colors, errbuf);
            iron_toml_free(lib_proj);
            return -1;
        }
    }

    /* 5. Append to ResolvedDeps. cache_path holds the absolute lib project
     *    dir. The dep's path is preserved on the resolved entry so pkg_build
     *    can branch on it when assembling combined.iron and the link line. */
    resolved_add(result, lib_proj->name,
                 lib_proj->version ? lib_proj->version : "0.0.0",
                 "" /* git: empty marker for path-deps */,
                 "" /* sha: empty marker for path-deps */,
                 abs_lib_dir);
    if (result->count > 0) {
        IronDep *added = &result->deps[result->count - 1];
        free(added->path);
        added->path = strdup(dep->path);
    }

    iron_toml_free(lib_proj);
    return 0;
}

/* ── DFS resolver ──────────────────────────────────────────────────────── */

static int resolve_recursive(IronDep *deps, int dep_count,
                             IronLockEntry *lock_entries, int lock_count,
                             const char *token, bool colors,
                             ResolvedDeps *result,
                             StringSet *visited, StringSet *path,
                             const char *consumer_dir) {
    for (int i = 0; i < dep_count; i++) {
        IronDep *dep = &deps[i];
        /* Phase 94 LIB-03: local-path dep — bypass git/SHA/cache pipeline. */
        if (dep->path) {
            int prc = handle_path_dep(dep, consumer_dir, colors, result);
            if (prc != 0) return prc;
            continue;
        }
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
                                            colors, result, visited, path,
                                            cache_dir);
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
                                result, &visited, &path_stack, proj_dir);

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
        free(result->deps[i].path);  /* Phase 94 LIB-03 */
    }
    free(result->deps);
    result->deps = NULL;
    result->count = 0;
    result->capacity = 0;
}
