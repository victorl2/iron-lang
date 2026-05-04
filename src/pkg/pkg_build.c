/*
 * iron package-mode commands: build, run, check, test
 *
 * Reads iron.toml, resolves entry point, invokes ironc as subprocess,
 * and prints Cargo-style status lines with Iron-branded orange.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdbool.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <time.h>
#endif

#include "cli/toml.h"
#include "cli/semver.h"
#include "cli/version.h"
#include "pkg/color.h"
#include "pkg/iron_pkg.h"
#include "pkg/pkg_build.h"
#include "pkg/resolver.h"
#include "pkg/fetcher.h"

/* ── Platform timing ────────────────────────────────────────────────────── */

#ifdef _WIN32
static double get_time_sec(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart / (double)freq.QuadPart;
}
#else
static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
#endif

/* ── find_iron_toml ─────────────────────────────────────────────────────── */

/*
 * Walk up from cwd to find iron.toml.
 * Returns malloc'd path on success, NULL if not found.
 */
static char *find_iron_toml(void) {
    char dir[4096];
    if (getcwd(dir, sizeof(dir)) == NULL) return NULL;

    while (1) {
        char path[4096 + 16];
        snprintf(path, sizeof(path), "%s/iron.toml", dir);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            return strdup(path);
        }

        char *sep = strrchr(dir, '/');
#ifdef _WIN32
        char *sep_win = strrchr(dir, '\\');
        if (sep_win > sep) sep = sep_win;
#endif
        if (!sep || sep == dir) break;
        *sep = '\0';
    }
    return NULL;
}

/* ── get_project_dir ────────────────────────────────────────────────────── */

/*
 * Extract directory component from iron.toml path (everything before last '/').
 * Returns malloc'd string; caller must free.
 */
static char *get_project_dir(const char *toml_path) {
    char *copy = strdup(toml_path);
    if (!copy) return NULL;
    char *sep = strrchr(copy, '/');
#ifdef _WIN32
    char *sep_win = strrchr(copy, '\\');
    if (sep_win > sep) sep = sep_win;
#endif
    if (sep) *sep = '\0';
    return copy;
}

/* ── Source collection helpers ──────────────────────────────────────────── */

/* qsort comparator for filename strings */
static int fname_cmp(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/*
 * Append contents of a file to the combined output.
 * Returns 0 on success, -1 on failure.
 */
static int append_file(FILE *out, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(f);
    fprintf(out, "\n");
    return 0;
}

/*
 * Collect all .iron files from a dependency's directory and write to out.
 * Looks in {dir}/src/ first, falls back to {dir}/ if no src/.
 * lib.iron is written first (if it exists), then remaining files alphabetically.
 */
static void collect_iron_files(FILE *out, const char *dir) {
    char src_dir[2048];
    snprintf(src_dir, sizeof(src_dir), "%s/src", dir);

    const char *scan_dir = src_dir;
    struct stat st;
    if (stat(src_dir, &st) != 0) {
        scan_dir = dir; /* no src/ subdir, scan root */
    }

#ifndef _WIN32
    DIR *d = opendir(scan_dir);
    if (!d) return;

    /* Collect filenames */
    char **names = NULL;
    int count = 0, cap = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".iron") != 0) continue;

        if (count >= cap) {
            cap = cap == 0 ? 16 : cap * 2;
            names = (char **)realloc(names, sizeof(char *) * (size_t)cap);
        }
        names[count++] = strdup(ent->d_name);
    }
    closedir(d);

    if (count == 0) {
        free(names);
        return;
    }

    /* Sort alphabetically */
    qsort(names, (size_t)count, sizeof(char *), fname_cmp);

    /* Move lib.iron to front if present */
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], "lib.iron") == 0 && i > 0) {
            char *tmp = names[i];
            memmove(&names[1], &names[0], sizeof(char *) * (size_t)i);
            names[0] = tmp;
            break;
        }
    }

    /* Write each file */
    for (int i = 0; i < count; i++) {
        char fpath[2048];
        snprintf(fpath, sizeof(fpath), "%s/%s", scan_dir, names[i]);
        append_file(out, fpath);
        free(names[i]);
    }
    free(names);
#else
    /* Windows: FindFirstFile/FindNextFile */
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s\\*.iron", scan_dir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    char **names = NULL;
    int count = 0, cap = 0;

    do {
        if (count >= cap) {
            cap = cap == 0 ? 16 : cap * 2;
            names = (char **)realloc(names, sizeof(char *) * (size_t)cap);
        }
        names[count++] = strdup(fd.cFileName);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    qsort(names, (size_t)count, sizeof(char *), fname_cmp);

    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], "lib.iron") == 0 && i > 0) {
            char *tmp = names[i];
            memmove(&names[1], &names[0], sizeof(char *) * (size_t)i);
            names[0] = tmp;
            break;
        }
    }

    for (int i = 0; i < count; i++) {
        char fpath[2048];
        snprintf(fpath, sizeof(fpath), "%s\\%s", scan_dir, names[i]);
        append_file(out, fpath);
        free(names[i]);
    }
    free(names);
#endif
}

/*
 * Collect all .iron files from project's src/ directory.
 * main.iron is written last (entry point).
 * lib.iron is written first (if it exists).
 */
static void collect_project_files(FILE *out, const char *proj_dir) {
    char src_dir[2048];
    snprintf(src_dir, sizeof(src_dir), "%s/src", proj_dir);

#ifndef _WIN32
    DIR *d = opendir(src_dir);
    if (!d) return;

    char **names = NULL;
    int count = 0, cap = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".iron") != 0) continue;
        /* Skip main.iron — write it last */
        if (strcmp(ent->d_name, "main.iron") == 0) continue;

        if (count >= cap) {
            cap = cap == 0 ? 16 : cap * 2;
            names = (char **)realloc(names, sizeof(char *) * (size_t)cap);
        }
        names[count++] = strdup(ent->d_name);
    }
    closedir(d);

    /* Sort alphabetically */
    if (count > 1) {
        qsort(names, (size_t)count, sizeof(char *), fname_cmp);
    }

    /* Move lib.iron to front if present */
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], "lib.iron") == 0 && i > 0) {
            char *tmp = names[i];
            memmove(&names[1], &names[0], sizeof(char *) * (size_t)i);
            names[0] = tmp;
            break;
        }
    }

    /* Write non-main files */
    for (int i = 0; i < count; i++) {
        char fpath[2048];
        snprintf(fpath, sizeof(fpath), "%s/%s", src_dir, names[i]);
        append_file(out, fpath);
        free(names[i]);
    }
    free(names);
#endif

    /* Write main.iron last (if it exists) */
    char main_path[2048];
    snprintf(main_path, sizeof(main_path), "%s/src/main.iron", proj_dir);
    struct stat mst;
    if (stat(main_path, &mst) == 0) {
        append_file(out, main_path);
    }
}

/* ── check_iron_version (Phase 95 PIN-02 / PIN-03) ─────────────────────────
 *
 * Compares the running compiler's IRON_VERSION_STRING against
 * proj->iron_constraint. Fail-fast: returns 1 with the locked error message
 * printed to stderr if the constraint is malformed or unsatisfied.
 * Returns 0 when:
 *   - proj->iron_constraint is NULL or empty (PIN-04: no field = no check)
 *   - the constraint parses and the running compiler satisfies it
 *
 * Locked error message (verbatim per 95-CONTEXT.md):
 *   error: <pkg> requires iron <constraint>, but you have <current>. Run
 *   'curl --proto =https --tlsv1.2 -sSfL https://ironlang.dev/install.sh |
 *   sh -s -- --version <suggested>' to update.
 *
 * Scope (v3.2): the version check fires for `iron build` and `iron run`
 * (which dispatches through cmd_build) only. `iron check` and `iron test`
 * intentionally skip the check per CONTEXT.md so contributors can iterate
 * on a package whose pin floor has drifted ahead of their toolchain.
 */
static int check_iron_version(const IronProject *proj, bool colors) {
    if (!proj->iron_constraint || proj->iron_constraint[0] == '\0') {
        return 0;  /* PIN-04: missing/empty field is permitted */
    }

    IronSemverConstraint *c = iron_semver_parse(proj->iron_constraint);
    if (!c) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "invalid iron version constraint in iron.toml: '%s'",
                 proj->iron_constraint);
        iron_print_error(colors, msg);
        fprintf(stderr,
                "  hint: use a Cargo-style semver constraint, e.g. "
                "iron = \">= 3.2.0\" or iron = \">= 3.0.0, < 4.0.0\".\n");
        return 1;
    }

    if (iron_semver_satisfies(c, IRON_VERSION_STRING)) {
        iron_semver_free(c);
        return 0;
    }

    const char *suggested = iron_semver_suggest_version(c);
    if (!suggested) suggested = IRON_VERSION_STRING;

    char msg[2048];
    snprintf(msg, sizeof(msg),
             "%s requires iron %s, but you have %s. Run 'curl --proto =https --tlsv1.2 -sSfL https://ironlang.dev/install.sh | sh -s -- --version %s' to update.",
             proj->name,
             proj->iron_constraint,
             IRON_VERSION_STRING,
             suggested);
    iron_print_error(colors, msg);
    iron_semver_free(c);
    return 1;
}

/* ── cmd_build (handles both build and run) ─────────────────────────────── */

static int cmd_build(bool run_after, int argc, char **argv) {
    bool colors = iron_color_init();

    /* Parse --verbose, --release, and -- separator for passthrough args.
     * Phase 94 LIB-04: --release is parsed at the iron build CLI layer and
     * forwarded to ironc below; the Finished status line differentiates
     * "release [optimized]" from "dev [unoptimized]" based on the same flag. */
    bool verbose = false;
    bool release = false;
    char **run_args = NULL;
    int run_arg_count = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--release") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--") == 0) {
            run_args = &argv[i + 1];
            run_arg_count = argc - i - 1;
            break;
        }
    }

    /* 1. Find iron.toml */
    char *toml_path = find_iron_toml();
    if (!toml_path) {
        iron_print_error(colors, "no iron.toml found in current directory or any parent");
        fprintf(stderr, "hint: run 'iron init' to create a new project, or pass a .iron file\n");
        return 1;
    }

    /* 2. Parse iron.toml */
    IronProject *proj = iron_toml_parse(toml_path);
    if (!proj || !proj->name || !proj->version) {
        iron_print_error(colors, "invalid iron.toml: missing required fields (name, version)");
        free(toml_path);
        if (proj) iron_toml_free(proj);
        return 1;
    }

    /* Phase 94 LIB-06: reject `iron run` on type=lib at iron.toml-parse moment.
     * This fires BEFORE entry-file lookup, dep resolution, or any compile work
     * so the user gets the friendly hint immediately. The em-dash in the
     * locked CONTEXT message is intentional (per 94-CONTEXT.md verbatim). */
    if (run_after && proj->type && strcmp(proj->type, "lib") == 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "package '%s' has type \"lib\" \xe2\x80\x94 libraries are not directly runnable.",
                 proj->name);
        iron_print_error(colors, msg);
        fprintf(stderr,
                "  hint: run `iron build` to produce target/lib%s.a, or run a consumer\n"
                "        binary that depends on it.\n",
                proj->name);
        free(toml_path);
        iron_toml_free(proj);
        return 1;
    }

    /* Phase 95 PIN-02 / PIN-03: enforce iron-compiler version pinning.
     * Fires after iron.toml parse + LIB-06 rejection but before any
     * filesystem work (target/ creation, ironc spawn) so a mismatched
     * constraint is fail-fast and side-effect-free. cmd_run dispatches
     * through cmd_build(true, ...) (see cmd_package), so this single
     * call site covers both `iron build` and `iron run`. */
    if (check_iron_version(proj, colors) != 0) {
        free(toml_path);
        iron_toml_free(proj);
        return 1;
    }

    /* 3. Derive project directory and entry point */
    char *proj_dir = get_project_dir(toml_path);
    const char *entry_basename = (proj->type && strcmp(proj->type, "lib") == 0)
                                  ? "src/lib.iron" : "src/main.iron";
    char entry_path[4096];
    snprintf(entry_path, sizeof(entry_path), "%s/%s", proj_dir, entry_basename);

    /* Verify entry file exists */
    struct stat st;
    if (stat(entry_path, &st) != 0) {
        char msg[512];
        snprintf(msg, sizeof(msg), "entry point not found: %s", entry_basename);
        iron_print_error(colors, msg);
        free(proj_dir); free(toml_path); iron_toml_free(proj);
        return 1;
    }

    /* 4. Ensure target/ directory exists */
    char target_dir[4096];
    snprintf(target_dir, sizeof(target_dir), "%s/target", proj_dir);
#ifdef _WIN32
    _mkdir(target_dir);
#else
    if (mkdir(target_dir, 0755) != 0 && errno != EEXIST) {
        char msg[512];
        snprintf(msg, sizeof(msg), "cannot create target/: %s", strerror(errno));
        iron_print_error(colors, msg);
        free(proj_dir); free(toml_path); iron_toml_free(proj);
        return 1;
    }
#endif

    /* 5. Build output path: target/<name> for bin, target/lib<name>.a for lib.
     * Phase 94 LIB-01: type=lib routes through the archive emit path. The
     * --emit-archive flag is forwarded to ironc below so the static archive
     * pipeline runs (clang -c then llvm-ar/ar wrap). Plan 94-02 needs this
     * forwarded slice so path-dep resolver can recursively build libs into
     * lib<name>.a archives; full LIB-04/05/06 work (init template + run
     * rejection) lands in Plan 94-03.
     *
     * Phase 96 RUN-02: when running a bin package (run_after && !is_lib),
     * route the binary to <proj_dir>/target/run/<name> instead of
     * <proj_dir>/target/<name> so cwd and the package root stay clean. mkdir
     * target/run/ if needed; ignore EEXIST. The lib branch is unaffected
     * (libs still emit to target/lib<name>.a; Phase 94 LIB-06 already rejects
     * `iron run` on libs upstream). `iron build` (run_after=false) keeps the
     * v3.1 behavior: binaries land at target/<name> so external scripts that
     * look for build artifacts there continue to work. */
    bool is_lib = (proj->type && strcmp(proj->type, "lib") == 0);
    bool route_to_run_dir = run_after && !is_lib;
    if (route_to_run_dir) {
        char target_run_dir[4096];
        snprintf(target_run_dir, sizeof(target_run_dir),
                 "%s/target/run", proj_dir);
#ifdef _WIN32
        _mkdir(target_run_dir);
#else
        if (mkdir(target_run_dir, 0755) != 0 && errno != EEXIST) {
            char msg[512];
            snprintf(msg, sizeof(msg),
                     "cannot create target/run/: %s", strerror(errno));
            iron_print_error(colors, msg);
            free(proj_dir); free(toml_path); iron_toml_free(proj);
            return 1;
        }
#endif
    }
    char output_path[4096];
#ifdef _WIN32
    if (is_lib) {
        snprintf(output_path, sizeof(output_path),
                 "%s/target/lib%s.a", proj_dir, proj->name);
    } else if (route_to_run_dir) {
        snprintf(output_path, sizeof(output_path),
                 "%s/target/run/%s.exe", proj_dir, proj->name);
    } else {
        snprintf(output_path, sizeof(output_path),
                 "%s/target/%s.exe", proj_dir, proj->name);
    }
#else
    if (is_lib) {
        snprintf(output_path, sizeof(output_path),
                 "%s/target/lib%s.a", proj_dir, proj->name);
    } else if (route_to_run_dir) {
        snprintf(output_path, sizeof(output_path),
                 "%s/target/run/%s", proj_dir, proj->name);
    } else {
        snprintf(output_path, sizeof(output_path),
                 "%s/target/%s", proj_dir, proj->name);
    }
#endif

    /* 6. Resolve dependencies (if any) */
    ResolvedDeps resolved = {0};
    if (proj->dep_count > 0) {
        int dep_ret = resolve_dependencies(proj, proj_dir, colors, &resolved);
        if (dep_ret != 0) {
            free(proj_dir); free(toml_path); iron_toml_free(proj);
            return 1;
        }
    }

    /* 7. Build source: concatenate deps + project into target/combined.iron
     * Use combined source when deps exist OR when project has multiple .iron files. */
    char combined_path[4096];
    snprintf(combined_path, sizeof(combined_path), "%s/target/combined.iron", proj_dir);

    /* Check if project has multiple .iron files in src/ */
    bool multi_file = false;
    {
        char src_path[4096];
        snprintf(src_path, sizeof(src_path), "%s/src", proj_dir);
#ifndef _WIN32
        DIR *src_d = opendir(src_path);
        if (src_d) {
            int iron_count = 0;
            struct dirent *src_ent;
            while ((src_ent = readdir(src_d)) != NULL) {
                size_t nlen = strlen(src_ent->d_name);
                if (nlen >= 6 && strcmp(src_ent->d_name + nlen - 5, ".iron") == 0)
                    iron_count++;
            }
            closedir(src_d);
            if (iron_count > 1) multi_file = true;
        }
#endif
    }

    bool use_combined = (resolved.count > 0 || multi_file);

    if (use_combined) {
        FILE *combined = fopen(combined_path, "w");
        if (!combined) {
            iron_print_error(colors, "cannot create target/combined.iron");
            resolved_deps_free(&resolved);
            free(proj_dir); free(toml_path); iron_toml_free(proj);
            return 1;
        }

        /* Write dep source files in topological order (leaves first).
         * Phase 94 LIB-03: path-deps concat the .iron-stub (NOT the lib's
         * source) preceded by Phase 93's `-- @file: <stub-path>` marker so
         * the consumer's combined source carries the lib's public surface
         * with the lib's own filename identity preserved. Git-form path is
         * kept for future REGISTRY-01 (currently parsed-but-runtime-ignored
         * in v3.2 — git-deps are not produced by the resolver right now). */
        for (int i = 0; i < resolved.count; i++) {
            if (resolved.deps[i].path) {
                /* Path-dep: concat <cache_path>/target/<name>.iron-stub */
                char stub_path[4096];
                snprintf(stub_path, sizeof(stub_path), "%s/target/%s.iron-stub",
                         resolved.deps[i].cache_path, resolved.deps[i].name);
                fprintf(combined, "-- @file: %s\n", stub_path);
                if (append_file(combined, stub_path) != 0) {
                    char errbuf[2048];
                    snprintf(errbuf, sizeof(errbuf),
                             "failed to read .iron-stub for path dep '%s' at %s",
                             resolved.deps[i].name, stub_path);
                    iron_print_error(colors, errbuf);
                    fclose(combined);
                    resolved_deps_free(&resolved);
                    free(proj_dir); free(toml_path); iron_toml_free(proj);
                    return 1;
                }
            } else {
                /* Git-source path (parsed-but-runtime-ignored in v3.2). */
                fprintf(combined, "-- dependency: %s\n", resolved.deps[i].name);
                collect_iron_files(combined, resolved.deps[i].cache_path);
            }
        }

        /* Write project source files (main.iron last) */
        fprintf(combined, "-- project: %s\n", proj->name);
        collect_project_files(combined, proj_dir);

        fclose(combined);
    }

    /* Use combined source when active, otherwise use entry point directly */
    char *build_source = use_combined ? combined_path : entry_path;

    /* 8. Print compiling status */
    char detail[512];
    snprintf(detail, sizeof(detail), "%s v%s", proj->name, proj->version);
    iron_print_status(colors, "Compiling", detail);

    /* 9. Time the build */
    double t_start = get_time_sec();

    /* 10. Invoke ironc: ironc build <source> --output <output_path> [--verbose] */
    char *ironc = find_ironc();

    int arg_count = 0;
    /* Phase 94 LIB-03: spawn_argv sized for up to 8 path-deps * 2 link flags
     * each plus the base argv (~10 entries) — bumped from 16 to 32. */
    char *spawn_argv[32];
    spawn_argv[arg_count++] = ironc;
    spawn_argv[arg_count++] = "build";
    spawn_argv[arg_count++] = build_source;
    spawn_argv[arg_count++] = "--output";
    spawn_argv[arg_count++] = output_path;
    if (verbose) spawn_argv[arg_count++] = "--verbose";
    /* Phase 94 LIB-04: forward --release from iron build CLI to ironc so
     * native -O2 (and web -Oz -flto) optimization tiers reach the underlying
     * clang -c invocation. Applies to both type=bin and type=lib builds. */
    if (release) spawn_argv[arg_count++] = "--release";
    /* Phase 94 LIB-01: lib builds go through ironc's archive emit path. */
    if (is_lib) {
        spawn_argv[arg_count++] = "--emit-archive";
        spawn_argv[arg_count++] = "--pkg-name";
        spawn_argv[arg_count++] = proj->name;
        spawn_argv[arg_count++] = "--pkg-version";
        spawn_argv[arg_count++] = proj->version;
    }
    /* Phase 94 LIB-03: per path-dep, emit -L<dep>/target -l<name> so the
     * consumer's clang link line picks up the static archives. Storage
     * persists for the duration of the spawn call (heap-owned). The
     * resolver's topological order is leaf-first; that order is preserved
     * here (resolved.deps[0] is the deepest leaf). */
    char *link_flag_storage[16] = {0};
    int link_flag_alloc = 0;
    for (int i = 0; i < resolved.count; i++) {
        if (!resolved.deps[i].path) continue;
        if (arg_count + 2 >=
            (int)(sizeof(spawn_argv) / sizeof(spawn_argv[0])) - 1) break;
        if (link_flag_alloc + 2 >
            (int)(sizeof(link_flag_storage) / sizeof(link_flag_storage[0]))) break;
        char buf[1024];
        snprintf(buf, sizeof(buf), "-L%s/target", resolved.deps[i].cache_path);
        link_flag_storage[link_flag_alloc] = strdup(buf);
        spawn_argv[arg_count++] = link_flag_storage[link_flag_alloc++];
        snprintf(buf, sizeof(buf), "-l%s", resolved.deps[i].name);
        link_flag_storage[link_flag_alloc] = strdup(buf);
        spawn_argv[arg_count++] = link_flag_storage[link_flag_alloc++];
    }
    spawn_argv[arg_count] = NULL;

    int ret = spawn_and_wait(ironc, spawn_argv);

    /* Free the heap-owned link-flag storage now that the spawn is done. */
    for (int li = 0; li < link_flag_alloc; li++) {
        free(link_flag_storage[li]);
    }

    double elapsed = get_time_sec() - t_start;

    /* 11. Report result.
     * Phase 94 LIB-04: status line differentiates "release [optimized]" from
     * "dev [unoptimized]" based on whether --release was passed to iron build. */
    if (ret == 0) {
        snprintf(detail, sizeof(detail), "%s [%s] in %.2fs",
                 release ? "release" : "dev",
                 release ? "optimized" : "unoptimized",
                 elapsed);
        iron_print_status(colors, "Finished", detail);

        if (run_after) {
            iron_print_status(colors, "Running", output_path);

            /* Execute the built binary with passthrough args */
            int exec_argc = 1 + run_arg_count + 1;
            char **exec_argv = (char **)malloc(sizeof(char *) * (size_t)exec_argc);
            if (!exec_argv) {
                fprintf(stderr, "error: out of memory\n");
                resolved_deps_free(&resolved);
                free(ironc); free(proj_dir); free(toml_path); iron_toml_free(proj);
                return 1;
            }
            exec_argv[0] = output_path;
            for (int i = 0; i < run_arg_count; i++) {
                exec_argv[1 + i] = run_args[i];
            }
            exec_argv[1 + run_arg_count] = NULL;

            ret = spawn_and_wait(output_path, exec_argv);
            free(exec_argv);
        }
    }
    /* ironc errors already printed to stderr — pass through unchanged */

    resolved_deps_free(&resolved);
    free(ironc);
    free(proj_dir);
    free(toml_path);
    iron_toml_free(proj);
    return ret;
}

/* ── cmd_check ──────────────────────────────────────────────────────────── */

static int cmd_check(int argc, char **argv) {
    (void)argc; (void)argv;
    bool colors = iron_color_init();

    char *toml_path = find_iron_toml();
    if (!toml_path) {
        iron_print_error(colors, "no iron.toml found");
        return 1;
    }

    IronProject *proj = iron_toml_parse(toml_path);
    if (!proj || !proj->name) {
        iron_print_error(colors, "invalid iron.toml");
        free(toml_path);
        if (proj) iron_toml_free(proj);
        return 1;
    }

    char *proj_dir = get_project_dir(toml_path);
    const char *entry_basename = (proj->type && strcmp(proj->type, "lib") == 0)
                                  ? "src/lib.iron" : "src/main.iron";
    char entry_path[4096];
    snprintf(entry_path, sizeof(entry_path), "%s/%s", proj_dir, entry_basename);

    char detail[512];
    snprintf(detail, sizeof(detail), "%s v%s", proj->name, proj->version ? proj->version : "?");
    iron_print_status(colors, "Checking", detail);

    char *ironc = find_ironc();
    char *spawn_argv[] = { ironc, "check", entry_path, NULL };
    int ret = spawn_and_wait(ironc, spawn_argv);

    if (ret == 0) {
        iron_print_status(colors, "Finished", "check completed");
    }

    free(ironc);
    free(proj_dir);
    free(toml_path);
    iron_toml_free(proj);
    return ret;
}

/* ── cmd_test ───────────────────────────────────────────────────────────── */

static int cmd_test(int argc, char **argv) {
    (void)argc; (void)argv;
    bool colors = iron_color_init();

    char *toml_path = find_iron_toml();
    if (!toml_path) {
        iron_print_error(colors, "no iron.toml found");
        return 1;
    }

    IronProject *proj = iron_toml_parse(toml_path);
    if (!proj || !proj->name) {
        iron_print_error(colors, "invalid iron.toml");
        free(toml_path);
        if (proj) iron_toml_free(proj);
        return 1;
    }

    char *proj_dir = get_project_dir(toml_path);
    char tests_dir[4096];
    snprintf(tests_dir, sizeof(tests_dir), "%s/tests", proj_dir);

    /* Check if tests/ directory exists */
#ifdef _WIN32
    struct stat tst;
    if (stat(tests_dir, &tst) != 0 || !S_ISDIR(tst.st_mode)) {
        iron_print_status(colors, "Testing", "no tests/ directory found");
        free(proj_dir); free(toml_path); iron_toml_free(proj);
        return 0;
    }
#else
    DIR *d = opendir(tests_dir);
    if (!d) {
        iron_print_status(colors, "Testing", "no tests/ directory found");
        free(proj_dir); free(toml_path); iron_toml_free(proj);
        return 0; /* not an error — just no tests */
    }
#endif

    char detail[512];
    snprintf(detail, sizeof(detail), "%s v%s", proj->name, proj->version ? proj->version : "?");
    iron_print_status(colors, "Testing", detail);

    char *ironc = find_ironc();

#ifndef _WIN32
    closedir(d);
#endif

    /* Delegate to ironc test with the tests/ directory.
     * ironc test expects a directory path and discovers .iron files itself. */
    char *spawn_argv[] = { ironc, "test", tests_dir, NULL };
    int ret = spawn_and_wait(ironc, spawn_argv);

    if (ret == 0) {
        iron_print_status(colors, "Finished", "all tests passed");
    }

    free(ironc);
    free(proj_dir);
    free(toml_path);
    iron_toml_free(proj);
    return ret;
}

/* ── cmd_package dispatcher ─────────────────────────────────────────────── */

int cmd_package(const char *cmd, int argc, char **argv) {
    if (strcmp(cmd, "build") == 0) return cmd_build(false, argc, argv);
    /* Phase 96 RUN-03 (reserved, NOT implemented in v3.2):
     *   --keep-binary  reserved to suppress the atexit unlink (iron-run-XXXXXX)
     *                  for users who want to inspect the produced binary.
     *   -o <path>      reserved as an output-path override for `iron run`.
     * Both flags are documented in `iron run --help` (Phase 97 HELP-03 scope).
     * Implementing them in v3.2 was descoped: the cwd-clean default covers the
     * primary issue (#53); a deliberate keep-binary flag belongs in a later
     * phase alongside the broader CLI help registry work. */
    if (strcmp(cmd, "run") == 0)   return cmd_build(true, argc, argv);
    if (strcmp(cmd, "check") == 0) return cmd_check(argc, argv);
    if (strcmp(cmd, "test") == 0)  return cmd_test(argc, argv);
    if (strcmp(cmd, "fmt") == 0) {
        /* fmt not yet supported in package mode */
        fprintf(stderr, "error: 'iron fmt' requires a file argument\n");
        return 1;
    }
    return 1;
}
