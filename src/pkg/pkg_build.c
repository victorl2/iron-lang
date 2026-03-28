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
#include "pkg/color.h"
#include "pkg/iron_pkg.h"
#include "pkg/pkg_build.h"

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

/* ── cmd_build (handles both build and run) ─────────────────────────────── */

static int cmd_build(bool run_after, int argc, char **argv) {
    bool colors = iron_color_init();

    /* Parse --verbose and -- separator for passthrough args */
    bool verbose = false;
    char **run_args = NULL;
    int run_arg_count = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
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

    /* 5. Build output path: target/<name> */
    char output_path[4096];
#ifdef _WIN32
    snprintf(output_path, sizeof(output_path), "%s/target/%s.exe", proj_dir, proj->name);
#else
    snprintf(output_path, sizeof(output_path), "%s/target/%s", proj_dir, proj->name);
#endif

    /* 6. Print compiling status */
    char detail[512];
    snprintf(detail, sizeof(detail), "%s v%s", proj->name, proj->version);
    iron_print_status(colors, "Compiling", detail);

    /* 7. Time the build */
    double t_start = get_time_sec();

    /* 8. Invoke ironc: ironc build <entry> --output <output_path> [--verbose] */
    char *ironc = find_ironc();

    int arg_count = 0;
    char *spawn_argv[16];
    spawn_argv[arg_count++] = ironc;
    spawn_argv[arg_count++] = "build";
    spawn_argv[arg_count++] = entry_path;
    spawn_argv[arg_count++] = "--output";
    spawn_argv[arg_count++] = output_path;
    if (verbose) spawn_argv[arg_count++] = "--verbose";
    spawn_argv[arg_count] = NULL;

    int ret = spawn_and_wait(ironc, spawn_argv);

    double elapsed = get_time_sec() - t_start;

    /* 9. Report result */
    if (ret == 0) {
        snprintf(detail, sizeof(detail), "dev [unoptimized] in %.2fs", elapsed);
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
    int total = 0, passed = 0, failed = 0;

#ifndef _WIN32
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".iron") != 0) continue;

        char file_path[4096];
        snprintf(file_path, sizeof(file_path), "%s/%s", tests_dir, ent->d_name);

        total++;
        iron_print_status(colors, "Running", ent->d_name);

        char *spawn_argv[] = { ironc, "test", file_path, NULL };
        int ret = spawn_and_wait(ironc, spawn_argv);
        if (ret == 0) {
            passed++;
        } else {
            failed++;
        }
    }
    closedir(d);
#endif

    /* Summary */
    if (failed == 0 && total > 0) {
        snprintf(detail, sizeof(detail), "%d test(s) passed", passed);
        iron_print_status(colors, "Finished", detail);
    } else if (failed > 0) {
        snprintf(detail, sizeof(detail), "%d passed, %d failed", passed, failed);
        iron_print_error(colors, detail);
    } else {
        iron_print_status(colors, "Finished", "no test files found in tests/");
    }

    free(ironc);
    free(proj_dir);
    free(toml_path);
    iron_toml_free(proj);
    return failed > 0 ? 1 : 0;
}

/* ── cmd_package dispatcher ─────────────────────────────────────────────── */

int cmd_package(const char *cmd, int argc, char **argv) {
    if (strcmp(cmd, "build") == 0) return cmd_build(false, argc, argv);
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
