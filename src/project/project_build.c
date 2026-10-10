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
#include "util/os.h"
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
/* `-- @file: "<rel>" @line: 1` ahead of each file in combined.iron, so the
 * lexer tags that file's tokens with its own path and 1-based lines:
 * diagnostics point at src/lib.iron:2 instead of target/combined.iron:40,
 * and cross-file visibility sees distinct files. A path the marker cannot
 * quote is left unmarked. */
static void write_file_marker(FILE *combined, const char *rel_path) {
    if (strpbrk(rel_path, "\"\n\r")) return;
    fprintf(combined, "-- @file: \"%s\" @line: 1\n", rel_path);
}

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
        write_file_marker(combined, vendor_files.items[i] + proj_prefix);
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
            write_file_marker(combined, project_files.items[i] + proj_prefix);
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
 * When the installed compiler is too new (only upper bounds fail), the
 * install hint would name the version the user already has, so the message
 * states the constraint instead.
 *
 * Scope: the check is an error for `iron build` and `iron run`. `iron check`
 * reports it as a warning (as_warning) so contributors can still iterate on
 * a package whose pin has drifted from their toolchain, but are told about
 * it.
 */
static int check_iron_version(const IronProject *proj, bool colors,
                              bool as_warning) {
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
    char msg[2048];
    if (suggested && iron_semver_below_lower_bound(c, IRON_VERSION_STRING)) {
        snprintf(msg, sizeof(msg),
                 "%s requires iron %s, but you have %s. Run 'curl --proto =https --tlsv1.2 -sSfL https://ironlang.dev/install.sh | sh -s -- --version %s' to update.",
                 proj->name,
                 proj->iron_constraint,
                 IRON_VERSION_STRING,
                 suggested);
    } else {
        snprintf(msg, sizeof(msg),
                 "%s requires iron %s, but you have %s, which is newer. Install an iron release that satisfies '%s'.",
                 proj->name,
                 proj->iron_constraint,
                 IRON_VERSION_STRING,
                 proj->iron_constraint);
    }
    iron_semver_free(c);
    if (as_warning) {
        iron_print_warning(colors, msg);
        return 0;
    }
    iron_print_error(colors, msg);
    return 1;
}

/* Reject any flag in argv[2..] (up to a `--` separator) that is not in
 * allowed[] (NULL-terminated). Unknown flags used to be ignored silently. */
static int reject_unknown_flags(const char *cmd, int argc, char **argv,
                                const char *const *allowed) {
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) break;
        if (argv[i][0] != '-') continue;
        bool ok = false;
        for (int k = 0; allowed[k]; k++) {
            size_t al = strlen(allowed[k]);
            /* "--flag=" entries accept any value. */
            if (allowed[k][al - 1] == '=' ? strncmp(argv[i], allowed[k], al) == 0
                                          : strcmp(argv[i], allowed[k]) == 0) { ok = true; break; }
        }
        if (!ok) {
            fprintf(stderr, "error: unknown flag '%s' for 'iron %s'\n", argv[i], cmd);
            return 1;
        }
    }
    return 0;
}

/* ── cmd_build (handles both build and run) ─────────────────────────────── */

/* ── iron debug (#312) ──────────────────────────────────────────────── */

static bool g_debug_session = false;   /* cmd_build: debug instead of run */
static const char *g_debugger = NULL;  /* "gdb", "lldb", or NULL: default */

static bool debug_file_exists(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0;
}

/* True when `tool` is an executable on PATH. */
static bool debug_on_path(const char *tool) {
    const char *path = getenv("PATH");
    if (!path) return false;
#ifdef _WIN32
    const char sep = ';';
    const char *ext = ".exe";
#else
    const char sep = ':';
    const char *ext = "";
#endif
    char buf[4096];
    for (const char *p = path; *p;) {
        const char *e = strchr(p, sep);
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n > 0 && n + strlen(tool) + 8 < sizeof(buf)) {
            snprintf(buf, sizeof(buf), "%.*s/%s%s", (int)n, p, tool, ext);
            if (debug_file_exists(buf)) return true;
        }
        if (!e) break;
        p = e + 1;
    }
    return false;
}

/* <iron>/lib/debug/<file> next to the ironc in use, or the source tree's
 * src/debug/<file> in a dev build; NULL when neither exists. */
static char *debug_formatter(const char *ironc, const char *file) {
    char buf[4200];
    const char *slash = strrchr(ironc, '/');
#ifdef _WIN32
    const char *bs = strrchr(ironc, '\\');
    if (bs > slash) slash = bs;
#endif
    if (slash) {
        snprintf(buf, sizeof(buf), "%.*s/../lib/debug/%s", (int)(slash - ironc), ironc, file);
        if (debug_file_exists(buf)) return strdup(buf);
    }
#ifdef IRON_SOURCE_DIR
    snprintf(buf, sizeof(buf), "%s/debug/%s", IRON_SOURCE_DIR, file);
    if (debug_file_exists(buf)) return strdup(buf);
#endif
    return NULL;
}

/* Start gdb or lldb on `binary` with its arguments and the Iron formatters
 * loaded; returns the debugger's exit status. */
static int iron_launch_debugger(const char *ironc, const char *binary,
                                char **args, int nargs) {
#ifdef __APPLE__
    const char *first = "lldb", *second = "gdb";
#else
    const char *first = "gdb", *second = "lldb";
#endif
    const char *tool = g_debugger;
    if (!tool) tool = debug_on_path(first) ? first : debug_on_path(second) ? second : NULL;
    if (!tool || !debug_on_path(tool)) {
        fprintf(stderr, "error: %s\n", tool ? "the requested debugger is not on PATH"
                                            : "no debugger found: install gdb or lldb");
        fprintf(stderr, "  the binary is built with debug info: %s\n"
                        "  VS Code (and Visual Studio on Windows) can debug it; see "
                        "the guide's Debugging section\n", binary);
        return 1;
    }
    bool lldb = strcmp(tool, "lldb") == 0;
    char *formatter = debug_formatter(ironc, lldb ? "iron_lldb.py" : "iron_gdb.py");
    char cmd[4300] = "";
    if (formatter) {
        snprintf(cmd, sizeof(cmd), lldb ? "command script import %s" : "source %s", formatter);
    } else {
        fprintf(stderr, "note: the Iron value formatters were not found; values show as C\n");
    }
    char **argv = (char **)calloc((size_t)nargs + 8, sizeof(char *));
    if (!argv) { free(formatter); return 1; }
    int ai = 0;
    argv[ai++] = (char *)tool;
    if (lldb) {
        if (formatter) { argv[ai++] = "-o"; argv[ai++] = cmd; }
        argv[ai++] = "--";
    } else {
        argv[ai++] = "-q";
        if (formatter) { argv[ai++] = "-ex"; argv[ai++] = cmd; }
        argv[ai++] = "--args";
    }
    argv[ai++] = (char *)binary;
    for (int i = 0; i < nargs; i++) argv[ai++] = args[i];
    argv[ai] = NULL;
    int ret = spawn_and_wait(tool, argv);
    free(argv);
    free(formatter);
    return ret;
}

int iron_debug_file(int argc, char **argv) {
    const char *file = NULL;
    char **prog_args = NULL;
    int prog_nargs = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--gdb") == 0) g_debugger = "gdb";
        else if (strcmp(argv[i], "--lldb") == 0) g_debugger = "lldb";
        else if (strcmp(argv[i], "--") == 0) {
            prog_args = &argv[i + 1];
            prog_nargs = argc - i - 1;
            break;
        } else if (!file) file = argv[i];
        else { fprintf(stderr, "error: unexpected argument '%s'\n", argv[i]); return 1; }
    }
    if (!file) { fprintf(stderr, "error: iron debug needs a .iron file or an iron.toml\n"); return 1; }
    char out[4200];
    const char *base = strrchr(file, '/');
#ifdef _WIN32
    const char *bs = strrchr(file, '\\');
    if (bs > base) base = bs;
#endif
    base = base ? base + 1 : file;
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    snprintf(out, sizeof(out), "%s\\%.*s-debug.exe", tmp ? tmp : ".",
             (int)(strlen(base) - 5), base);
#else
    const char *tmp = getenv("TMPDIR");
    snprintf(out, sizeof(out), "%s/%.*s-debug-%d", tmp && *tmp ? tmp : "/tmp",
             (int)(strlen(base) - 5), base, (int)getpid());
#endif
    char *ironc = find_ironc();
    char *build_argv[] = { ironc, "build", (char *)file, "--debug", "-o", out, NULL };
    int ret = spawn_and_wait(ironc, build_argv);
    if (ret == 0) ret = iron_launch_debugger(ironc, out, prog_args, prog_nargs);
    remove(out);
    free(ironc);
    return ret;
}

/* `iron dap [--adapter <path>]`: the Debug Adapter Protocol server that
 * editors run (lib/debug/iron_dap.py, in Python): it builds .iron files
 * and packages with --debug, runs lldb-dap or gdb's DAP mode with the
 * value formatters loaded, and shows locals under their Iron names. */
int iron_dap(const char *self_path, int argc, char **argv) {
    char *ironc = find_ironc();
    char *script = debug_formatter(ironc, "iron_dap.py");
    free(ironc);
    if (!script) {
        fprintf(stderr, "error: iron dap: lib/debug/iron_dap.py was not found next to this iron\n");
        return 1;
    }
#ifdef _WIN32
    const char *const pythons[] = { "python3", "python", "py", NULL };
#else
    const char *const pythons[] = { "python3", "python", NULL };
#endif
    const char *python = NULL;
    for (int i = 0; pythons[i] && !python; i++)
        if (debug_on_path(pythons[i])) python = pythons[i];
    if (!python) {
        fprintf(stderr, "error: iron dap needs Python 3 on PATH\n");
        free(script);
        return 1;
    }
    char **dargv = (char **)calloc((size_t)argc + 6, sizeof(char *));
    if (!dargv) { free(script); return 1; }
    int ai = 0;
    dargv[ai++] = (char *)python;
    if (strcmp(python, "py") == 0) dargv[ai++] = "-3";
    dargv[ai++] = script;
    dargv[ai++] = "--iron";
    dargv[ai++] = (char *)self_path;
    for (int i = 2; i < argc; i++) dargv[ai++] = argv[i];
    dargv[ai] = NULL;
    int ret = spawn_and_wait(python, dargv);
    free(dargv);
    free(script);
    return ret;
}

static int cmd_build(bool run_after, int argc, char **argv) {
    bool colors = iron_color_init();

    /* Parse --verbose, --release, and -- separator for passthrough args.
     * Phase 94 LIB-04: --release is parsed at the iron build CLI layer and
     * forwarded to ironc below; the Finished status line differentiates
     * "release [optimized]" from "dev [unoptimized]" based on the same flag. */
    static const char *const allowed[] = { "--verbose", "--release", "--debug", "--target=", NULL };
    static const char *const allowed_debug[] = { "--verbose", "--gdb", "--lldb", NULL };
    if (reject_unknown_flags(g_debug_session ? "debug" : run_after ? "run" : "build", argc, argv,
                             g_debug_session ? allowed_debug : allowed) != 0)
        return 1;
    bool verbose = false;
    bool release = false;
    bool debug = g_debug_session; /* --debug: debug info on Iron lines (#312) */
    const char *target = NULL;   /* --target=<name>: web or a cross target, forwarded to ironc */
    char **run_args = NULL;
    int run_arg_count = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--release") == 0) {
            release = true;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
        } else if (strcmp(argv[i], "--gdb") == 0) {
            g_debugger = "gdb";
        } else if (strcmp(argv[i], "--lldb") == 0) {
            g_debugger = "lldb";
        } else if (strncmp(argv[i], "--target=", 9) == 0) {
            target = argv[i] + 9;
            if (strcmp(target, "native") == 0) target = NULL;
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
    if (check_iron_version(proj, colors, false) != 0) {
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
    /* A cross target's binary cannot run here, and it lands in its own
     * directory so builds for several targets do not overwrite each other. */
    bool cross = target && strcmp(target, "web") != 0;
    if (cross && run_after) {
        char msg[256];
        snprintf(msg, sizeof(msg), "a %s binary cannot run on this host; use `iron build --target=%s`", target, target);
        iron_print_error(colors, msg);
        free(proj_dir); free(toml_path); iron_toml_free(proj);
        return 1;
    }
    if (cross && is_lib) {
        iron_print_error(colors, "library packages cannot be built for another target yet");
        free(proj_dir); free(toml_path); iron_toml_free(proj);
        return 1;
    }
    if (cross) {
        char target_sub[4096];
        snprintf(target_sub, sizeof(target_sub), "%s/target/%s", proj_dir, target);
#ifdef _WIN32
        _mkdir(target_sub);
#else
        if (mkdir(target_sub, 0755) != 0 && errno != EEXIST) {
            char msg[512];
            snprintf(msg, sizeof(msg), "cannot create target/%s/: %s", target, strerror(errno));
            iron_print_error(colors, msg);
            free(proj_dir); free(toml_path); iron_toml_free(proj);
            return 1;
        }
#endif
    }
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
    if (cross) {
        /* target/<target>/<name>, with the target's executable suffix. */
        snprintf(output_path, sizeof(output_path), "%s/target/%s/%s%s", proj_dir, target, proj->name,
                 strncmp(target, "windows-", 8) == 0 ? ".exe" : "");
    }

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
    if (debug) path_list_add(&args, "--debug");
    if (target) {
        char tflag[160];
        snprintf(tflag, sizeof(tflag), "--target=%s", target);
        path_list_add(&args, tflag);
    }
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

        if (g_debug_session) {
            iron_print_status(colors, "Debugging", output_path);
            ret = iron_launch_debugger(ironc, output_path, run_args, run_arg_count);
        } else if (run_after) {
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
    static const char *const allowed[] = { NULL };
    if (reject_unknown_flags("check", argc, argv, allowed) != 0) return 1;
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

    /* Report a version pin mismatch as a warning (malformed pins still
     * fail). */
    if (check_iron_version(proj, colors, true) != 0) {
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

/* Whether the file declares `func main(` at the start of a line: a
 * standalone test program, rather than a file of `test` blocks. */
static bool defines_main(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[4096];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        const char *c = line;
        while (*c == ' ' || *c == '\t' || (unsigned char)*c == 0xEF ||
               (unsigned char)*c == 0xBB || (unsigned char)*c == 0xBF) c++;
        if (strncmp(c, "pub ", 4) == 0) c += 4;
        if (strncmp(c, "func main(", 10) == 0) found = true;
    }
    fclose(f);
    return found;
}

/*
 * `iron test [filter]`: the `test "name" { ... }` blocks of vendor/, src/
 * and tests/ are compiled together, as one program with the project's own
 * code (target/tests.iron), and run by `ironc test`, one process per test;
 * the filter keeps the tests whose name contains it. A tests/test_*.iron
 * file with its own main is a standalone test program, run as before.
 */
static int cmd_test(int argc, char **argv) {
    static const char *const allowed[] = { NULL };
    if (reject_unknown_flags("test", argc, argv, allowed) != 0) return 1;
    bool colors = iron_color_init();
    const char *filter = NULL;
    for (int i = 2; i < argc; i++)
        if (argv[i][0] != '-') { filter = argv[i]; break; }

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

    char detail[512];
    snprintf(detail, sizeof(detail), "%s v%s", proj->name, proj->version ? proj->version : "?");
    iron_print_status(colors, "Testing", detail);

    PathList vendor_files = {0}, project_files = {0}, test_files = {0};
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/vendor", proj_dir);
    if (is_dir(dir)) collect_vendor_dir(dir, &vendor_files);
    collect_project_sources(proj_dir, &project_files);
    snprintf(dir, sizeof(dir), "%s/tests", proj_dir);
    bool has_tests_dir = is_dir(dir);
    bool has_standalone = false;
    if (has_tests_dir) {
        PathList names = {0};
        if (list_dir_sorted(dir, &names) == 0) {
            for (int i = 0; i < names.count; i++) {
                if (!has_iron_ext(names.items[i])) continue;
                char path[4096];
                snprintf(path, sizeof(path), "%s/%s", dir, names.items[i]);
                if (!is_file(path)) continue;
                if (defines_main(path)) {
                    if (strncmp(names.items[i], "test_", 5) == 0) has_standalone = true;
                } else {
                    path_list_add(&test_files, path);
                }
            }
        }
        path_list_free(&names);
    }

    /* target/tests.iron: the project with its test files. */
    char target_dir[4096], combined_path[4096];
    snprintf(target_dir, sizeof(target_dir), "%s/target", proj_dir);
#ifdef _WIN32
    _mkdir(target_dir);
#else
    mkdir(target_dir, 0755);
#endif
    snprintf(combined_path, sizeof(combined_path), "%s/target/tests.iron", proj_dir);
    FILE *combined = fopen(combined_path, "w");
    int ret = 0;
    if (!combined) {
        iron_print_error(colors, "cannot create target/tests.iron");
        ret = 1;
    } else {
        size_t proj_prefix = strlen(proj_dir) + 1;
        PathList *groups[3] = { &vendor_files, &project_files, &test_files };
        for (int g = 0; g < 3; g++) {
            for (int i = 0; i < groups[g]->count; i++) {
                write_file_marker(combined, groups[g]->items[i] + proj_prefix);
                append_file(combined, groups[g]->items[i]);
            }
        }
        fclose(combined);

        char *ironc = find_ironc();
        char *spawn_argv[] = { ironc, "test", combined_path, (char *)filter, NULL };
        ret = spawn_and_wait(ironc, spawn_argv);
        if (has_standalone) {
            /* Standalone programs run whole; the filter does not apply. */
            char *legacy_argv[] = { ironc, "test", dir, NULL };
            int lret = spawn_and_wait(ironc, legacy_argv);
            if (ret == 0) ret = lret;
        }
        free(ironc);
    }

    if (ret == 0) iron_print_status(colors, "Finished", "all tests passed");

    path_list_free(&vendor_files);
    path_list_free(&project_files);
    path_list_free(&test_files);
    free(proj_dir);
    free(toml_path);
    iron_toml_free(proj);
    return ret;
}

/* ── cmd_project dispatcher ─────────────────────────────────────────────── */

int cmd_project(const char *cmd, int argc, char **argv) {
    if (strcmp(cmd, "build") == 0) return cmd_build(false, argc, argv);
    if (strcmp(cmd, "run") == 0)   return cmd_build(true, argc, argv);
    if (strcmp(cmd, "debug") == 0) {
        g_debug_session = true;
        return cmd_build(true, argc, argv);
    }
    if (strcmp(cmd, "check") == 0) return cmd_check(argc, argv);
    if (strcmp(cmd, "test") == 0)  return cmd_test(argc, argv);
    if (strcmp(cmd, "fmt") == 0) {
        /* fmt not yet supported in project mode */
        fprintf(stderr, "error: 'iron fmt' requires a file argument\n");
        return 1;
    }
    return 1;
}
