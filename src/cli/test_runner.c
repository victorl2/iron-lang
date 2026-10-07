#include "cli/test_runner.h"
#include "cli/build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "util/os.h"
#ifdef _WIN32
  #include <process.h>
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/wait.h>
  #include <spawn.h>
#endif

#ifndef _WIN32
extern char **environ;
#endif

/* ── ANSI color helpers (gated by isatty) ────────────────────────────────── */

#define COL_RED    "\033[31m"
#define COL_GREEN  "\033[32m"
#define COL_BLUE   "\033[34m"
#define COL_RESET  "\033[0m"

static int use_color = -1; /* -1 = uninitialised */

static void init_color(void) {
    if (use_color == -1) {
        use_color = isatty(STDOUT_FILENO) ? 1 : 0;
    }
}

static const char *color(const char *code) {
    return use_color ? code : "";
}

/* ── Simple dynamic array of strings ─────────────────────────────────────── */

typedef struct {
    char **items;
    int    count;
    int    capacity;
} StrVec;

static void strvec_push(StrVec *v, char *s) {
    if (v->count == v->capacity) {
        int new_cap = v->capacity == 0 ? 8 : v->capacity * 2;
        char **new_items = (char **)realloc(v->items,
                                            (size_t)new_cap * sizeof(char *));
        if (!new_items) return;
        v->items    = new_items;
        v->capacity = new_cap;
    }
    v->items[v->count++] = s;
}

static void strvec_free(StrVec *v) {
    for (int i = 0; i < v->count; i++) {
        free(v->items[i]);
    }
    free(v->items);
    v->items    = NULL;
    v->count    = 0;
    v->capacity = 0;
}

/* ── Compare function for qsort (alphabetical) ───────────────────────────── */

static int str_compare(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

/* ── Execute a binary and return its exit code ───────────────────────────── */

static int run_binary(const char *path) {
#ifdef _WIN32
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "\"%s\"", path);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
#else
    const char *argv[] = { path, NULL };
    pid_t pid;
    int status = posix_spawn(&pid, path, NULL, NULL,
                              (char *const *)argv, environ);
    if (status != 0) {
        return -1;
    }
    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(wstatus)) {
        return WEXITSTATUS(wstatus);
    }
    return -1;
#endif
}

/* Whether the file declares `func main(` at the start of a line. */
bool iron_file_defines_main(const char *path) {
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

/* ── iron_test ───────────────────────────────────────────────────────────── */

int iron_test(const char *dir_path) {
    init_color();

    if (!dir_path) dir_path = ".";

    /* 1. Open directory and collect test_*.iron files */
    DIR *d = opendir(dir_path);
    if (!d) {
        fprintf(stderr, "iron test: cannot open directory '%s': %s\n",
                dir_path, strerror(errno));
        return 1;
    }

    StrVec test_files = { NULL, 0, 0 };

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        /* Match files starting with "test_" and ending with ".iron" */
        if (strncmp(name, "test_", 5) != 0) continue;
        size_t nlen = strlen(name);
        if (nlen <= 5) continue;
        if (strcmp(name + nlen - 5, ".iron") != 0) continue;


        /* Build full path: dir_path + "/" + name */
        size_t dir_len  = strlen(dir_path);
        size_t name_len = nlen;
        char *full_path = (char *)malloc(dir_len + 1 + name_len + 1);
        if (!full_path) continue;
        memcpy(full_path, dir_path, dir_len);
        full_path[dir_len] = '/';
        memcpy(full_path + dir_len + 1, name, name_len + 1);

        /* A standalone test is a program with its own main; a file of
         * `test` blocks is run by iron_test_file instead. */
        if (!iron_file_defines_main(full_path)) { free(full_path); continue; }
        strvec_push(&test_files, full_path);
    }
    closedir(d);

    if (test_files.count == 0) {
        fprintf(stdout, "iron test: no test_*.iron files found in '%s'\n",
                dir_path);
        strvec_free(&test_files);
        return 0;
    }

    /* 2. Sort alphabetically for deterministic order */
    qsort(test_files.items, (size_t)test_files.count,
          sizeof(char *), str_compare);

    /* 3. Run each test */
    int pass_count = 0;
    int fail_count = 0;

    for (int i = 0; i < test_files.count; i++) {
        const char *file_path = test_files.items[i];

        /* Extract filename for display */
        const char *display_name = strrchr(file_path, '/');
        display_name = display_name ? display_name + 1 : file_path;

        printf("%s[RUN ]%s %s\n",
               color(COL_BLUE), color(COL_RESET), display_name);
        fflush(stdout);

        /* Build a temp binary path */
        char tmp_binary[512];
        const char *tmpdir = getenv("TMPDIR");
#ifdef _WIN32
        if (!tmpdir || !*tmpdir) tmpdir = getenv("TEMP");
        if (!tmpdir || !*tmpdir) tmpdir = getenv("TMP");
        if (!tmpdir || !*tmpdir) tmpdir = ".";
        snprintf(tmp_binary, sizeof(tmp_binary), "%s\\iron_test_%d_%d.exe",
                 tmpdir, (int)getpid(), i);
#else
        if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
        snprintf(tmp_binary, sizeof(tmp_binary), "%s/iron_test_%d_%d",
                 tmpdir, (int)getpid(), i);
#endif

        /* Compile the test file */
        IronBuildOpts opts = {
            .verbose      = false,
            .debug_build  = false,
            .run_after    = false,
            .run_args     = NULL,
            .run_arg_count = 0,
            .quiet        = true
        };
        int build_ret = iron_build(file_path, tmp_binary, opts);

        if (build_ret != 0) {
            printf("%s[FAIL]%s %s (compilation error)\n",
                   color(COL_RED), color(COL_RESET), display_name);
            fflush(stdout);
            fail_count++;
            continue;
        }

        /* Run the compiled binary */
        int exit_code = run_binary(tmp_binary);

        /* Remove temp binary */
        unlink(tmp_binary);

        if (exit_code == 0) {
            printf("%s[PASS]%s %s\n",
                   color(COL_GREEN), color(COL_RESET), display_name);
            fflush(stdout);
            pass_count++;
        } else {
            printf("%s[FAIL]%s %s (exit code %d)\n",
                   color(COL_RED), color(COL_RESET), display_name, exit_code);
            fflush(stdout);
            fail_count++;
        }
    }

    /* 4. Print summary */
    int total = pass_count + fail_count;
    printf("\nResults: %d passed, %d failed, %d total\n",
           pass_count, fail_count, total);

    strvec_free(&test_files);
    return (fail_count > 0) ? 1 : 0;
}

/* ── Test blocks: iron_test_file ─────────────────────────────────────────── */

typedef struct {
#ifdef _WIN32
    HANDLE proc;
#else
    pid_t  pid;
#endif
    bool   started;
} TestProc;

/* Starts `bin <a1> <a2>` with stdout and stderr written to out_path. */
static bool start_capture(TestProc *tp, const char *bin, const char *a1,
                          const char *a2, const char *out_path) {
    tp->started = false;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE h = CreateFileA(out_path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "\"%s\" %s %s", bin, a1, a2 ? a2 : "");
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = h;
    si.hStdError = h;
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    CloseHandle(h);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    tp->proc = pi.hProcess;
#else
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    char *argv[] = { (char *)bin, (char *)a1, (char *)a2, NULL };
    int rc = posix_spawn(&tp->pid, bin, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return false;
#endif
    tp->started = true;
    return true;
}

/* Waits for the process; returns its exit code, or -1 when it died by a
 * signal (an assertion or panic aborts) or could not be waited for. */
static int finish_capture(TestProc *tp) {
    if (!tp->started) return -1;
#ifdef _WIN32
    WaitForSingleObject(tp->proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(tp->proc, &code);
    CloseHandle(tp->proc);
    return (int)code;
#else
    int st = 0;
    if (waitpid(tp->pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

static char *slurp_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(n > 0 ? (size_t)n + 1 : 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = n > 0 ? fread(buf, 1, (size_t)n, f) : 0;
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static int cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    return (int)n;
}

int iron_test_file(const char *source, const char *filter) {
    init_color();
    const char *tmpdir = getenv("TMPDIR");
#ifdef _WIN32
    if (!tmpdir || !*tmpdir) tmpdir = getenv("TEMP");
    if (!tmpdir || !*tmpdir) tmpdir = ".";
    const char *exe_ext = ".exe";
#else
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    const char *exe_ext = "";
#endif
    char bin[1024], list_out[1024];
    snprintf(bin, sizeof(bin), "%s/iron_tests_%d%s", tmpdir, (int)getpid(), exe_ext);
    snprintf(list_out, sizeof(list_out), "%s/iron_tests_%d.list", tmpdir, (int)getpid());

    IronBuildOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.test_mode = true;
    opts.strict_v3 = true;
    opts.quiet = true;
    if (iron_build(source, bin, opts) != 0) {
        printf("%s[FAIL]%s %s (compilation error)\n", color(COL_RED), color(COL_RESET), source);
        return 1;
    }

    /* The test names, one per line. */
    TestProc lp;
    char *names_text = NULL;
    if (start_capture(&lp, bin, "--iron-list", NULL, list_out) && finish_capture(&lp) == 0)
        names_text = slurp_file(list_out);
    unlink(list_out);
    if (!names_text) {
        fprintf(stderr, "iron test: could not list the tests of %s\n", source);
        unlink(bin);
        return 1;
    }
    StrVec names = { NULL, 0, 0 };
    for (char *line = names_text; *line; ) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);
        if (len > 0 && line[len - 1] == '\r') len--;
        char *name = (char *)malloc(len + 1);
        if (name) { memcpy(name, line, len); name[len] = '\0'; strvec_push(&names, name); }
        if (!nl) break;
        line = nl + 1;
    }
    free(names_text);

    int total = names.count;
    int *selected = (int *)calloc(total > 0 ? (size_t)total : 1, sizeof(int));
    int nsel = 0;
    for (int i = 0; i < total; i++)
        if (!filter || strstr(names.items[i], filter)) selected[nsel++] = i;

    if (nsel == 0) {
        printf("%s\n", total == 0 ? "no tests found" : "no test matches the filter");
        strvec_free(&names);
        free(selected);
        unlink(bin);
        return 0;
    }

    /* Run them in batches of one per CPU, each in its own process, so an
     * assertion or panic fails only its test. */
    int *codes = (int *)calloc((size_t)nsel, sizeof(int));
    char (*outs)[1024] = calloc((size_t)nsel, sizeof(*outs));
    if (!codes || !outs) { free(codes); free(outs); free(selected); strvec_free(&names); unlink(bin); return 1; }
    int width = cpu_count();
    for (int base = 0; base < nsel; base += width) {
        TestProc procs[16];
        int n = nsel - base < width ? nsel - base : width;
        for (int k = 0; k < n; k++) {
            int s = base + k;
            char idx[32];
            snprintf(idx, sizeof(idx), "%d", selected[s]);
            snprintf(outs[s], sizeof(outs[s]), "%s/iron_tests_%d_%d.out", tmpdir, (int)getpid(), s);
            start_capture(&procs[k], bin, "--iron-test", idx, outs[s]);
        }
        for (int k = 0; k < n; k++) codes[base + k] = finish_capture(&procs[k]);
    }

    int passed = 0, failed = 0;
    for (int s = 0; s < nsel; s++) {
        const char *name = names.items[selected[s]];
        if (codes[s] == 0) {
            printf("test %s ... %sok%s\n", name, color(COL_GREEN), color(COL_RESET));
            passed++;
        } else {
            printf("test %s ... %sFAILED%s\n", name, color(COL_RED), color(COL_RESET));
            char *out = slurp_file(outs[s]);
            if (out && *out) {
                for (char *line = out; *line; ) {
                    char *nl = strchr(line, '\n');
                    if (nl) *nl = '\0';
                    printf("    %s\n", line);
                    if (!nl) break;
                    line = nl + 1;
                }
            }
            free(out);
            failed++;
        }
        unlink(outs[s]);
    }
    printf("\n%d passed, %d failed", passed, failed);
    if (nsel < total) printf(", %d filtered out", total - nsel);
    printf("\n");
    fflush(stdout);

    free(codes);
    free(outs);
    free(selected);
    strvec_free(&names);
    unlink(bin);
    return failed > 0 ? 1 : 0;
}
