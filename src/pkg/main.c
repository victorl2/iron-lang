/*
 * iron - The Iron package manager
 *
 * Discovers and invokes `ironc` for single-file workflows.
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

#ifndef IRON_VERSION_STRING
#define IRON_VERSION_STRING "0.0.2"
#endif
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
static char *find_ironc(void) {
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

/* ── Subprocess invocation ──────────────────────────────────────────────── */

/*
 * forward_to_ironc: spawn ironc with argv[1..] forwarded verbatim.
 * Returns ironc's exit code.
 */
static int forward_to_ironc(int argc, char **argv) {
    char *ironc_path = find_ironc();

#ifdef _WIN32
    /* Build command line string for CreateProcess */
    char cmd[32768];
    int pos = snprintf(cmd, sizeof(cmd), "\"%s\"", ironc_path);
    for (int i = 1; i < argc && pos < (int)sizeof(cmd) - 1; i++) {
        pos += snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, " \"%s\"", argv[i]);
    }

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "error: failed to spawn ironc (error %lu)\n", GetLastError());
        free(ironc_path);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    free(ironc_path);
    return (int)exit_code;
#else
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

    pid_t pid;
    int status = posix_spawnp(&pid, ironc_path, NULL, NULL,
                               (char *const *)new_argv, environ);
    free(new_argv);
    if (status != 0) {
        fprintf(stderr, "error: failed to spawn ironc: %s\n", strerror(status));
        free(ironc_path);
        return 1;
    }
    free(ironc_path);

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

/* ── Help and version ───────────────────────────────────────────────────── */

static void print_version(void) {
    printf("iron %s (%s %s)\n",
           IRON_VERSION_STRING, IRON_GIT_HASH, IRON_BUILD_DATE);
}

static void print_help(void) {
    print_version();
    printf("The Iron package manager\n\n");
    printf("Usage: iron <command> [options] [args]\n\n");
    printf("Package Commands:\n");
    printf("  build    Build the current package or a .iron file\n");
    printf("  run      Run the current package or a .iron file\n");
    printf("  check    Type-check without producing a binary\n");
    printf("  fmt      Format Iron source code\n");
    printf("  test     Discover and run tests\n");
    printf("\nSee also: ironc -- raw compiler for direct file compilation\n");
    printf("\nOptions:\n");
    printf("  --version    Print version and exit\n");
    printf("  --help       Print this help\n");
}

/* ── Entry point ────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    /* No args: show help (exit 0 — Cargo-style, unlike ironc which exits 1) */
    if (argc < 2) {
        print_help();
        return 0;
    }

    const char *cmd = argv[1];

    /* Global flags */
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        print_version();
        return 0;
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_help();
        return 0;
    }

    /* Known subcommands */
    if (strcmp(cmd, "build") == 0 || strcmp(cmd, "run") == 0 ||
        strcmp(cmd, "check") == 0 || strcmp(cmd, "fmt") == 0 ||
        strcmp(cmd, "test") == 0) {

        /* If a .iron file argument is present, forward to ironc silently */
        if (has_iron_file_arg(argc, argv)) {
            return forward_to_ironc(argc, argv);
        }

        /* No file arg: package mode (Phase 13 will implement full iron.toml support) */
        fprintf(stderr, "error: no iron.toml found in current directory\n");
        fprintf(stderr, "hint: run 'iron init' to create a new project, or pass a .iron file\n");
        return 1;
    }

    /* init subcommand — stub for Phase 13 */
    if (strcmp(cmd, "init") == 0) {
        fprintf(stderr, "error: 'iron init' is not yet implemented\n");
        return 1;
    }

    /* Unknown command */
    fprintf(stderr, "iron: unknown command '%s'\n\n", cmd);
    print_help();
    return 1;
}
