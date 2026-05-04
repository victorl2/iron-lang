/*
 * iron init — scaffold a new Iron project (binary or library).
 *
 * Creates:
 *   iron.toml        — project manifest
 *   src/main.iron    — hello-world entry (bin) OR
 *   src/lib.iron     — library entry (--lib)
 *   .gitignore       — /target/
 * And runs `git init` if no .git directory exists.
 *
 * All file writes are non-destructive: existing files are skipped.
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
#endif

#include "pkg/color.h"
#include "pkg/init.h"
#include "pkg/iron_pkg.h"
#include "cli/version.h"

/* ── write_if_absent ────────────────────────────────────────────────────── */

/*
 * Write content to path only if the file does not already exist.
 * Returns:  1  — file created
 *           0  — file already exists (skipped)
 *          -1  — error
 */
static int write_if_absent(const char *path, const char *content) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return 0; /* already exists — skip */
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "error: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t len = strlen(content);
    if (fwrite(content, 1, len, f) != len) {
        fprintf(stderr, "error: write failed for %s: %s\n", path, strerror(errno));
        fclose(f);
        return -1;
    }
    fclose(f);
    return 1;
}

/* ── compute_minor_floor_version ────────────────────────────────────────── */

/*
 * Compute the major.minor.0 floor of IRON_VERSION_STRING for the iron init
 * template's `iron = ">= X.Y.0"` line (Phase 95 PIN-04). The minor floor
 * keeps the pin loose enough that the freshly scaffolded project always
 * builds on the compiler that scaffolded it. Returns the heap "X.Y.0"
 * string (caller must free) or NULL on parse failure (caller falls back
 * to omitting the iron line).
 */
static char *compute_minor_floor_version(void) {
    int major = 0, minor = 0;
    if (sscanf(IRON_VERSION_STRING, "%d.%d.", &major, &minor) != 2) {
        return NULL;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.0", major, minor);
    char *out = (char *)malloc(strlen(buf) + 1);
    if (!out) return NULL;
    strcpy(out, buf);
    return out;
}

/* ── cmd_init ───────────────────────────────────────────────────────────── */

int cmd_init(int argc, char **argv) {
    /* Parse --lib flag */
    bool is_lib = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--lib") == 0) {
            is_lib = true;
        }
    }

    bool colors = iron_color_init();

    /* Derive package name from current directory basename */
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        fprintf(stderr, "error: cannot determine current directory: %s\n",
                strerror(errno));
        return 1;
    }

    const char *pkg_name = strrchr(cwd, '/');
#ifdef _WIN32
    {
        const char *pkg_name_win = strrchr(cwd, '\\');
        if (pkg_name_win > pkg_name) pkg_name = pkg_name_win;
    }
#endif
    pkg_name = (pkg_name != NULL) ? pkg_name + 1 : cwd;

    /* ── iron.toml ──────────────────────────────────────────────────────── */
    /* Phase 95 PIN-04: emit the [package].iron field with a major.minor.0
     * floor computed from IRON_VERSION_STRING so the scaffolded project
     * always satisfies its own pin on the compiler that scaffolded it.
     * If parsing the running version fails, fall back to omitting the
     * iron line (PIN-04 backward-compat path). */
    char toml_content[1024];
    char *floor = compute_minor_floor_version();
    if (floor) {
        snprintf(toml_content, sizeof(toml_content),
                 "[package]\n"
                 "name = \"%s\"\n"
                 "version = \"0.1.0\"\n"
                 "type = \"%s\"\n"
                 "iron = \">= %s\"  # Minimum iron compiler version. Update when adopting features that require a newer compiler.\n"
                 "\n"
                 "[dependencies]\n",
                 pkg_name, is_lib ? "lib" : "bin", floor);
        free(floor);
    } else {
        snprintf(toml_content, sizeof(toml_content),
                 "[package]\n"
                 "name = \"%s\"\n"
                 "version = \"0.1.0\"\n"
                 "type = \"%s\"\n"
                 "\n"
                 "[dependencies]\n",
                 pkg_name, is_lib ? "lib" : "bin");
    }

    if (write_if_absent("iron.toml", toml_content) < 0) return 1;

    /* ── src/ directory ─────────────────────────────────────────────────── */
#ifdef _WIN32
    _mkdir("src");
#else
    if (mkdir("src", 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "error: cannot create src/: %s\n", strerror(errno));
        return 1;
    }
#endif

    /* ── source file ────────────────────────────────────────────────────── */
    if (is_lib) {
        /* Phase 94 LIB-04: lib template uses `pub` so it compiles as a real
         * archive on first build (no empty-pub-surface warning) and exercises
         * the Phase 93 top-level pub keyword. The function name `hello` is
         * fixed (locked by 94-CONTEXT.md) so the lib_init_smoke.sh test in
         * Plan 94-04 can grep for it via nm. */
        char lib_content[512];
        snprintf(lib_content, sizeof(lib_content),
                 "pub func hello() -> String {\n"
                 "    return \"Hello from %s!\"\n"
                 "}\n",
                 pkg_name);
        if (write_if_absent("src/lib.iron", lib_content) < 0) return 1;
    } else {
        const char *main_content =
            "func main() {\n"
            "    println(\"Hello, Iron!\")\n"
            "}\n";
        if (write_if_absent("src/main.iron", main_content) < 0) return 1;
    }

    /* ── .gitignore ─────────────────────────────────────────────────────── */
    if (write_if_absent(".gitignore", "/target/\n") < 0) return 1;

    /* ── git init (if no .git present) ─────────────────────────────────── */
    struct stat git_st;
    if (stat(".git", &git_st) != 0) {
        char *git_argv[] = { "git", "init", "-q", NULL };
        spawn_and_wait("git", git_argv); /* ignore failure — git may not be installed */
    }

    /* ── success status ─────────────────────────────────────────────────── */
    char detail[512];
    snprintf(detail, sizeof(detail), "%s `%s` package",
             is_lib ? "library" : "binary", pkg_name);
    iron_print_status(colors, "Created", detail);

    return 0;
}
