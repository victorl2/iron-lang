/*
 * iron project-mode commands: build, run, check, test
 *
 * Reads iron.toml, assembles the project's sources (vendored code under
 * vendor/ first, then src/), invokes ironc as a subprocess, and prints
 * Cargo-style status lines with Iron-branded orange.
 *
 * Iron has no package manager. Third-party Iron code is copied into the
 * project's vendor/ directory and compiled together with the project.
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
#include "project/color.h"
#include "project/iron_project.h"
#include "project/project_build.h"

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

/* ── Path lists ─────────────────────────────────────────────────────────── */

typedef struct {
    char **items;
    int    count;
    int    cap;
} PathList;

static void path_list_add(PathList *l, const char *path) {
    if (l->count >= l->cap) {
        int new_cap = l->cap == 0 ? 16 : l->cap * 2;
        char **grown = (char **)realloc(l->items, sizeof(char *) * (size_t)new_cap);
        if (!grown) return;
        l->items = grown;
        l->cap = new_cap;
    }
    char *copy = strdup(path);
    if (copy) l->items[l->count++] = copy;
}

static void path_list_free(PathList *l) {
    for (int i = 0; i < l->count; i++) free(l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = l->cap = 0;
}

/* qsort comparator for path strings */
static int path_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static bool has_iron_ext(const char *name) {
    size_t n = strlen(name);
    return n > 5 && strcmp(name + n - 5, ".iron") == 0;
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* List the names in `dir` (excluding "." and ".."), sorted.
 * Returns 0 on success, -1 when the directory cannot be opened. */
static int list_dir_sorted(const char *dir, PathList *out) {
#ifdef _WIN32
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        path_list_add(out, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        path_list_add(out, ent->d_name);
    }
    closedir(d);
#endif
    if (out->count > 1) {
        qsort(out->items, (size_t)out->count, sizeof(char *), path_cmp);
    }
    return 0;
}

/* ── Vendored sources ───────────────────────────────────────────────────── */

/*
 * Directories never compiled from vendor/: hidden directories (.git, ...),
 * build output, and a vendored library's own tests and examples.
 */
static bool vendor_skip_dir(const char *name) {
    return name[0] == '.' ||
           strcmp(name, "target") == 0 ||
           strcmp(name, "tests") == 0 ||
           strcmp(name, "examples") == 0;
}

/*
 * Recursively collect .iron files under `dir` into `out` (sorted, depth
 * first). A directory that holds both an iron.toml and a src/ directory is
 * a vendored Iron project: only its src/ is compiled, so a library can be
 * copied into vendor/ as-is.
 */
static void collect_vendor_dir(const char *dir, PathList *out) {
    char probe[4096];
    snprintf(probe, sizeof(probe), "%s/iron.toml", dir);
    if (is_file(probe)) {
        char src_dir[4096];
        snprintf(src_dir, sizeof(src_dir), "%s/src", dir);
        if (is_dir(src_dir)) {
            collect_vendor_dir(src_dir, out);
            return;
        }
    }

    PathList names = {0};
    if (list_dir_sorted(dir, &names) != 0) return;
    for (int i = 0; i < names.count; i++) {
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", dir, names.items[i]);
        if (is_dir(child)) {
            if (!vendor_skip_dir(names.items[i])) collect_vendor_dir(child, out);
        } else if (has_iron_ext(names.items[i])) {
            path_list_add(out, child);
        }
    }
    path_list_free(&names);
}

/* ── Project sources ────────────────────────────────────────────────────── */

/*
 * Collect the project's own .iron files from src/ (top level only):
 * lib.iron first (if present), the rest alphabetically, main.iron last.
 */
static void collect_project_sources(const char *proj_dir, PathList *out) {
    char src_dir[4096];
    snprintf(src_dir, sizeof(src_dir), "%s/src", proj_dir);

    PathList names = {0};
    if (list_dir_sorted(src_dir, &names) != 0) return;

    bool has_lib = false, has_main = false;
    for (int i = 0; i < names.count; i++) {
        if (strcmp(names.items[i], "lib.iron") == 0)  has_lib = true;
        if (strcmp(names.items[i], "main.iron") == 0) has_main = true;
    }

    char path[4096];
    if (has_lib) {
        snprintf(path, sizeof(path), "%s/lib.iron", src_dir);
        path_list_add(out, path);
    }
    for (int i = 0; i < names.count; i++) {
        const char *n = names.items[i];
        if (!has_iron_ext(n)) continue;
        if (strcmp(n, "lib.iron") == 0 || strcmp(n, "main.iron") == 0) continue;
        snprintf(path, sizeof(path), "%s/%s", src_dir, n);
        if (is_file(path)) path_list_add(out, path);
    }
    if (has_main) {
        snprintf(path, sizeof(path), "%s/main.iron", src_dir);
        path_list_add(out, path);
    }
    path_list_free(&names);
}

/* ── Source assembly ────────────────────────────────────────────────────── */

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
 * Decide what ironc compiles for this project. A single-file project with
 * nothing vendored compiles its entry point directly. Otherwise every
 * vendored file (everything under vendor/, sorted) and every project file
 * (the .iron files in src/) is concatenated into target/combined.iron,
 * which ironc compiles as one unit.
 *
 * Writes the chosen path into source_out. Returns 0 on success, 1 on error
 * (already reported).
 */
static int assemble_sources(const IronProject *proj, const char *proj_dir,
                            const char *entry_path, bool colors,
                            char *source_out, size_t source_out_size) {
    PathList vendor_files = {0};
    char vendor_dir[4096];
    snprintf(vendor_dir, sizeof(vendor_dir), "%s/vendor", proj_dir);
    if (is_dir(vendor_dir)) collect_vendor_dir(vendor_dir, &vendor_files);

    PathList project_files = {0};
    collect_project_sources(proj_dir, &project_files);

    if (vendor_files.count == 0 && project_files.count <= 1) {
        snprintf(source_out, source_out_size, "%s", entry_path);
        path_list_free(&vendor_files);
        path_list_free(&project_files);
        return 0;
    }

    char target_dir[4096];
    snprintf(target_dir, sizeof(target_dir), "%s/target", proj_dir);
#ifdef _WIN32
    _mkdir(target_dir);
#else
    if (mkdir(target_dir, 0755) != 0 && errno != EEXIST) {
        char msg[4200];
        snprintf(msg, sizeof(msg), "cannot create target/: %s", strerror(errno));
        iron_print_error(colors, msg);
        path_list_free(&vendor_files);
        path_list_free(&project_files);
        return 1;
    }
#endif

    snprintf(source_out, source_out_size, "%s/target/combined.iron", proj_dir);
    FILE *combined = fopen(source_out, "w");
    if (!combined) {
        iron_print_error(colors, "cannot create target/combined.iron");
        path_list_free(&vendor_files);
        path_list_free(&project_files);
        return 1;
    }

    size_t proj_prefix = strlen(proj_dir) + 1;  /* strip "<proj_dir>/" */
    int ret = 0;
    for (int i = 0; i < vendor_files.count && ret == 0; i++) {
        fprintf(combined, "-- vendor: %s\n", vendor_files.items[i] + proj_prefix);
        if (append_file(combined, vendor_files.items[i]) != 0) {
            char msg[4200];
            snprintf(msg, sizeof(msg), "cannot read vendored file %s",
                     vendor_files.items[i] + proj_prefix);
            iron_print_error(colors, msg);
            ret = 1;
        }
    }
    if (ret == 0) {
        fprintf(combined, "-- project: %s\n", proj->name);
        for (int i = 0; i < project_files.count; i++) {
            append_file(combined, project_files.items[i]);
        }
    }

    fclose(combined);
    path_list_free(&vendor_files);
    path_list_free(&project_files);
    return ret;
}

/* ── Legacy [dependencies] ──────────────────────────────────────────────── */

/*
 * Iron has no package manager, so [dependencies] is not supported. An empty
 * table (the old `iron init` template) or `raylib = true` (raylib is enabled
 * by `import raylib`) only warns; any real entry is an error that points at
 * vendoring. Returns 1 when the build must stop.
 */
static int check_legacy_dependencies(const IronProject *proj, bool colors) {
    if (!proj->legacy_deps_section) return 0;

    if (proj->legacy_dep_name) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "iron.toml declares dependency '%s', but Iron has no package manager.",
                 proj->legacy_dep_name);
        iron_print_error(colors, msg);
        fprintf(stderr,
                "  hint: copy the library's source into vendor/%s/ and remove the\n"
                "        [dependencies] table. `iron build` compiles everything under\n"
                "        vendor/ together with your project.\n"
                "        See https://ironlang.dev/guide/#vendoring\n",
                proj->legacy_dep_name);
        return 1;
    }

    iron_print_warning(colors,
        "[dependencies] in iron.toml is ignored: Iron has no package manager. "
        "Remove the table (raylib is enabled by `import raylib`).");
    return 0;
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
     * This fires BEFORE entry-file lookup or any compile work so the user
     * gets the friendly hint immediately. The em-dash in the locked CONTEXT
     * message is intentional (per 94-CONTEXT.md verbatim). */
    if (run_after && proj->type && strcmp(proj->type, "lib") == 0) {
        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "package '%s' has type \"lib\" \xe2\x80\x94 libraries are not directly runnable.",
                 proj->name);
        iron_print_error(colors, msg);
        fprintf(stderr,
                "  hint: run `iron build` to produce target/lib%s.a, or vendor the\n"
                "        library into a binary project's vendor/ directory.\n",
                proj->name);
        free(toml_path);
        iron_toml_free(proj);
        return 1;
    }

    /* Iron has no package manager: stop on a legacy [dependencies] entry
     * before any filesystem work so the migration hint is the only output. */
    if (check_legacy_dependencies(proj, colors) != 0) {
        free(toml_path);
        iron_toml_free(proj);
        return 1;
    }

    /* Phase 95 PIN-02 / PIN-03: enforce iron-compiler version pinning.
     * Fires after iron.toml parse + LIB-06 rejection but before any
     * filesystem work (target/ creation, ironc spawn) so a mismatched
     * constraint is fail-fast and side-effect-free. cmd_run dispatches
     * through cmd_build(true, ...) (see cmd_project), so this single
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
     * pipeline runs (clang -c then llvm-ar/ar wrap).
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

    /* 6. Assemble sources: vendored files + project files, or just the
     * entry point for a single-file project with nothing vendored. */
    char build_source[4096];
    if (assemble_sources(proj, proj_dir, entry_path, colors,
                         build_source, sizeof(build_source)) != 0) {
        free(proj_dir); free(toml_path); iron_toml_free(proj);
        return 1;
    }

    /* 7. Assemble the ironc argv:
     *    ironc build <source> --output <output_path> [flags] */
    char *ironc = find_ironc();
    PathList args = {0};
    path_list_add(&args, ironc);
    path_list_add(&args, "build");
    path_list_add(&args, build_source);
    path_list_add(&args, "--output");
    path_list_add(&args, output_path);
    if (verbose) path_list_add(&args, "--verbose");
    /* Phase 94 LIB-04: forward --release from iron build CLI to ironc so
     * native -O2 (and web -Oz -flto) optimization tiers reach the underlying
     * clang -c invocation. Applies to both type=bin and type=lib builds. */
    if (release) path_list_add(&args, "--release");
    /* Phase 94 LIB-01: lib builds go through ironc's archive emit path. */
    if (is_lib) {
        path_list_add(&args, "--emit-archive");
        path_list_add(&args, "--pkg-name");
        path_list_add(&args, proj->name);
        path_list_add(&args, "--pkg-version");
        path_list_add(&args, proj->version);
    }
    char **spawn_argv = (char **)malloc(sizeof(char *) * (size_t)(args.count + 1));
    if (!spawn_argv) {
        fprintf(stderr, "error: out of memory\n");
        path_list_free(&args);
        free(ironc); free(proj_dir); free(toml_path); iron_toml_free(proj);
        return 1;
    }
    for (int i = 0; i < args.count; i++) spawn_argv[i] = args.items[i];
    spawn_argv[args.count] = NULL;

    /* 8. Print compiling status */
    char detail[512];
    snprintf(detail, sizeof(detail), "%s v%s", proj->name, proj->version);
    iron_print_status(colors, "Compiling", detail);

    /* 9. Time the build and invoke ironc */
    double t_start = get_time_sec();
    int ret = spawn_and_wait(ironc, spawn_argv);
    double elapsed = get_time_sec() - t_start;

    free(spawn_argv);
    path_list_free(&args);

    /* 10. Report result.
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

    if (check_legacy_dependencies(proj, colors) != 0) {
        free(toml_path);
        iron_toml_free(proj);
        return 1;
    }

    char *proj_dir = get_project_dir(toml_path);
    const char *entry_basename = (proj->type && strcmp(proj->type, "lib") == 0)
                                  ? "src/lib.iron" : "src/main.iron";
    char entry_path[4096];
    snprintf(entry_path, sizeof(entry_path), "%s/%s", proj_dir, entry_basename);

    /* Check the same source set `iron build` compiles, so vendored code and
     * sibling src/ files resolve. */
    char check_source[4096];
    if (assemble_sources(proj, proj_dir, entry_path, colors,
                         check_source, sizeof(check_source)) != 0) {
        free(proj_dir); free(toml_path); iron_toml_free(proj);
        return 1;
    }

    char detail[512];
    snprintf(detail, sizeof(detail), "%s v%s", proj->name, proj->version ? proj->version : "?");
    iron_print_status(colors, "Checking", detail);

    char *ironc = find_ironc();
    char *spawn_argv[] = { ironc, "check", check_source, NULL };
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

/* ── cmd_project dispatcher ─────────────────────────────────────────────── */

int cmd_project(const char *cmd, int argc, char **argv) {
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
        /* fmt not yet supported in project mode */
        fprintf(stderr, "error: 'iron fmt' requires a file argument\n");
        return 1;
    }
    return 1;
}
