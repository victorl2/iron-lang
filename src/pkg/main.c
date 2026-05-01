/*
 * iron - The Iron package manager
 *
 * Discovers and invokes `ironc` for single-file workflows.
 * Implements iron init, build, run, check, test for iron.toml projects.
 * Shows Cargo-style help for package commands.
 *
 * Design: iron does NOT link iron_compiler — clean process boundary.
 * It discovers ironc as a sibling binary and spawns it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#  include <windows.h>
#  include <process.h>
#else
#  include <unistd.h>
#  include <spawn.h>
#  include <sys/wait.h>
#endif

#include "pkg/color.h"
#include "pkg/init.h"
#include "pkg/pkg_build.h"
#include "pkg/iron_pkg.h"
#include "cli/version.h"
#include "cli/help_registry.h"

#ifndef IRON_GIT_HASH
#define IRON_GIT_HASH "unknown"
#endif
#ifndef IRON_BUILD_DATE
#define IRON_BUILD_DATE "unknown"
#endif

#ifndef _WIN32
extern char **environ;
#endif

/* ── Platform: resolve full path of current executable ─────────────────── */

static int resolve_self_path(char *buf, size_t buf_size) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)buf_size;
    if (_NSGetExecutablePath(buf, &size) != 0) return -1;
    return 0;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, buf_size - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)buf_size);
    if (n == 0 || n >= (DWORD)buf_size) return -1;
    return 0;
#else
    (void)buf; (void)buf_size;
    return -1;
#endif
}

/* ── ironc discovery ────────────────────────────────────────────────────── */

/*
 * find_ironc: returns malloc'd absolute path to the ironc binary.
 * Strategy: resolve own path, replace "iron" with "ironc" in filename.
 * Falls back to "ironc" (PATH search) if sibling not found.
 */
char *find_ironc(void) {
    char self_path[4096];
    if (resolve_self_path(self_path, sizeof(self_path)) != 0) {
        return strdup("ironc");
    }

    /* Find last path separator */
    char *last_sep = strrchr(self_path, '/');
#ifdef _WIN32
    {
        char *last_sep_win = strrchr(self_path, '\\');
        if (last_sep_win > last_sep) last_sep = last_sep_win;
    }
#endif

    if (!last_sep) return strdup("ironc");

    /* Check the filename portion */
    char *filename = last_sep + 1;

#ifdef _WIN32
    /* On Windows: replace "iron.exe" with "ironc.exe" */
    if (strcmp(filename, "iron.exe") == 0 || strcmp(filename, "Iron.exe") == 0) {
        strcpy(filename, "ironc.exe");
    } else {
        return strdup("ironc");
    }
#else
    /* On Unix: replace "iron" with "ironc" */
    if (strcmp(filename, "iron") == 0) {
        strcpy(filename, "ironc");
    } else {
        return strdup("ironc");
    }
#endif

    /* Verify the sibling exists */
    struct stat st;
    if (stat(self_path, &st) == 0) {
        return strdup(self_path);
    }

    /* Fallback: rely on PATH */
    return strdup("ironc");
}

/* ── Subprocess invocation ──────────────────────────────────────────────── */

/*
 * spawn_and_wait: spawn an arbitrary program with argv (NULL-terminated).
 * Returns the child exit code, or 1 on spawn failure.
 */
int spawn_and_wait(const char *prog, char *const argv[]) {
#ifdef _WIN32
    /* Build command line string for CreateProcess */
    char cmd[32768];
    int pos = snprintf(cmd, sizeof(cmd), "\"%s\"", prog);
    for (int i = 1; argv[i] != NULL && pos < (int)sizeof(cmd) - 1; i++) {
        pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " \"%s\"", argv[i]);
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "error: failed to spawn %s (error %lu)\n", prog, GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
#else
    pid_t pid;
    int status = posix_spawnp(&pid, prog, NULL, NULL,
                               (char *const *)argv, environ);
    if (status != 0) {
        fprintf(stderr, "error: failed to spawn %s: %s\n", prog, strerror(status));
        return 1;
    }

    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0) {
        fprintf(stderr, "error: waitpid failed: %s\n", strerror(errno));
        return 1;
    }
    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }
    return 1;
#endif
}

/* ── Argument inspection ────────────────────────────────────────────────── */

/*
 * has_iron_file_arg: returns 1 if any argument (after the subcommand)
 * ends in ".iron". Flags (starting with '-') are skipped.
 */
static int has_iron_file_arg(int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-') continue; /* skip flags */
        size_t len = strlen(argv[i]);
        if (len > 5 && strcmp(argv[i] + len - 5, ".iron") == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * forward_to_ironc: spawn ironc with argv[1..] forwarded verbatim.
 * Returns ironc's exit code.
 */
int forward_to_ironc(int argc, char **argv) {
    char *ironc_path = find_ironc();

    /* Build argv array: [ironc_path, argv[1], argv[2], ..., NULL] */
    char **new_argv = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
    if (!new_argv) {
        fprintf(stderr, "error: out of memory\n");
        free(ironc_path);
        return 1;
    }
    new_argv[0] = ironc_path;
    for (int i = 1; i < argc; i++) {
        new_argv[i] = argv[i];
    }
    new_argv[argc] = NULL;

    int ret = spawn_and_wait(ironc_path, (char *const *)new_argv);
    free(new_argv);
    free(ironc_path);
    return ret;
}

/* ── Help and version ───────────────────────────────────────────────────── */

static void print_version(void) {
    printf("iron %s (%s %s)\n",
           IRON_VERSION_STRING, IRON_GIT_HASH, IRON_BUILD_DATE);
}

/*
 * print_help: forwards to the central help registry. The `colors` parameter
 * is preserved for caller signature stability (the unknown-command branch
 * passes it); registry output is uncolored in v3.2.
 */
static void print_help(bool colors) {
    (void)colors;
    iron_help_print_all(stdout);
}

/*
 * argv_contains_help: scan argv[start..argc) for --help or -h.
 * Returns 1 if found anywhere, 0 otherwise. Phase 97 HELP-01 pre-dispatch
 * scan helper: lets main() short-circuit to subcommand-scoped help BEFORE
 * any subcommand handler runs, so commands like `iron init --help` print
 * help and exit 0 instead of scaffolding files.
 */
static int argv_contains_help(int argc, char **argv, int start) {
    for (int i = start; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            return 1;
        }
    }
    return 0;
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    bool colors = iron_color_init();

    /* No args: show help (exit 0 — Cargo-style, unlike ironc which exits 1) */
    if (argc < 2) {
        print_help(colors);
        return 0;
    }

    const char *cmd = argv[1];

    /* Global flags */
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        print_version();
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        iron_help_print_all(stdout);
        return 0;
    }

    /*
     * Phase 97 HELP-01: pre-dispatch --help scan. If argv[1] is a known
     * subcommand AND --help/-h appears anywhere in argv[2..argc), print
     * subcommand-scoped help and exit 0 BEFORE the subcommand handler
     * runs. This is what makes `iron init --help` not scaffold files,
     * `iron build --help` not create target/, and so on (HELP-02).
     *
     * `migrate` is included even though src/pkg/main.c does not currently
     * dispatch it (it's an ironc-only command); listing it here means
     * `iron migrate --help` prints migrate help instead of falling
     * through to the unknown-command branch.
     */
    {
        static const char *KNOWN_SUBS[] = {
            "init", "build", "run", "check", "fmt", "test", "migrate", NULL
        };
        int is_known_sub = 0;
        for (int i = 0; KNOWN_SUBS[i]; i++) {
            if (strcmp(cmd, KNOWN_SUBS[i]) == 0) { is_known_sub = 1; break; }
        }
        if (is_known_sub && argv_contains_help(argc, argv, 2)) {
            iron_help_print_subcommand(cmd, stdout);
            return 0;
        }
    }

    /* init subcommand */
    if (strcmp(cmd, "init") == 0) {
        return cmd_init(argc, argv);
    }

    /* Known subcommands */
    if (strcmp(cmd, "build") == 0 || strcmp(cmd, "run") == 0 ||
        strcmp(cmd, "check") == 0 || strcmp(cmd, "fmt") == 0 ||
        strcmp(cmd, "test") == 0) {

        /* If a .iron file argument is present, forward to ironc silently */
        if (has_iron_file_arg(argc, argv)) {
            return forward_to_ironc(argc, argv);
        }

        /* No file arg: package mode */
        return cmd_package(cmd, argc, argv);
    }

    /* Unknown command */
    fprintf(stderr, "iron: unknown command '%s'\n\n", cmd);
    print_help(colors);
    return 1;
}
