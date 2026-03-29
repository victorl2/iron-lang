#include "cli/build.h"
#include "cli/toml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
  #include <direct.h>  /* _mkdir */
#else
  #include <unistd.h>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <libgen.h>
#endif
#ifdef __APPLE__
  #include <mach-o/dyld.h>
#endif

/* IRON_SOURCE_DIR is injected by CMake at build time — absolute path to src/ */
#ifndef IRON_SOURCE_DIR
#define IRON_SOURCE_DIR "src"
#endif

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "analyzer/analyzer.h"
#include "ir/lower.h"
#include "ir/emit_c.h"
#include "ir/ir_optimize.h"
#include "ir/print.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"

#ifndef _WIN32
extern char **environ;
#endif

/* ── Windows compatibility shims ─────────────────────────────────────────── */
#ifdef _WIN32
/* basename: return pointer to last path component (no allocation) */
static const char *win_basename(const char *path) {
    const char *p = path;
    const char *last = path;
    while (*p) {
        if (*p == '/' || *p == '\\') last = p + 1;
        p++;
    }
    return last;
}
/* dirname: copy directory part into a static buffer */
static char *win_dirname(char *path) {
    char *p = path + strlen(path);
    while (p > path && *p != '/' && *p != '\\') p--;
    if (p == path) {
        path[0] = '.';
        path[1] = '\0';
    } else {
        *p = '\0';
    }
    return path;
}
#define basename(p)  win_basename(p)
#define dirname(p)   win_dirname(p)
#endif

/* ── Runtime path resolution ─────────────────────────────────────────────── */

/* resolve_self_dir: fills buf with the directory containing this binary.
   Returns 0 on success, -1 on error. */
static int resolve_self_dir(char *buf, size_t buf_size) {
#ifdef __APPLE__
    uint32_t size = (uint32_t)buf_size;
    if (_NSGetExecutablePath(buf, &size) != 0) return -1;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", buf, buf_size - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)buf_size);
    if (n == 0 || n >= (DWORD)buf_size) return -1;
#else
    return -1;
#endif
    /* Truncate at last path separator to get directory */
    char *last = strrchr(buf, '/');
#ifdef _WIN32
    char *last_win = strrchr(buf, '\\');
    if (last_win > last) last = last_win;
#endif
    if (last) *last = '\0';
    return 0;
}

/* get_iron_lib_dir: returns malloc'd path to the lib/ or src/ base directory.
   For installed builds: resolves to sibling ../lib/ of the binary directory.
   For dev builds: falls back to IRON_SOURCE_DIR compile-time define.
   Caller must free(). */
static char *get_iron_lib_dir(void) {
    char self_dir[4096];
    if (resolve_self_dir(self_dir, sizeof(self_dir)) == 0) {
        /* Try sibling ../lib/ directory (installed layout) */
        size_t dlen = strlen(self_dir);
        /* Truncate to parent dir (go up from bin/) */
        char *parent_slash = strrchr(self_dir, '/');
#ifdef _WIN32
        char *parent_slash_win = strrchr(self_dir, '\\');
        if (parent_slash_win > parent_slash) parent_slash = parent_slash_win;
#endif
        if (parent_slash) {
            *parent_slash = '\0';
            dlen = strlen(self_dir);
        }
        size_t lib_len = dlen + strlen("/lib") + 1;
        char *lib_path = (char *)malloc(lib_len);
        if (lib_path) {
            snprintf(lib_path, lib_len, "%s/lib", self_dir);
            struct stat st;
            if (stat(lib_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                return lib_path;
            }
            free(lib_path);
        }
    }
    /* Fallback: compile-time IRON_SOURCE_DIR (dev builds) */
    return strdup(IRON_SOURCE_DIR);
}

/* ── Helper: read a file into a heap-allocated string ────────────────────── */

static char *read_file(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "error: cannot seek '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)(size + 1));
    if (!buf) {
        fclose(f);
        fprintf(stderr, "error: out of memory reading '%s'\n", path);
        return NULL;
    }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';
    if (out_size) *out_size = (long)read;
    return buf;
}

/* ── Helper: derive output binary name from source path ──────────────────── */

static char *derive_output_name(const char *source_path) {
    /* Make a mutable copy for basename calls */
    char *path_copy = strdup(source_path);
    if (!path_copy) return NULL;

#ifdef _WIN32
    const char *base = win_basename(path_copy);
#else
    char *base = basename(path_copy);
#endif

    /* Strip .iron extension if present */
    size_t len = strlen(base);
    const char *ext = ".iron";
    size_t ext_len = strlen(ext);
    char *out;
    if (len > ext_len && strcmp(base + len - ext_len, ext) == 0) {
        out = (char *)malloc(len - ext_len + 1);
        if (out) {
            memcpy(out, base, len - ext_len);
            out[len - ext_len] = '\0';
        }
    } else {
        out = strdup(base);
    }
    free(path_copy);

#ifdef _WIN32
    /* Append .exe extension on Windows */
    if (out) {
        size_t out_len = strlen(out);
        char *out_exe = (char *)malloc(out_len + 5); /* +4 for ".exe" + nul */
        if (out_exe) {
            memcpy(out_exe, out, out_len);
            memcpy(out_exe + out_len, ".exe", 5);
            free(out);
            out = out_exe;
        }
    }
#endif

    return out;
}

/* ── Helper: write C source to a temp file ───────────────────────────────── */

static char *write_temp_c(const char *c_src, bool debug_build,
                           const char *binary_name) {
    char *path = NULL;

    if (debug_build) {
        /* Use .iron-build/ directory */
#ifdef _WIN32
        if (_mkdir(".iron-build") != 0 && errno != EEXIST) {
#else
        if (mkdir(".iron-build", 0755) != 0 && errno != EEXIST) {
#endif
            fprintf(stderr, "error: cannot create .iron-build/: %s\n",
                    strerror(errno));
            return NULL;
        }
        size_t plen = strlen(binary_name) + 32;
        path = (char *)malloc(plen);
        if (!path) return NULL;
        snprintf(path, plen, ".iron-build/%s.c", binary_name);

        FILE *f = fopen(path, "w");
        if (!f) {
            fprintf(stderr, "error: cannot create temp file: %s\n",
                    strerror(errno));
            free(path);
            return NULL;
        }
        size_t total = strlen(c_src);
        if (fwrite(c_src, 1, total, f) != total) {
            fprintf(stderr, "error: write failed: %s\n", strerror(errno));
            fclose(f);
            free(path);
            return NULL;
        }
        fclose(f);
        return path;
    }

#ifdef _WIN32
    /* Windows: use GetTempPath + GetTempFileName */
    char tmp_dir[MAX_PATH];
    DWORD dir_len = GetTempPathA(MAX_PATH, tmp_dir);
    if (dir_len == 0) {
        fprintf(stderr, "error: cannot get temp dir\n");
        return NULL;
    }
    char tmp_file[MAX_PATH];
    if (GetTempFileNameA(tmp_dir, "iron", 0, tmp_file) == 0) {
        fprintf(stderr, "error: cannot create temp file\n");
        return NULL;
    }
    /* GetTempFileName creates a .tmp file; rename to .c */
    size_t base_len = strlen(tmp_file);
    path = (char *)malloc(base_len + 3); /* +2 for ".c" + nul */
    if (!path) return NULL;
    /* Replace last 4 chars (.tmp) with .c */
    if (base_len > 4) {
        memcpy(path, tmp_file, base_len - 4);
        memcpy(path + base_len - 4, ".c", 3);
    } else {
        memcpy(path, tmp_file, base_len);
        memcpy(path + base_len, ".c", 3);
    }
    DeleteFileA(tmp_file); /* remove the .tmp placeholder */

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "error: cannot create temp file '%s': %s\n",
                path, strerror(errno));
        free(path);
        return NULL;
    }
    size_t total = strlen(c_src);
    if (fwrite(c_src, 1, total, f) != total) {
        fprintf(stderr, "error: write failed: %s\n", strerror(errno));
        fclose(f);
        free(path);
        return NULL;
    }
    fclose(f);
    return path;
#else
    /* Unix: use /tmp with mkstemps */
    path = strdup("/tmp/iron_XXXXXX.c");
    if (!path) return NULL;
    int fd = mkstemps(path, 2); /* 2 = length of ".c" suffix */

    if (fd < 0) {
        fprintf(stderr, "error: cannot create temp file: %s\n",
                strerror(errno));
        free(path);
        return NULL;
    }

    size_t total = strlen(c_src);
    size_t written = 0;
    while (written < total) {
        ssize_t n = write(fd, c_src + written, total - written);
        if (n < 0) {
            fprintf(stderr, "error: write failed: %s\n", strerror(errno));
            close(fd);
            free(path);
            return NULL;
        }
        written += (size_t)n;
    }
    close(fd);
    return path;
#endif
}

/* ── Helper: build a path from a base directory and relative path ─────────── */

static char *make_path(const char *base, const char *rel) {
    size_t base_len = strlen(base);
    size_t rel_len  = strlen(rel);
    char *out = (char *)malloc(base_len + 1 + rel_len + 1);
    if (!out) return NULL;
    memcpy(out, base, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, rel, rel_len + 1);
    return out;
}

/* ── Helper: invoke clang to compile generated C with runtime sources ──────── */

/* Build the source file list shared by both Unix and Windows invoke helpers */
static int build_src_list(const char **argv_buf, int *ai_out,
                           const char *c_file, const char *output,
                           char **src_i_flag_out, char **vendor_i_flag_out,
                           char **stdlib_i_flag_out,
                           char **rt_stb_out, char **rt_arena_out,
                           char **rt_strbuf_out, char **rt_string_out,
                           char **rt_rc_out, char **rt_builtin_out,
                           char **rt_threads_out, char **rt_collect_out,
                           char **sl_math_out, char **sl_io_out,
                           char **sl_time_out, char **sl_log_out,
                           IronBuildOpts opts,
                           char **rl_src_out, char **rl_i_flag_out,
                           char **base_dir_out) {
    /* Resolve runtime base directory at runtime */
    char *base_dir = get_iron_lib_dir();
    if (!base_dir) return 1;
    *base_dir_out = base_dir;

    /* Build -I flag for headers */
    size_t src_i_len = strlen("-I") + strlen(base_dir) + 1;
    char *src_i_flag = (char *)malloc(src_i_len);
    if (!src_i_flag) return 1;
    snprintf(src_i_flag, src_i_len, "-I%s", base_dir);
    *src_i_flag_out = src_i_flag;

    /* Also add vendor dir for stb_ds.h */
    size_t vendor_i_len = strlen("-I") + strlen(base_dir) + strlen("/vendor") + 1;
    char *vendor_i_flag = (char *)malloc(vendor_i_len);
    if (!vendor_i_flag) return 1;
    snprintf(vendor_i_flag, vendor_i_len, "-I%s/vendor", base_dir);
    *vendor_i_flag_out = vendor_i_flag;

    /* Also add stdlib dir so stdlib *.c files can find their own *.h headers */
    size_t stdlib_i_len = strlen("-I") + strlen(base_dir) + strlen("/stdlib") + 1;
    char *stdlib_i_flag = (char *)malloc(stdlib_i_len);
    if (!stdlib_i_flag) return 1;
    snprintf(stdlib_i_flag, stdlib_i_len, "-I%s/stdlib", base_dir);
    *stdlib_i_flag_out = stdlib_i_flag;

    *rt_stb_out     = make_path(base_dir, "util/stb_ds_impl.c");
    *rt_arena_out   = make_path(base_dir, "util/arena.c");
    *rt_strbuf_out  = make_path(base_dir, "util/strbuf.c");
    *rt_string_out  = make_path(base_dir, "runtime/iron_string.c");
    *rt_rc_out      = make_path(base_dir, "runtime/iron_rc.c");
    *rt_builtin_out = make_path(base_dir, "runtime/iron_builtins.c");
    *rt_threads_out = make_path(base_dir, "runtime/iron_threads.c");
    *rt_collect_out = make_path(base_dir, "runtime/iron_collections.c");
    *sl_math_out    = make_path(base_dir, "stdlib/iron_math.c");
    *sl_io_out      = make_path(base_dir, "stdlib/iron_io.c");
    *sl_time_out    = make_path(base_dir, "stdlib/iron_time.c");
    *sl_log_out     = make_path(base_dir, "stdlib/iron_log.c");

    if (!*rt_stb_out || !*rt_arena_out || !*rt_strbuf_out || !*rt_string_out ||
        !*rt_rc_out || !*rt_builtin_out || !*rt_threads_out || !*rt_collect_out ||
        !*sl_math_out || !*sl_io_out || !*sl_time_out || !*sl_log_out) {
        return 1;
    }

    if (opts.use_raylib) {
        *rl_src_out = make_path(base_dir, "vendor/raylib/raylib.c");
        size_t rl_i_len = strlen("-I") + strlen(base_dir) + strlen("/vendor/raylib") + 1;
        char *rl_i_flag = (char *)malloc(rl_i_len);
        if (!rl_i_flag) return 1;
        snprintf(rl_i_flag, rl_i_len, "-I%s/vendor/raylib", base_dir);
        *rl_i_flag_out = rl_i_flag;
    }

    int ai = 0;
#ifdef _WIN32
    argv_buf[ai++] = "clang-cl";
    argv_buf[ai++] = "/std:c11";
    argv_buf[ai++] = "/O2";
    argv_buf[ai++] = c_file;
    argv_buf[ai++] = *rt_stb_out;
    argv_buf[ai++] = *rt_arena_out;
    argv_buf[ai++] = *rt_strbuf_out;
    argv_buf[ai++] = *rt_string_out;
    argv_buf[ai++] = *rt_rc_out;
    argv_buf[ai++] = *rt_builtin_out;
    argv_buf[ai++] = *rt_threads_out;
    argv_buf[ai++] = *rt_collect_out;
    argv_buf[ai++] = *sl_math_out;
    argv_buf[ai++] = *sl_io_out;
    argv_buf[ai++] = *sl_time_out;
    argv_buf[ai++] = *sl_log_out;
    argv_buf[ai++] = src_i_flag;
    argv_buf[ai++] = vendor_i_flag;
    argv_buf[ai++] = stdlib_i_flag;
    /* Output flag for clang-cl */
    {
        static char out_flag[1024];
        snprintf(out_flag, sizeof(out_flag), "/Fe%s", output);
        argv_buf[ai++] = out_flag;
    }
#else
    argv_buf[ai++] = "clang";
    argv_buf[ai++] = "-std=gnu17";
    argv_buf[ai++] = "-O3";
    argv_buf[ai++] = "-o";
    argv_buf[ai++] = output;
    argv_buf[ai++] = c_file;
    argv_buf[ai++] = *rt_stb_out;
    argv_buf[ai++] = *rt_arena_out;
    argv_buf[ai++] = *rt_strbuf_out;
    argv_buf[ai++] = *rt_string_out;
    argv_buf[ai++] = *rt_rc_out;
    argv_buf[ai++] = *rt_builtin_out;
    argv_buf[ai++] = *rt_threads_out;
    argv_buf[ai++] = *rt_collect_out;
    argv_buf[ai++] = *sl_math_out;
    argv_buf[ai++] = *sl_io_out;
    argv_buf[ai++] = *sl_time_out;
    argv_buf[ai++] = *sl_log_out;
    argv_buf[ai++] = src_i_flag;
    argv_buf[ai++] = vendor_i_flag;
    argv_buf[ai++] = stdlib_i_flag;
    argv_buf[ai++] = "-lm";
    argv_buf[ai++] = "-lpthread";
#endif

    /* Raylib-specific args */
    if (opts.use_raylib && *rl_src_out) {
        argv_buf[ai++] = *rl_src_out;
        argv_buf[ai++] = *rl_i_flag_out;
        argv_buf[ai++] = "-DPLATFORM_DESKTOP";
#ifdef __APPLE__
        argv_buf[ai++] = "-framework";
        argv_buf[ai++] = "OpenGL";
        argv_buf[ai++] = "-framework";
        argv_buf[ai++] = "Cocoa";
        argv_buf[ai++] = "-framework";
        argv_buf[ai++] = "IOKit";
        argv_buf[ai++] = "-framework";
        argv_buf[ai++] = "CoreVideo";
#elif defined(__linux__)
        argv_buf[ai++] = "-lGL";
        argv_buf[ai++] = "-ldl";
        argv_buf[ai++] = "-lrt";
#endif
    }
    argv_buf[ai] = NULL;
    *ai_out = ai;
    return 0;
}

static void free_src_list(char *base_dir,
                           char *src_i_flag, char *vendor_i_flag,
                           char *stdlib_i_flag,
                           char *rt_stb, char *rt_arena, char *rt_strbuf,
                           char *rt_string, char *rt_rc, char *rt_builtin,
                           char *rt_threads, char *rt_collect,
                           char *sl_math, char *sl_io, char *sl_time,
                           char *sl_log, char *rl_src, char *rl_i_flag) {
    free(base_dir);
    free(src_i_flag); free(vendor_i_flag); free(stdlib_i_flag);
    free(rt_stb); free(rt_arena); free(rt_strbuf);
    free(rt_string); free(rt_rc); free(rt_builtin);
    free(rt_threads); free(rt_collect);
    free(sl_math); free(sl_io); free(sl_time); free(sl_log);
    free(rl_src); free(rl_i_flag);
}

static int invoke_clang(const char *c_file, const char *output,
                         const char *src_dir, IronBuildOpts opts) {
    (void)src_dir;

    char *base_dir = NULL;
    char *src_i_flag = NULL, *vendor_i_flag = NULL, *stdlib_i_flag = NULL;
    char *rt_stb = NULL, *rt_arena = NULL, *rt_strbuf = NULL;
    char *rt_string = NULL, *rt_rc = NULL, *rt_builtin = NULL;
    char *rt_threads = NULL, *rt_collect = NULL;
    char *sl_math = NULL, *sl_io = NULL, *sl_time = NULL, *sl_log = NULL;
    char *rl_src = NULL, *rl_i_flag = NULL;

    const char *argv_buf[96];
    int ai = 0;

    if (build_src_list(argv_buf, &ai, c_file, output,
                       &src_i_flag, &vendor_i_flag, &stdlib_i_flag,
                       &rt_stb, &rt_arena, &rt_strbuf,
                       &rt_string, &rt_rc, &rt_builtin,
                       &rt_threads, &rt_collect,
                       &sl_math, &sl_io, &sl_time, &sl_log,
                       opts, &rl_src, &rl_i_flag, &base_dir) != 0) {
        free_src_list(base_dir, src_i_flag, vendor_i_flag, stdlib_i_flag,
                      rt_stb, rt_arena, rt_strbuf,
                      rt_string, rt_rc, rt_builtin,
                      rt_threads, rt_collect,
                      sl_math, sl_io, sl_time, sl_log,
                      rl_src, rl_i_flag);
        return 1;
    }

#ifdef _WIN32
    /* Build a single command-line string for CreateProcess */
    char cmd[32768];
    int pos = 0;
    for (int i = 0; i < ai && argv_buf[i]; i++) {
        if (i > 0) cmd[pos++] = ' ';
        /* Quote args containing spaces */
        int has_space = (strchr(argv_buf[i], ' ') != NULL);
        if (has_space) cmd[pos++] = '"';
        size_t arglen = strlen(argv_buf[i]);
        memcpy(cmd + pos, argv_buf[i], arglen);
        pos += (int)arglen;
        if (has_space) cmd[pos++] = '"';
    }
    cmd[pos] = '\0';

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    free_src_list(base_dir, src_i_flag, vendor_i_flag, stdlib_i_flag,
                  rt_stb, rt_arena, rt_strbuf,
                  rt_string, rt_rc, rt_builtin,
                  rt_threads, rt_collect,
                  sl_math, sl_io, sl_time, sl_log,
                  rl_src, rl_i_flag);

    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "error: failed to spawn clang-cl (error %lu)\n",
                GetLastError());
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exit_code != 0) {
        fprintf(stderr, "error: clang-cl exited with code %lu\n", exit_code);
        return 1;
    }
    return 0;
#else
    pid_t pid;
    int status = posix_spawnp(&pid, "clang", NULL, NULL,
                               (char *const *)argv_buf, environ);

    free_src_list(base_dir, src_i_flag, vendor_i_flag, stdlib_i_flag,
                  rt_stb, rt_arena, rt_strbuf,
                  rt_string, rt_rc, rt_builtin,
                  rt_threads, rt_collect,
                  sl_math, sl_io, sl_time, sl_log,
                  rl_src, rl_i_flag);

    if (status != 0) {
        fprintf(stderr, "error: failed to spawn clang: %s\n",
                strerror(status));
        return 1;
    }
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "error: waitpid failed: %s\n", strerror(errno));
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        fprintf(stderr, "error: clang exited with code %d\n", code);
        return 1;
    }
    return 0;
#endif
}

/* ── Main build function ─────────────────────────────────────────────────── */

int iron_build(const char *source_path, const char *output_path,
               IronBuildOpts opts) {
    /* Resolve runtime lib/src base directory once for this build */
    char *base_dir = get_iron_lib_dir();
    if (!base_dir) return 1;

    /* 1. Read source file */
    long src_size = 0;
    char *source = read_file(source_path, &src_size);
    if (!source) { free(base_dir); return 1; }

    /* 1b. Check iron.toml for raylib = true */
    {
        /* Look for iron.toml in the same directory as the source file */
        char *src_copy = strdup(source_path);
        if (src_copy) {
            char *dir = dirname(src_copy);
            size_t toml_len = strlen(dir) + strlen("/iron.toml") + 1;
            char *toml_path = (char *)malloc(toml_len);
            if (toml_path) {
                snprintf(toml_path, toml_len, "%s/iron.toml", dir);
                IronProject *proj = iron_toml_parse(toml_path);
                if (proj) {
                    if (proj->raylib) opts.use_raylib = true;
                    iron_toml_free(proj);
                }
                free(toml_path);
            }
            free(src_copy);
        }
    }

    /* 1c. Detect "import raylib" in source and prepend raylib.iron */
    if (strstr(source, "import raylib") != NULL) {
        opts.use_raylib = true;
        /* Locate raylib.iron relative to base_dir */
        char *rl_path = make_path(base_dir, "stdlib/raylib.iron");
        if (rl_path) {
            long rl_size = 0;
            char *rl_src = read_file(rl_path, &rl_size);
            free(rl_path);
            if (rl_src) {
                /* Prepend raylib.iron source, then append user source */
                size_t combined_len = (size_t)rl_size + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, rl_src, (size_t)rl_size);
                    combined[rl_size] = '\n';
                    strcpy(combined + rl_size + 1, source);
                    free(source);
                    source = combined;
                }
                free(rl_src);
            }
        }
    }

    /* 1d. Detect "import math" and prepend math.iron */
    if (strstr(source, "import math") != NULL) {
        char *math_path = make_path(base_dir, "stdlib/math.iron");
        if (math_path) {
            long math_size = 0;
            char *math_src = read_file(math_path, &math_size);
            free(math_path);
            if (math_src) {
                size_t combined_len = (size_t)math_size + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, math_src, (size_t)math_size);
                    combined[math_size] = '\n';
                    strcpy(combined + math_size + 1, source);
                    free(source);
                    source = combined;
                }
                free(math_src);
            }
        }
    }

    /* 1e. Detect "import io" and prepend io.iron */
    if (strstr(source, "import io") != NULL) {
        char *io_path = make_path(base_dir, "stdlib/io.iron");
        if (io_path) {
            long io_size = 0;
            char *io_src = read_file(io_path, &io_size);
            free(io_path);
            if (io_src) {
                size_t combined_len = (size_t)io_size + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, io_src, (size_t)io_size);
                    combined[io_size] = '\n';
                    strcpy(combined + io_size + 1, source);
                    free(source);
                    source = combined;
                }
                free(io_src);
            }
        }
    }

    /* 1f. Detect "import time" and prepend time.iron */
    if (strstr(source, "import time") != NULL) {
        char *time_path = make_path(base_dir, "stdlib/time.iron");
        if (time_path) {
            long time_size = 0;
            char *time_src = read_file(time_path, &time_size);
            free(time_path);
            if (time_src) {
                size_t combined_len = (size_t)time_size + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, time_src, (size_t)time_size);
                    combined[time_size] = '\n';
                    strcpy(combined + time_size + 1, source);
                    free(source);
                    source = combined;
                }
                free(time_src);
            }
        }
    }

    /* 1g. Detect "import log" and prepend log.iron */
    if (strstr(source, "import log") != NULL) {
        char *log_path = make_path(base_dir, "stdlib/log.iron");
        if (log_path) {
            long log_size = 0;
            char *log_src = read_file(log_path, &log_size);
            free(log_path);
            if (log_src) {
                size_t combined_len = (size_t)log_size + 1 + strlen(source) + 1;
                char *combined = (char *)malloc(combined_len);
                if (combined) {
                    memcpy(combined, log_src, (size_t)log_size);
                    combined[log_size] = '\n';
                    strcpy(combined + log_size + 1, source);
                    free(source);
                    source = combined;
                }
                free(log_src);
            }
        }
    }

    /* 2. Set up arena and diagnostics */
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    /* 3. Lex */
    Iron_Lexer lexer = iron_lexer_create(source, source_path, &arena, &diags);
    Iron_Token *tokens = iron_lex_all(&lexer);

    if (diags.error_count > 0) {
        iron_diag_print_all(&diags, source);
        arrfree(tokens);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        free(base_dir);
        return 1;
    }

    /* 4. Parse */
    int token_count = (int)arrlen(tokens);
    Iron_Parser parser = iron_parser_create(tokens, token_count,
                                            source, source_path,
                                            &arena, &diags);
    Iron_Node *ast = iron_parse(&parser);
    arrfree(tokens);

    if (diags.error_count > 0) {
        iron_diag_print_all(&diags, source);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        free(base_dir);
        return 1;
    }

    /* 5. Analyze */
    /* Derive source directory for comptime read_file() resolution */
    char *src_path_copy = strdup(source_path);
    const char *src_file_dir = src_path_copy ? dirname(src_path_copy) : NULL;
    Iron_AnalyzeResult analysis = iron_analyze((Iron_Program *)ast, &arena,
                                               &diags,
                                               src_file_dir,
                                               source,
                                               (size_t)src_size,
                                               opts.force_comptime);
    free(src_path_copy);

    if (diags.error_count > 0 || analysis.has_errors) {
        iron_diag_print_all(&diags, source);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        free(base_dir);
        return 1;
    }

    /* 6. Lower AST to IR */
    Iron_Arena ir_arena = iron_arena_create(1024 * 1024);
    IronIR_Module *ir_module = iron_ir_lower((Iron_Program *)ast,
                                              analysis.global_scope,
                                              &ir_arena, &diags);

    if (!ir_module || diags.error_count > 0) {
        iron_diag_print_all(&diags, source);
        iron_diaglist_free(&diags);
        iron_arena_free(&ir_arena);
        iron_arena_free(&arena);
        free(source);
        free(base_dir);
        return 1;
    }

    /* 7. Verbose: print IR (before phi elimination) */
    if (opts.verbose) {
        char *ir_text = iron_ir_print(ir_module, true);
        if (ir_text) {
            fprintf(stdout, "=== Generated IR ===\n%s\n=== End IR ===\n\n", ir_text);
            free(ir_text);
        }
    }

    /* 7a. IR optimization passes */
    IronIR_OptimizeInfo optimize_info;
    iron_ir_optimize(ir_module, &optimize_info, &arena,
                     opts.dump_ir_passes, opts.no_optimize);

    /* 8. Emit C from IR */
    const char *c_src = iron_ir_emit_c(ir_module, &arena, &diags, &optimize_info);

    iron_ir_module_destroy(ir_module);
    iron_arena_free(&ir_arena);

    if (!c_src || diags.error_count > 0) {
        iron_diag_print_all(&diags, source);
        iron_diaglist_free(&diags);
        iron_ir_optimize_info_free(&optimize_info);
        iron_arena_free(&arena);
        free(source);
        free(base_dir);
        return 1;
    }

    iron_ir_optimize_info_free(&optimize_info);

    /* 9. Verbose: print generated C */
    if (opts.verbose) {
        fprintf(stdout, "=== Generated C ===\n%s\n=== End Generated C ===\n",
                c_src);
    }

    /* 10. Determine output binary name */
    char *derived_output = NULL;
    const char *binary_name;
    if (output_path) {
        binary_name = output_path;
    } else {
        derived_output = derive_output_name(source_path);
        if (!derived_output) {
            iron_diaglist_free(&diags);
            iron_arena_free(&arena);
            free(source);
            free(base_dir);
            return 1;
        }
        binary_name = derived_output;
    }

    /* 12. Write generated C to temp file */
    char *c_file_path = write_temp_c(c_src, opts.debug_build, binary_name);
    if (!c_file_path) {
        free(derived_output);
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        free(source);
        free(base_dir);
        return 1;
    }

    /* 13. Invoke clang */
    int ret = invoke_clang(c_file_path, binary_name, "src", opts);

    /* 14. Clean up temp file unless --debug-build */
    if (!opts.debug_build) {
#ifdef _WIN32
        DeleteFileA(c_file_path);
#else
        unlink(c_file_path);
#endif
    }
    free(c_file_path);

    /* 15. Clean up compiler resources */
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(source);
    free(base_dir);

    if (ret != 0) {
        free(derived_output);
        return 1;
    }

    /* 16. Run if requested (iron run) */
    if (opts.run_after) {
#ifdef _WIN32
        /* On Windows use CreateProcess to run the compiled binary */
        char run_cmd[32768];
        snprintf(run_cmd, sizeof(run_cmd), "%s", binary_name);
        /* Append extra args */
        for (int i = 0; i < opts.run_arg_count; i++) {
            size_t pos = strlen(run_cmd);
            snprintf(run_cmd + pos, sizeof(run_cmd) - pos, " %s", opts.run_args[i]);
        }
        STARTUPINFOA run_si;
        PROCESS_INFORMATION run_pi;
        memset(&run_si, 0, sizeof(run_si));
        run_si.cb = sizeof(run_si);
        memset(&run_pi, 0, sizeof(run_pi));
        if (!CreateProcessA(NULL, run_cmd, NULL, NULL, FALSE, 0, NULL, NULL,
                            &run_si, &run_pi)) {
            fprintf(stderr, "error: failed to run '%s' (error %lu)\n",
                    binary_name, GetLastError());
            free(derived_output);
            return 1;
        }
        WaitForSingleObject(run_pi.hProcess, INFINITE);
        DWORD run_exit = 0;
        GetExitCodeProcess(run_pi.hProcess, &run_exit);
        CloseHandle(run_pi.hProcess);
        CloseHandle(run_pi.hThread);
        free(derived_output);
        return (int)run_exit;
#else
        /* If binary_name has no '/', it's in the current directory.
         * posix_spawnp searches PATH, which won't find ./binary.
         * Prefix with "./" so the shell-like search works correctly. */
        char *exec_path = NULL;
        bool needs_free = false;
        if (strchr(binary_name, '/') == NULL) {
            size_t plen = strlen("./") + strlen(binary_name) + 1;
            exec_path = (char *)malloc(plen);
            if (exec_path) {
                snprintf(exec_path, plen, "./%s", binary_name);
                needs_free = true;
            } else {
                exec_path = (char *)binary_name;
            }
        } else {
            exec_path = (char *)binary_name;
        }

        /* Build argv */
        int total_args = 1 + opts.run_arg_count;
        char **run_argv = (char **)malloc(
            (size_t)(total_args + 1) * sizeof(char *));
        if (!run_argv) {
            if (needs_free) free(exec_path);
            free(derived_output);
            return 1;
        }
        run_argv[0] = exec_path;
        for (int i = 0; i < opts.run_arg_count; i++) {
            run_argv[i + 1] = (char *)opts.run_args[i];
        }
        run_argv[total_args] = NULL;

        /* Use posix_spawn (not posix_spawnp) so path is used directly */
        pid_t pid;
        int spawn_status = posix_spawn(&pid, exec_path, NULL, NULL,
                                        run_argv, environ);
        free(run_argv);
        if (needs_free) free(exec_path);
        if (spawn_status != 0) {
            fprintf(stderr, "error: failed to run '%s': %s\n",
                    binary_name, strerror(spawn_status));
            free(derived_output);
            return 1;
        }
        int wstatus;
        if (waitpid(pid, &wstatus, 0) < 0) {
            fprintf(stderr, "error: waitpid failed: %s\n", strerror(errno));
            free(derived_output);
            return 1;
        }
        free(derived_output);
        return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
#endif
    }

    /* 14. Success message */
    fprintf(stderr, "Built: %s\n", binary_name);
    free(derived_output);
    return 0;
}
