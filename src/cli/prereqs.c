/* prereqs.c: see prereqs.h. */
#include "cli/prereqs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "cli/toolchain.h"
#include "util/os.h"

#ifndef _WIN32
  #include <spawn.h>
  #include <sys/wait.h>
  extern char **environ;
#endif

/* ── Capturing the compiler's stderr ───────────────────────────────────── */

bool iron_cc_capture_open(IronCcCapture *c) {
    memset(c, 0, sizeof *c);
    c->fd = -1;
#ifdef _WIN32
    char dir[MAX_PATH];
    if (!GetTempPathA(sizeof dir, dir)) return false;
    if (!GetTempFileNameA(dir, "irn", 0, c->path)) return false;
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };   /* inheritable */
    HANDLE h = CreateFileA(c->path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    c->handle = h;
    return true;
#else
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(c->path, sizeof c->path, "%s/iron-cc-XXXXXX", tmp);
    c->fd = mkstemp(c->path);
    return c->fd >= 0;
#endif
}

/* The platform's installer for the missing piece, as argv, or NULL. */
static const char *const *installer_argv(const char **summary, const char **shown) {
#ifdef _WIN32
    static const char *const argv[] = {
        "winget", "install", "--id", "Microsoft.VisualStudio.2022.BuildTools", "-e",
        "--accept-package-agreements", "--accept-source-agreements",
        "--override", "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended",
        NULL };
    *summary = "the Visual Studio Build Tools (MSVC and the Windows SDK)";
    *shown = "winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override \"--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\"";
    return argv;
#elif defined(__APPLE__)
    static const char *const argv[] = { "xcode-select", "--install", NULL };
    *summary = "the Xcode command line tools (the macOS SDK)";
    *shown = "xcode-select --install";
    return argv;
#else
    struct stat st;
    *summary = "the C library development headers";
    if (stat("/usr/bin/apt-get", &st) == 0) {
        static const char *const a[] = { "sudo", "apt-get", "install", "-y", "libc6-dev", NULL };
        *shown = "sudo apt-get install -y libc6-dev"; return a;
    }
    if (stat("/usr/bin/dnf", &st) == 0) {
        static const char *const a[] = { "sudo", "dnf", "install", "-y", "glibc-devel", NULL };
        *shown = "sudo dnf install -y glibc-devel"; return a;
    }
    if (stat("/usr/bin/pacman", &st) == 0) {
        static const char *const a[] = { "sudo", "pacman", "-S", "--needed", "--noconfirm", "glibc", NULL };
        *shown = "sudo pacman -S --needed glibc"; return a;
    }
    if (stat("/usr/bin/zypper", &st) == 0) {
        static const char *const a[] = { "sudo", "zypper", "install", "-y", "glibc-devel", NULL };
        *shown = "sudo zypper install -y glibc-devel"; return a;
    }
    if (stat("/sbin/apk", &st) == 0) {
        static const char *const a[] = { "sudo", "apk", "add", "musl-dev", NULL };
        *shown = "sudo apk add musl-dev"; return a;
    }
    *shown = "your distribution's C library development package (libc6-dev, glibc-devel)";
    return NULL;
#endif
}

/* Does the compiler output describe a missing platform SDK or C library,
 * as opposed to an ordinary error in the generated C? */
static bool looks_like_missing_platform(const char *out) {
    static const char *const marks[] = {
        /* headers */
        "'stdio.h' file not found", "'stdlib.h' file not found", "'string.h' file not found",
        "'stdint.h' file not found", "'errno.h' file not found", "'math.h' file not found",
        "'windows.h' file not found", "'corecrt.h' file not found", "'vcruntime.h' file not found",
        /* Windows toolchain discovery and link inputs */
        "unable to find a Visual Studio installation", "could not open 'libcmt.lib'",
        "could not open 'kernel32.lib'", "could not open 'ucrt.lib'", "could not open 'msvcrt.lib'",
        "LNK1104", "LNK1181",
        /* macOS SDK */
        "no such sysroot directory", "invalid active developer path", "library 'System' not found",
        "library not found for -lSystem", "unable to find utility \"clang\"",
        /* Linux crt and libc */
        "cannot find crt1.o", "cannot find crti.o", "cannot find -lc:", "cannot find -lc\n",
        "cannot find Scrt1.o", "unable to find library -lc",
        NULL };
    for (int i = 0; marks[i]; i++)
        if (strstr(out, marks[i])) return true;
    return false;
}

static bool interactive(void) {
    const char *ni = getenv("IRON_NONINTERACTIVE");
    if (ni && *ni && strcmp(ni, "0") != 0) return false;
    return isatty(STDIN_FILENO) && isatty(STDERR_FILENO);
}

/* Name the missing piece, and offer to install it. */
static void explain_missing_platform(void) {
    const char *summary = NULL, *shown = NULL;
    const char *const *argv = installer_argv(&summary, &shown);
    fprintf(stderr,
        "\niron: the C compiler could not find the platform's C library.\n"
        "      Iron brings its own compiler (toolchain llvm %s), but compiling\n"
        "      a program still needs %s.\n",
        IRON_TOOLCHAIN_LLVM, summary);
    if (!argv) {
        fprintf(stderr, "      Install %s and run the build again.\n", shown);
        return;
    }
    if (interactive()) {
        fprintf(stderr, "      Install it now with\n        %s\n      [y/N] ", shown);
        fflush(stderr);
        char answer[16] = {0};
        if (fgets(answer, sizeof answer, stdin) && (answer[0] == 'y' || answer[0] == 'Y')) {
            int rc = iron_toolchain_run((char *const *)argv);
            if (rc == 0)
                fprintf(stderr, "iron: installed. Run the build again (a new terminal may be needed on Windows).\n");
            else if (rc < 0)
                fprintf(stderr, "iron: could not start the installer; run it yourself:\n        %s\n", shown);
            else
                fprintf(stderr, "iron: the installer exited with status %d; run it yourself:\n        %s\n", rc, shown);
            return;
        }
    }
    fprintf(stderr, "      Install it with\n        %s\n      and run the build again.\n", shown);
}

bool iron_cc_capture_finish(IronCcCapture *c, bool cc_failed) {
#ifdef _WIN32
    if (c->handle) { CloseHandle((HANDLE)c->handle); c->handle = NULL; }
#else
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
#endif
    if (!c->path[0]) return false;
    FILE *f = fopen(c->path, "rb");
    char *buf = NULL;
    size_t len = 0;
    if (f) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long n = ftell(f);
            if (n > 0 && fseek(f, 0, SEEK_SET) == 0) {
                buf = (char *)malloc((size_t)n + 1);
                if (buf) { len = fread(buf, 1, (size_t)n, f); buf[len] = '\0'; }
            }
        }
        fclose(f);
    }
    unlink(c->path);
    c->path[0] = '\0';
    bool explained = false;
    if (buf && len) {
        fwrite(buf, 1, len, stderr);
        if (cc_failed && looks_like_missing_platform(buf)) {
            explain_missing_platform();
            explained = true;
        }
    }
    free(buf);
    return explained;
}

/* ── iron toolchain check ──────────────────────────────────────────────── */

int iron_prereqs_check(void) {
    const IronToolchain *tc = iron_toolchain_get(true);
    if (!tc) return 1;
    char dir[4096], src[4096], out[4096];
    const char *tmp = getenv("TMPDIR");
#ifdef _WIN32
    if (!tmp || !*tmp) tmp = getenv("TEMP");
#endif
    if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(dir, sizeof dir, "%s/iron-check-XXXXXX", tmp);
#ifdef _WIN32
    if (_mktemp_s(dir, strlen(dir) + 1) != 0 || _mkdir(dir) != 0) {
#else
    if (!mkdtemp(dir)) {
#endif
        fprintf(stderr, "error: cannot create %s: %s\n", dir, strerror(errno));
        return 1;
    }
    snprintf(src, sizeof src, "%s/probe.c", dir);
    FILE *f = fopen(src, "w");
    if (!f) { fprintf(stderr, "error: cannot write %s\n", src); return 1; }
    fputs("#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
          "int main(void) { puts(\"ok\"); return 0; }\n", f);
    fclose(f);

    IronCcCapture cap;
    if (!iron_cc_capture_open(&cap)) { fprintf(stderr, "error: cannot capture the compiler output\n"); return 1; }
    int rc;
#ifdef _WIN32
    snprintf(out, sizeof out, "/Fe%s\\probe.exe", dir);
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "\"%s\" /nologo \"%s\" \"%s\"", iron_toolchain_tool(tc, "clang-cl"), src, out);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = (HANDLE)cap.handle;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        rc = -1;
    } else {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        rc = (int)code;
    }
#else
    snprintf(out, sizeof out, "%s/probe", dir);
    const char *argv[12];
    int ai = 0;
    argv[ai++] = iron_toolchain_tool(tc, "clang");
    if (iron_toolchain_sysroot()) { argv[ai++] = "-isysroot"; argv[ai++] = iron_toolchain_sysroot(); }
    argv[ai++] = src; argv[ai++] = "-o"; argv[ai++] = out; argv[ai] = NULL;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, cap.fd, STDERR_FILENO);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ) != 0) {
        rc = -1;
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    posix_spawn_file_actions_destroy(&fa);
#endif
    bool explained = iron_cc_capture_finish(&cap, rc != 0);
    unlink(src);
    unlink(out);
#ifdef _WIN32
    { char obj[4096]; snprintf(obj, sizeof obj, "probe.obj"); unlink(obj); }
#endif
    rmdir(dir);
    if (rc == 0) {
        printf("ok: the platform C library and linker are available (toolchain llvm %s bundle %s)\n",
               tc->llvm, tc->bundle);
        return 0;
    }
    if (!explained)
        fprintf(stderr, "iron: the probe program did not build (exit %d); see the compiler output above\n", rc);
    return 1;
}
