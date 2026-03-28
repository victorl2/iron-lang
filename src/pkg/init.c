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
    char toml_content[1024];
    snprintf(toml_content, sizeof(toml_content),
             "[package]\n"
             "name = \"%s\"\n"
             "version = \"0.1.0\"\n"
             "type = \"%s\"\n"
             "\n"
             "[dependencies]\n",
             pkg_name, is_lib ? "lib" : "bin");

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
        const char *lib_content =
            "func greet(name: String) {\n"
            "    println(\"Hello, {name}!\")\n"
            "}\n";
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
