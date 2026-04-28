#ifdef _WIN32
#error "build_web.c is not supported on Windows in Phase 2 (deferred until Iron gains Windows base support)"
#endif

#include "cli/build_web.h"
#include "cli/toml.h"
#include "cli/web_config.h"
#include "cli/web_shell_template.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       /* access() */
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <libgen.h>       /* dirname() */
#include <errno.h>
#include <fcntl.h>        /* O_WRONLY etc (mkstemp uses it) */

extern char **environ;

/* ── Section B: iron_read_pinned_emsdk_version ───────────────────────────── */

/* Read .emsdk-version from the repo root at runtime. Returns a malloc'd
 * copy of the version string (trailing newline stripped), or NULL if the
 * file is missing or unreadable. Caller must free().
 *
 * Resolution order:
 *   1. ./.emsdk-version (current working directory — typical iron dev setup)
 *   2. <source-file-dir>/../.emsdk-version (if source is in a subdir — best effort)
 *
 * If both miss, returns NULL.
 */
static char *iron_read_pinned_emsdk_version(const char *source_path) {
    char buf[64];
    FILE *f = fopen(".emsdk-version", "r");

    if (!f && source_path) {
        /* Fall back to <dirname(source_path)>/../.emsdk-version */
        char *src_copy = strdup(source_path);
        if (src_copy) {
            char *dir = dirname(src_copy);
            /* Construct path: <dir>/../.emsdk-version */
            size_t path_len = strlen(dir) + strlen("/../.emsdk-version") + 1;
            char *path = (char *)malloc(path_len);
            if (path) {
                snprintf(path, path_len, "%s/../.emsdk-version", dir);
                f = fopen(path, "r");
                free(path);
            }
            free(src_copy);
        }
    }

    if (!f) return NULL;

    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);

    /* Strip trailing \r and \n */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    if (len == 0) return NULL;
    return strdup(buf);
}

/* ── Section C: find_emcc ────────────────────────────────────────────────── */

/* Probe PATH for `emcc`. Returns a malloc'd absolute path to the emcc
 * binary on success, NULL on failure. Caller must free().
 *
 * Phase 2: Linux/macOS only. Does NOT probe emcc.bat/emcc.cmd — Windows
 * support deferred until Iron gains Windows base support.
 *
 * Does NOT consult $EMSDK or $EMSCRIPTEN_ROOT — PATH only. Env-var
 * fallback is deferred to Phase 7 per CONTEXT.md Claude's Discretion.
 */
static char *find_emcc(void) {
    const char *path_env = getenv("PATH");
    if (!path_env) return NULL;

    /* Duplicate so we can strtok without mutating the environment */
    char *path_dup = strdup(path_env);
    if (!path_dup) return NULL;

    char *found = NULL;
    char *token = strtok(path_dup, ":");

    while (token) {
        const char *dir = (token[0] == '\0') ? "." : token;
        size_t candidate_len = strlen(dir) + strlen("/emcc") + 1;
        char *candidate = (char *)malloc(candidate_len);
        if (candidate) {
            snprintf(candidate, candidate_len, "%s/emcc", dir);
            if (access(candidate, X_OK) == 0) {
                found = candidate;
                break;
            }
            free(candidate);
        }
        token = strtok(NULL, ":");
    }

    free(path_dup);
    return found;
}

/* ── Section D: get_emcc_version ─────────────────────────────────────────── */

/* Spawn `emcc --version` via popen and capture the first line of stdout.
 * Returns a malloc'd version string (e.g. "X.Y.Z") or NULL on failure.
 * Caller must free().
 *
 * emcc --version output looks like:
 *   emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld) X.Y.Z (...)
 *   Copyright (C) 2014 the Emscripten authors (see AUTHORS.txt)
 *   ...
 *
 * We extract the first X.Y.Z version sequence from the first line.
 */
static char *get_emcc_version(const char *emcc_path) {
    /* Build the command string: "<emcc_path> --version 2>&1" */
    size_t cmd_len = strlen(emcc_path) + strlen(" --version 2>&1") + 1;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) return NULL;
    snprintf(cmd, cmd_len, "%s --version 2>&1", emcc_path);

    FILE *fp = popen(cmd, "r");
    free(cmd);
    if (!fp) return NULL;

    char line[512];
    if (!fgets(line, sizeof(line), fp)) {
        pclose(fp);
        return NULL;
    }
    pclose(fp);

    /* Scan for the first X.Y.Z pattern (digits.digits.digits) */
    const char *p = line;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            /* Try to parse X.Y.Z starting here */
            int major, minor, patch;
            int consumed = 0;
            if (sscanf(p, "%d.%d.%d%n", &major, &minor, &patch, &consumed) == 3
                && consumed > 0) {
                char version_buf[32];
                snprintf(version_buf, sizeof(version_buf), "%d.%d.%d",
                         major, minor, patch);
                return strdup(version_buf);
            }
        }
        p++;
    }

    return NULL;
}

/* ── Section E: print_install_one_liner ──────────────────────────────────── */

/* Print the multi-line "error: emcc not found" message to stderr, with
 * the pinned emsdk version interpolated from .emsdk-version. If the
 * version cannot be read, falls back to a shorter message.
 */
static void print_install_one_liner(const char *pinned_version) {
    if (!pinned_version) {
        fprintf(stderr,
                "error: emcc not found in PATH\n"
                "\n"
                "iron build --target=web requires Emscripten.\n"
                "install via emsdk: https://emscripten.org/docs/getting_started/downloads.html\n");
        return;
    }

    fprintf(stderr,
            "error: emcc not found in PATH\n"
            "\n"
            "iron build --target=web requires Emscripten %s.\n"
            "install via emsdk: https://emscripten.org/docs/getting_started/downloads.html\n"
            "\n"
            "  git clone https://github.com/emscripten-core/emsdk\n"
            "  cd emsdk\n"
            "  ./emsdk install %s\n"
            "  ./emsdk activate %s\n"
            "  source ./emsdk_env.sh\n",
            pinned_version, pinned_version, pinned_version);
}

/* ── Section F: validate_web_config ─────────────────────────────────────── */

/* Validate the parsed IronWebConfig for Phase 2 sanity. Returns 0 on
 * success, non-zero on failure (and prints a descriptive error).
 *
 * Phase 2 checks (ONLY what Phase 2 will use):
 *   - title if set, must be non-empty
 *   - initial_memory if set (> 0) must be > 0 (belt-and-braces)
 *   - stack_size if set must be > 0
 *   - pthread_pool_size if set must be > 0 and <= 16
 *
 * Phase 7 will extend this with forbidden-flag rejection (WEB-BUILD-07)
 * and asset path existence / iron.toml-relative resolution.
 */
static int validate_web_config(const IronWebConfig *cfg) {
    if (cfg->title && strlen(cfg->title) == 0) {
        fprintf(stderr, "error: [web].title must not be empty\n");
        return 1;
    }

    if (cfg->initial_memory < 0) {
        fprintf(stderr, "error: [web].initial_memory must be positive\n");
        return 1;
    }

    if (cfg->stack_size < 0) {
        fprintf(stderr, "error: [web].stack_size must be positive\n");
        return 1;
    }

    if (cfg->pthread_pool_size < 0 || cfg->pthread_pool_size > 16) {
        fprintf(stderr,
                "error: [web].pthread_pool_size must be between 1 and 16 (got %d)\n",
                cfg->pthread_pool_size);
        return 1;
    }

    return 0;
}

/* ── Section G: iron_build_web entry point ───────────────────────────────── */

int iron_build_web(const char *source_path, const char *output_path,
                   IronBuildOpts opts) {
    (void)output_path;  /* Preflight: unused here; Phase 7 consumes for emcc output path */
    (void)opts;         /* Preflight: opts consumed by caller (iron_build) for target check; Phase 7 uses release flags */

    /* 1. Parse iron.toml alongside the source file, if any (best-effort). */
    IronWebConfig local_cfg = {0};
    IronWebConfig *cfg = &local_cfg;
    IronProject *proj = NULL;
    char *src_copy = strdup(source_path);
    if (src_copy) {
        char *dir = dirname(src_copy);
        size_t toml_len = strlen(dir) + strlen("/iron.toml") + 1;
        char *toml_path = (char *)malloc(toml_len);
        if (toml_path) {
            snprintf(toml_path, toml_len, "%s/iron.toml", dir);
            proj = iron_toml_parse(toml_path);
            free(toml_path);
        }
        free(src_copy);
    }
    if (proj) cfg = &proj->web;

    /* 2. Read pinned emsdk version (may be NULL if .emsdk-version missing). */
    char *pinned_version = iron_read_pinned_emsdk_version(source_path);

    /* 3. Probe PATH for emcc. */
    char *emcc_path = find_emcc();
    if (!emcc_path) {
        print_install_one_liner(pinned_version);
        free(pinned_version);
        if (proj) iron_toml_free(proj);
        return 1;
    }

    /* 4. Detect emcc version. */
    char *emcc_version = get_emcc_version(emcc_path);
    if (!emcc_version) {
        fprintf(stderr, "error: could not parse `emcc --version` output from %s\n",
                emcc_path);
        free(emcc_path);
        free(pinned_version);
        if (proj) iron_toml_free(proj);
        return 1;
    }

    /* 5. Print banner (stdout, before any other work). */
    printf("using emcc %s from %s\n", emcc_version, emcc_path);

    /* 6. Soft-warn on version drift (non-fatal). */
    if (pinned_version && strcmp(pinned_version, emcc_version) != 0) {
        fprintf(stderr,
                "warning: pinned version is %s per .emsdk-version but emcc reports %s;"
                " web builds may be non-reproducible\n",
                pinned_version, emcc_version);
    }

    /* 7. Validate config (Phase 2 sanity only). */
    if (validate_web_config(cfg) != 0) {
        free(emcc_version);
        free(emcc_path);
        free(pinned_version);
        if (proj) iron_toml_free(proj);
        return 1;
    }

    /* 8. Preflight success: iron_build() will run the main pipeline. */

    /* 9. Cleanup. */
    free(emcc_version);
    free(emcc_path);
    free(pinned_version);
    if (proj) iron_toml_free(proj);
    return 0;
}

/* ── Section H: mkdir_p helper (Phase 7, WEB-BUILD-06) ─────────────────────── */

/* Recursively create a directory path (like `mkdir -p`). Handles nested
 * directories, EEXIST as success, and reports errors to stderr.
 *
 * Linux/macOS only — Phase 7 keeps the #ifdef _WIN32 #error guard at
 * file top. Windows support is deferred until Iron gains Windows base
 * support (same rationale as Phase 1).
 *
 * Returns 0 on success (directory exists or was created), non-zero on error.
 */
static int mkdir_p(const char *path) {
    if (!path || !*path) return -1;

    /* Duplicate so we can mutate separators */
    char *copy = strdup(path);
    if (!copy) return -1;

    size_t len = strlen(copy);
    /* Strip trailing slash(es) so we don't create a bogus empty component */
    while (len > 1 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }

    /* Walk each path component, creating intermediates */
    for (char *p = copy + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                fprintf(stderr, "error: mkdir_p: cannot create '%s': %s\n",
                        copy, strerror(errno));
                free(copy);
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "error: mkdir_p: cannot create '%s': %s\n",
                copy, strerror(errno));
        free(copy);
        return -1;
    }

    free(copy);
    return 0;
}

/* ── Section I: iron_build_web_link (Phase 7, WEB-BUILD-01/03/04/06) ───────── */

/* Canonical emcc flag set (WEB-BUILD-03). These 12 entries are hardcoded
 * and MUST appear verbatim in every web link. No user override in Phase 7.
 *
 * Phase 2's IronWebConfig carries optional overrides for initial_memory,
 * stack_size, and pthread_pool_size — these are NOT applied in Phase 7
 * (Phase 11 may wire them). The defaults below match IRON_WEB_DEFAULT_*
 * in web_config.h so behavior is consistent.
 */
static const char *const IRON_WEB_CANONICAL_FLAGS[] = {
    "-pthread",
    "-sUSE_PTHREADS=1",
    "-sPTHREAD_POOL_SIZE=4",
    "-sINITIAL_MEMORY=134217728",
    "-sALLOW_MEMORY_GROWTH=1",
    "-sMAXIMUM_MEMORY=268435456",
    "-sSTACK_SIZE=1048576",
    "-sUSE_GLFW=3",
    "-sFORCE_FILESYSTEM=1",
    "-sGL_ENABLE_GET_PROC_ADDRESS=1",
    "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPF32,HEAP32,HEAPU8",
    "-sASYNCIFY=0",
};
static const size_t IRON_WEB_CANONICAL_FLAGS_COUNT =
    sizeof(IRON_WEB_CANONICAL_FLAGS) / sizeof(IRON_WEB_CANONICAL_FLAGS[0]);

/* ── Section I.2: Forbidden emcc flags (Phase 7, WEB-BUILD-07) ────────────── */

/* Forbidden emcc flag substrings. These strings MUST NEVER appear inside
 * any argv entry passed to emcc for a web build (WEB-BUILD-07). The list
 * is copied verbatim from REQUIREMENTS.md WEB-BUILD-07. Each entry is a
 * substring tested via strstr() against each argv entry — this handles
 * both the `-s<NAME>` form (emcc setting flags) and the `-fwasm-exceptions`
 * literal flag form.
 *
 * Rationale for each entry:
 *   ASYNCIFY=1                      — 20-40% runtime overhead, up to 10x WASM size blowup
 *   MINIMAL_RUNTIME                 — incompatible with -pthread in practice
 *   PROXY_TO_PTHREAD                — breaks emscripten_set_main_loop registration from main thread
 *   SAFE_HEAP                       — runtime cost, not needed once LIR verifier is trusted
 *   ALLOW_BLOCKING_ON_MAIN_THREAD   — papers over the underlying deadlock; fix the code instead
 *   ERROR_ON_UNDEFINED_SYMBOLS=0    — silences the #1 signal that something is linked wrong
 *   MODULARIZE                      — breaks the {{{ SCRIPT }}} template path in shell
 *   EXPORT_ES6                      — breaks the {{{ SCRIPT }}} template path in shell
 *   -fwasm-exceptions               — breaks simulate_infinite_loop semantics
 *
 * Plan 02 adds is_forbidden_flag() as a linear scan. Phase 7 has NO user-input
 * channel feeding argv, so this is a structural safety net — the audit catches
 * accidental additions in future phases (Plan 11 output layout overrides, user-
 * flag channels in Phase 13+).
 */
static const char *const IRON_WEB_FORBIDDEN_FLAGS[] = {
    "ASYNCIFY=1",
    "MINIMAL_RUNTIME",
    "PROXY_TO_PTHREAD",
    "SAFE_HEAP",
    "ALLOW_BLOCKING_ON_MAIN_THREAD",
    "ERROR_ON_UNDEFINED_SYMBOLS=0",
    "MODULARIZE",
    "EXPORT_ES6",
    "-fwasm-exceptions",
};
static const size_t IRON_WEB_FORBIDDEN_FLAGS_COUNT =
    sizeof(IRON_WEB_FORBIDDEN_FLAGS) / sizeof(IRON_WEB_FORBIDDEN_FLAGS[0]);

/* Test whether `flag` contains any forbidden substring.
 * Returns a pointer to the matching forbidden entry on hit, NULL if clean.
 * NULL-safe: is_forbidden_flag(NULL) returns NULL. */
static const char *is_forbidden_flag(const char *flag) {
    if (!flag) return NULL;
    for (size_t i = 0; i < IRON_WEB_FORBIDDEN_FLAGS_COUNT; i++) {
        if (strstr(flag, IRON_WEB_FORBIDDEN_FLAGS[i]) != NULL) {
            return IRON_WEB_FORBIDDEN_FLAGS[i];
        }
    }
    return NULL;
}

int iron_build_web_link(const char *c_file_path, IronBuildOpts opts,
                        IronWebConfig *cfg, const char *toml_dir,
                        const char *lib_dir) {
    if (!c_file_path) {
        fprintf(stderr, "error: iron_build_web_link: c_file_path is NULL\n");
        return 1;
    }

    /* 1. Find emcc (PATH probe). Same helper the Phase 2/6 preflight uses. */
    char *emcc_path = find_emcc();
    if (!emcc_path) {
        char *pinned = iron_read_pinned_emsdk_version(NULL);
        print_install_one_liner(pinned);
        free(pinned);
        return 1;
    }

    /* 2. Create dist/web/ (WEB-BUILD-06). */
    if (mkdir_p("dist/web") != 0) {
        fprintf(stderr, "error: cannot create dist/web/ output directory\n");
        free(emcc_path);
        return 1;
    }

    /* 2b. Resolve shell file (WEB-SHELL-05/06, Phase 9 Plan 02).
     *
     * Two paths:
     *   A. cfg->shell is set: validate the file contains {{{ SCRIPT }}};
     *      use it directly as the shell path (no temp file).
     *   B. cfg->shell is NULL (or cfg is NULL): materialize IRON_WEB_DEFAULT_SHELL
     *      to a mkstemp temp file; unlink after emcc completes.
     *
     * The shell path (either cfg->shell or the temp path) is stored in
     * shell_path_buf / shell_path and appended to argv as:
     *   "--shell-file"  shell_path
     *
     * use_temp_shell tracks whether we own the file (must unlink).
     */
    char shell_path_buf[64];
    shell_path_buf[0] = '\0';
    const char *shell_path = NULL;
    int use_temp_shell = 0;

    if (cfg && cfg->shell && cfg->shell[0] != '\0') {
        /* Path A: custom shell — validate it contains {{{ SCRIPT }}} */
        FILE *sf = fopen(cfg->shell, "r");
        if (!sf) {
            fprintf(stderr, "error: cannot open [web].shell file '%s': %s\n",
                    cfg->shell, strerror(errno));
            free(emcc_path);
            return 1;
        }

        /* Read the entire file into a heap buffer */
        if (fseek(sf, 0, SEEK_END) != 0) {
            fclose(sf);
            fprintf(stderr, "error: cannot seek [web].shell file '%s': %s\n",
                    cfg->shell, strerror(errno));
            free(emcc_path);
            return 1;
        }
        long shell_file_size = ftell(sf);
        rewind(sf);

        if (shell_file_size < 0) {
            fclose(sf);
            fprintf(stderr, "error: cannot determine size of [web].shell file '%s'\n",
                    cfg->shell);
            free(emcc_path);
            return 1;
        }

        char *shell_contents = (char *)malloc((size_t)shell_file_size + 1);
        if (!shell_contents) {
            fclose(sf);
            free(emcc_path);
            return 1;
        }

        size_t shell_read = fread(shell_contents, 1, (size_t)shell_file_size, sf);
        fclose(sf);
        shell_contents[shell_read] = '\0';

        if (strstr(shell_contents, "{{{ SCRIPT }}}") == NULL) {
            fprintf(stderr,
                    "error: [web].shell file '%s' is missing the required "
                    "{{{ SCRIPT }}} substitution token — emcc needs this to "
                    "insert its glue loader. Fix the file or remove [web].shell "
                    "to use the default.\n",
                    cfg->shell);
            free(shell_contents);
            free(emcc_path);
            return 1;
        }

        free(shell_contents);
        shell_path = cfg->shell;
        use_temp_shell = 0;

    } else {
        /* Path B: no custom shell — materialize IRON_WEB_DEFAULT_SHELL to a
         * temp file via mkstemp so emcc's --shell-file can point at a real path. */
        /* mkstemp requires a mutable buffer; copy the template in */
        const char *tmpl = "/tmp/iron_web_shell_XXXXXX.html";
        size_t tmpl_len = strlen(tmpl);
        if (tmpl_len >= sizeof(shell_path_buf)) {
            fprintf(stderr, "error: temp shell path template too long\n");
            free(emcc_path);
            return 1;
        }
        memcpy(shell_path_buf, tmpl, tmpl_len + 1);

        /* mkstemp replaces the XXXXXX suffix in-place; the .html suffix after
         * it is preserved (mkstemps would allow a suffix but is non-POSIX on
         * macOS/Linux; mkstemp on the full template string works fine because
         * the XXXXXX are the last 6 chars before the .html suffix — which
         * unfortunately means mkstemp does NOT randomise past the XXXXXX and
         * the .html remains literal). We use mkstemps when available; fall
         * back to mkstemp if not. */
#if defined(__APPLE__) || defined(__linux__)
        int tmp_fd = mkstemps(shell_path_buf, 5 /* ".html" length */);
#else
        /* Generic POSIX fallback: mkstemp ignores the suffix but the path
         * still ends in XXXXXX-replaced chars; caller gets a valid temp fd. */
        int tmp_fd = mkstemp(shell_path_buf);
#endif
        if (tmp_fd < 0) {
            fprintf(stderr, "error: mkstemp for default web shell failed: %s\n",
                    strerror(errno));
            free(emcc_path);
            return 1;
        }

        /* Write the embedded default shell in one shot, retrying on EINTR. */
        const char *shell_data = IRON_WEB_DEFAULT_SHELL;
        size_t shell_data_len = strlen(shell_data);
        size_t written = 0;
        while (written < shell_data_len) {
            ssize_t w = write(tmp_fd, shell_data + written, shell_data_len - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr,
                        "error: write to temp shell file '%s' failed: %s\n",
                        shell_path_buf, strerror(errno));
                close(tmp_fd);
                unlink(shell_path_buf);
                free(emcc_path);
                return 1;
            }
            written += (size_t)w;
        }
        close(tmp_fd);

        shell_path = shell_path_buf;
        use_temp_shell = 1;
    }

    /* 3. Resolve source file paths (Iron runtime + util + web-compatible
     *    stdlib, mirroring build_src_list in build.c but substituting
     *    iron_time_web.c for iron_time.c and dropping iron_net*.c).
     *
     *    Files linked on web (13 .c files total + the emitted C):
     *      src/util/stb_ds_impl.c
     *      src/util/arena.c
     *      src/util/strbuf.c
     *      src/runtime/iron_string.c
     *      src/runtime/iron_rc.c
     *      src/runtime/iron_builtins.c
     *      src/runtime/iron_threads.c
     *      src/runtime/iron_collections.c
     *      src/stdlib/iron_io.c
     *      src/stdlib/iron_log.c
     *      src/stdlib/iron_math.c
     *      src/stdlib/iron_hint.c
     *      src/stdlib/iron_time_web.c      (NOT iron_time.c)
     *
     *    Explicitly NOT linked on web:
     *      src/runtime/iron_net_init.c    (networking runtime — Phase 14)
     *      src/stdlib/iron_net.c          (networking stdlib — Phase 14)
     *      src/stdlib/iron_time.c         (native time shim — replaced by iron_time_web.c)
     *
     *    lib_dir comes from get_iron_lib_dir() in build.c, resolving to either
     *    ../lib/ (installed layout) or IRON_SOURCE_DIR (dev builds).
     */
    const char *iron_src_dir = lib_dir ? lib_dir : "src";

#define IRON_WEB_SRC_COUNT 14
    const char *rel_paths[IRON_WEB_SRC_COUNT] = {
        "util/stb_ds_impl.c",
        "util/arena.c",
        "util/strbuf.c",
        "runtime/iron_string.c",
        "runtime/iron_rc.c",
        "runtime/iron_builtins.c",
        "runtime/iron_threads.c",
        "runtime/iron_collections.c",
        "runtime/iron_fmt.c",            /* Phase 78: Int/Int32/Float → String */
        "stdlib/iron_io.c",
        "stdlib/iron_log.c",
        "stdlib/iron_math.c",
        "stdlib/iron_hint.c",
        "stdlib/iron_time_web.c",
    };
    char *abs_paths[IRON_WEB_SRC_COUNT] = {0};
    for (int i = 0; i < IRON_WEB_SRC_COUNT; i++) {
        size_t need = strlen(iron_src_dir) + 1 + strlen(rel_paths[i]) + 1;
        abs_paths[i] = (char *)malloc(need);
        if (!abs_paths[i]) {
            for (int j = 0; j < i; j++) free(abs_paths[j]);
            free(emcc_path);
            return 1;
        }
        snprintf(abs_paths[i], need, "%s/%s", iron_src_dir, rel_paths[i]);
    }

    /* Raylib sources — compiled as separate translation units, not amalgamated.
     *
     * When use_raylib is set, each raylib source is passed to emcc as its own
     * .c argument. emcc compiles each as an independent TU, so a
     * `#define FOO_IMPLEMENTATION` at the top of one source never leaks into
     * another. Crucially this matches what build.c already does for NATIVE
     * raylib builds (see invoke_clang's pre-compile loop at build.c:636-700),
     * which was designed specifically to avoid the bug where rlgl.h's
     * implementation block — which sits OUTSIDE its #ifndef RLGL_H header
     * guard — gets re-emitted when a second raylib source re-includes the
     * header in the same TU.
     *
     * The old approach used src/vendor/raylib/raylib.c as an amalgamation
     * driver (`#include "rcore.c"`, `#include "rshapes.c"`, ...) into one TU.
     * That worked only after adding `#undef RLGL_IMPLEMENTATION` between the
     * includes, and would have broken again the moment a raylib bump or new
     * amalgamated source re-included any other single-header library. The
     * per-source pattern is structurally immune to the entire bug class.
     *
     * rglfw is excluded: PLATFORM_WEB routes rcore.c through rcore_web.c
     * (raylib's web backend, included transitively via platform dispatch),
     * not through rcore_desktop_glfw.c.
     */
    /* Phase 60 Plan 01: +2 slots for the Iron-side raylib shim TUs
     * (iron_raylib.c wrappers + iron_raylib_layout.c ABI asserts).
     * They live under stdlib/, not vendor/raylib/, but share the
     * conditional-on-use_raylib lifecycle so they fit cleanly into
     * the same allocation / free / emcc-append loops below. */
#define IRON_WEB_RAYLIB_SRC_COUNT 8
    const char *rl_rel_paths[IRON_WEB_RAYLIB_SRC_COUNT] = {
        "vendor/raylib/rcore.c",
        "vendor/raylib/rshapes.c",
        "vendor/raylib/rtextures.c",
        "vendor/raylib/rtext.c",
        "vendor/raylib/rmodels.c",
        "vendor/raylib/raudio.c",
        "stdlib/iron_raylib.c",         /* Phase 60 Plan 01 shim */
        "stdlib/iron_raylib_layout.c",  /* Phase 60 Plan 01 ABI asserts */
    };
    char *rl_abs_paths[IRON_WEB_RAYLIB_SRC_COUNT] = {0};
    char *rl_i_flag = NULL;
    if (opts.use_raylib) {
        for (int i = 0; i < IRON_WEB_RAYLIB_SRC_COUNT; i++) {
            size_t need = strlen(iron_src_dir) + 1 + strlen(rl_rel_paths[i]) + 1;
            rl_abs_paths[i] = (char *)malloc(need);
            if (!rl_abs_paths[i]) {
                for (int j = 0; j < i; j++) free(rl_abs_paths[j]);
                for (int j = 0; j < IRON_WEB_SRC_COUNT; j++) free(abs_paths[j]);
                free(emcc_path);
                return 1;
            }
            snprintf(rl_abs_paths[i], need, "%s/%s", iron_src_dir, rl_rel_paths[i]);
        }
        size_t rl_i_len = strlen("-I") + strlen(iron_src_dir) + strlen("/vendor/raylib") + 1;
        rl_i_flag = (char *)malloc(rl_i_len);
        if (!rl_i_flag) {
            for (int j = 0; j < IRON_WEB_RAYLIB_SRC_COUNT; j++) free(rl_abs_paths[j]);
            for (int j = 0; j < IRON_WEB_SRC_COUNT; j++) free(abs_paths[j]);
            free(emcc_path);
            return 1;
        }
        snprintf(rl_i_flag, rl_i_len, "-I%s/vendor/raylib", iron_src_dir);
    }

    /* Build the -I flags (src dir for header resolution). */
    size_t src_i_len = strlen("-I") + strlen(iron_src_dir) + 1;
    char *src_i_flag = (char *)malloc(src_i_len);
    if (!src_i_flag) {
        free(rl_i_flag);
        for (int i = 0; i < IRON_WEB_RAYLIB_SRC_COUNT; i++) free(rl_abs_paths[i]);
        for (int i = 0; i < IRON_WEB_SRC_COUNT; i++) free(abs_paths[i]);
        free(emcc_path);
        return 1;
    }
    snprintf(src_i_flag, src_i_len, "-I%s", iron_src_dir);

    size_t stdlib_i_len = strlen("-I") + strlen(iron_src_dir) + strlen("/stdlib") + 1;
    char *stdlib_i_flag = (char *)malloc(stdlib_i_len);
    if (!stdlib_i_flag) {
        free(src_i_flag);
        free(rl_i_flag);
        for (int i = 0; i < IRON_WEB_RAYLIB_SRC_COUNT; i++) free(rl_abs_paths[i]);
        for (int i = 0; i < IRON_WEB_SRC_COUNT; i++) free(abs_paths[i]);
        free(emcc_path);
        return 1;
    }
    snprintf(stdlib_i_flag, stdlib_i_len, "-I%s/stdlib", iron_src_dir);

    size_t vendor_i_len = strlen("-I") + strlen(iron_src_dir) + strlen("/vendor") + 1;
    char *vendor_i_flag = (char *)malloc(vendor_i_len);
    if (!vendor_i_flag) {
        free(src_i_flag); free(stdlib_i_flag);
        free(rl_i_flag);
        for (int i = 0; i < IRON_WEB_RAYLIB_SRC_COUNT; i++) free(rl_abs_paths[i]);
        for (int i = 0; i < IRON_WEB_SRC_COUNT; i++) free(abs_paths[i]);
        free(emcc_path);
        return 1;
    }
    snprintf(vendor_i_flag, vendor_i_len, "-I%s/vendor", iron_src_dir);

    /* WEB-ASSET-01/02/04/05: per-asset preload mapping strings.
     * Heap-allocated; freed on every return path below. */
    char *preload_mappings[16] = {0};
    int   preload_count        = 0;

    /* 4. Build the emcc argv.
     *
     *    Layout:
     *      [0]     emcc
     *      [1..12] canonical flags (12 entries from IRON_WEB_CANONICAL_FLAGS)
     *      [13..15] release flag set (3 entries) OR [13..16] debug flag set (4 entries, includes -gsource-map)
     *      [16]    --shell-file  (Phase 9 Plan 02)
     *      [17]    <shell_path>  (cfg->shell or temp file)
     *      [18]    -o
     *      [19]    dist/web/index.html
     *      [20..22] -I include paths (src, stdlib, vendor)
     *      [23]    c_file_path (emitted C from emit_web_module)
     *      [24..36] 13 Iron runtime/stdlib source files
     *      [37..47] raylib entries when opts.use_raylib: 6 vendor raylib .c files +
     *                2 Iron shim .c files + -DPLATFORM_WEB +
     *                -DGRAPHICS_API_OPENGL_ES2 + -I<lib>/vendor/raylib (11 slots)
     *      [48..63] up to 8 --preload-file pairs (8 assets × 2 slots)
     *      [64]    NULL terminator
     *
     *    High-water mark with release flags and every feature on: 64 slots used;
     *    debug adds one more slot, still leaving safe margin.
     *    Allocate 80 slots — safe margin for future phases.
     */
    const int max_argv = 80;
    const char *argv[80];
    int n = 0;

    argv[n++] = emcc_path;

    /* Canonical flag set (WEB-BUILD-03) */
    for (size_t i = 0; i < IRON_WEB_CANONICAL_FLAGS_COUNT; i++) {
        argv[n++] = IRON_WEB_CANONICAL_FLAGS[i];
    }

    /* Release/debug flag set (WEB-CLI-05/06 from Phase 2; WEB-OUT-04 from Phase 11) */
    if (opts.release) {
        argv[n++] = "-Oz";
        argv[n++] = "-flto";
        argv[n++] = "-sASSERTIONS=0";
    } else {
        argv[n++] = "-O0";
        argv[n++] = "-g";
        argv[n++] = "-sASSERTIONS=1";
        argv[n++] = "-gsource-map";  /* WEB-OUT-04: emit index.wasm.map for DevTools sourcemap support */
    }

    /* Shell file (WEB-SHELL-05/06, Phase 9 Plan 02).
     * shell_path is either cfg->shell (user-supplied, already validated) or
     * the temp file path written above. emcc accepts --shell-file anywhere in
     * the linker argv, so we place it before -o for clarity. */
    argv[n++] = "--shell-file";
    argv[n++] = shell_path;

    /* Output path */
    argv[n++] = "-o";
    argv[n++] = "dist/web/index.html";

    /* Include paths */
    argv[n++] = src_i_flag;
    argv[n++] = stdlib_i_flag;
    argv[n++] = vendor_i_flag;

    /* Emitted C file from emit_web_module + write_temp_c */
    argv[n++] = c_file_path;

    /* Iron runtime + util + stdlib source files */
    for (int i = 0; i < IRON_WEB_SRC_COUNT; i++) {
        argv[n++] = abs_paths[i];
    }

    /* Raylib sources + flags (WEB-BUILD-05, Phase 8 — rewritten to drop the
     * amalgamation driver after it caused a cascade of masked CI failures).
     *
     * When the user's program imports raylib (or iron.toml sets raylib = true),
     * build.c sets opts.use_raylib = true and we append each raylib source
     * file as an individual .c argument to emcc, plus three flags:
     *   -DPLATFORM_WEB              routes rcore.c through platforms/rcore_web.c
     *                               (raylib's web backend, verified at
     *                               src/vendor/raylib/rcore.c lines 545-546)
     *   -DGRAPHICS_API_OPENGL_ES2   selects the GLES2 rlgl backend, which is
     *                               what emcc + WebGL2 expects
     *   -I<lib>/vendor/raylib       header search path for raylib's internal
     *                               #include "raylib.h" / "rcamera.h" / etc.
     *
     * The fourth raylib-required flag — -sUSE_GLFW=3 — already lives in
     * IRON_WEB_CANONICAL_FLAGS since Phase 7. We do NOT touch it here.
     *
     * Why NOT an amalgamation: the old src/vendor/raylib/raylib.c included
     * each raylib source via `#include "rcore.c"` etc. into one TU. That
     * was broken by rlgl.h's implementation block sitting OUTSIDE its
     * `#ifndef RLGL_H` header guard — the second re-include of rlgl.h in
     * the same TU re-emitted every rlgl symbol. Native builds already
     * side-step this bug by pre-compiling each raylib source as its own .o
     * (see build.c:636-700). This path now does the same: emcc receives
     * each .c as a separate input and compiles each in its own TU, so
     * `#define FOO_IMPLEMENTATION` in one source never leaks into another.
     * Structurally immune to the whole bug class, including future raylib
     * bumps that might introduce new cross-file re-includes.
     *
     * rglfw is excluded: PLATFORM_WEB routes rcore.c through rcore_web.c
     * via raylib's platform dispatch, not through rcore_desktop_glfw.c.
     *
     * The 6 raylib source paths plus the 2 Iron shim paths are heap-allocated above via rl_abs_paths;
     * rl_i_flag is the heap-allocated -I flag. Both are freed on every
     * exit path.
     */
    if (opts.use_raylib) {
        for (int i = 0; i < IRON_WEB_RAYLIB_SRC_COUNT; i++) {
            argv[n++] = rl_abs_paths[i];
        }
        argv[n++] = "-DPLATFORM_WEB";
        argv[n++] = "-DGRAPHICS_API_OPENGL_ES2";
        argv[n++] = rl_i_flag;
    }

    /* WEB-ASSET-01/02/04/05: preload [web].assets directories via --preload-file.
     *
     * Each entry in cfg->assets[] is resolved relative to toml_dir (WEB-ASSET-04),
     * stat-checked for existence + directory-ness, and emitted as a --preload-file
     * argv pair mapping <abs>@/<basename>. Missing directories warn + skip,
     * never fail the build (WEB-ASSET-05, asset-free games are supported).
     */
    if (cfg && cfg->assets && cfg->asset_count > 0) {
        const char *eff_toml_dir = toml_dir ? toml_dir : ".";
        for (int ai = 0; ai < cfg->asset_count && ai < 16; ai++) {
            const char *rel = cfg->assets[ai];
            if (!rel || !*rel) continue;

            /* Resolve: <toml_dir>/<rel>, then strip any trailing slash(es)
             * so the final --preload-file mapping is `<abs>@/<mount>` and
             * not `<abs>/@/<mount>` when the user wrote "assets/" with a
             * trailing slash in iron.toml. emcc accepts both forms but the
             * CI smoke asserts the canonical "assets@/assets" shape. */
            size_t need = strlen(eff_toml_dir) + 1 + strlen(rel) + 1;
            char *abs = (char *)malloc(need);
            if (!abs) continue;
            snprintf(abs, need, "%s/%s", eff_toml_dir, rel);
            size_t abs_len = strlen(abs);
            while (abs_len > 1 && abs[abs_len - 1] == '/') {
                abs[--abs_len] = '\0';
            }

            /* stat + is-directory check (WEB-ASSET-05 missing-dir warning) */
            struct stat st;
            if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
                fprintf(stderr,
                        "warning: [web].assets directory '%s' not found — "
                        "asset-free build continues\n", rel);
                free(abs);
                continue;
            }

            /* Compute basename of the ORIGINAL rel for the mount point.
             * "assets/" -> "assets"; "sounds/sfx/" -> "sfx".
             * Strip trailing slash(es), then take the last '/' segment. */
            char *rel_copy = strdup(rel);
            if (!rel_copy) { free(abs); continue; }
            size_t rc_len = strlen(rel_copy);
            while (rc_len > 0 && rel_copy[rc_len - 1] == '/') {
                rel_copy[--rc_len] = '\0';
            }
            const char *slash = strrchr(rel_copy, '/');
            const char *mount_base = slash ? slash + 1 : rel_copy;

            /* Build "<abs>@/<mount_base>" */
            size_t map_len = strlen(abs) + 2 + strlen(mount_base) + 1;
            char *mapping = (char *)malloc(map_len);
            if (!mapping) { free(rel_copy); free(abs); continue; }
            snprintf(mapping, map_len, "%s@/%s", abs, mount_base);
            free(rel_copy);
            free(abs);

            /* Track for cleanup + append to argv */
            preload_mappings[preload_count++] = mapping;
            argv[n++] = "--preload-file";
            argv[n++] = mapping;
        }
    }

    argv[n] = NULL;

    /* 5. Verbose mode: dump the full command before spawning */
    if (opts.verbose) {
        fprintf(stderr, "iron build --target=web: spawning emcc:\n  ");
        for (int i = 0; i < n; i++) {
            fprintf(stderr, "%s ", argv[i]);
        }
        fprintf(stderr, "\n");
    }

    /* 6. Forbidden-flag self-audit (WEB-BUILD-07 + WEB-BUILD-08, Plan 02).
     *
     * Walk the final argv and reject the build if any entry contains a
     * forbidden substring. Phase 7 has no user-input channel feeding argv
     * so this audit is a structural safety net — it catches accidental
     * additions in future phases. Runs BEFORE posix_spawnp so a rejection
     * never actually starts emcc.
     */
    for (int i = 0; i < n; i++) {
        const char *hit = is_forbidden_flag(argv[i]);
        if (hit) {
            fprintf(stderr,
                    "error: forbidden emcc flag '%s' — "
                    "web builds cannot use this flag. Refusing to build.\n",
                    hit);
            if (use_temp_shell) unlink(shell_path_buf);
            for (int pi = 0; pi < preload_count; pi++) free(preload_mappings[pi]);
            free(stdlib_i_flag); free(vendor_i_flag);
            free(src_i_flag);
            free(rl_i_flag);
            for (int j = 0; j < IRON_WEB_RAYLIB_SRC_COUNT; j++) free(rl_abs_paths[j]);
            for (int j = 0; j < IRON_WEB_SRC_COUNT; j++) free(abs_paths[j]);
            free(emcc_path);
            return 1;
        }
    }

    /* 7. Spawn emcc via posix_spawnp. */
    pid_t pid;
    int spawn_rc = posix_spawnp(&pid, emcc_path, NULL, NULL,
                                (char *const *)argv, environ);
    if (spawn_rc != 0) {
        fprintf(stderr, "error: failed to spawn emcc: %s\n", strerror(spawn_rc));
        if (use_temp_shell) unlink(shell_path_buf);
        for (int pi = 0; pi < preload_count; pi++) free(preload_mappings[pi]);
        free(stdlib_i_flag); free(vendor_i_flag);
        free(src_i_flag);
        free(rl_i_flag);
        for (int i = 0; i < IRON_WEB_RAYLIB_SRC_COUNT; i++) free(rl_abs_paths[i]);
        for (int i = 0; i < IRON_WEB_SRC_COUNT; i++) free(abs_paths[i]);
        free(emcc_path);
        return 1;
    }

    int wstatus;
    if (waitpid(pid, &wstatus, 0) < 0) {
        fprintf(stderr, "error: waitpid on emcc failed: %s\n", strerror(errno));
        if (use_temp_shell) unlink(shell_path_buf);
        for (int pi = 0; pi < preload_count; pi++) free(preload_mappings[pi]);
        free(stdlib_i_flag); free(vendor_i_flag);
        free(src_i_flag);
        free(rl_i_flag);
        for (int i = 0; i < IRON_WEB_RAYLIB_SRC_COUNT; i++) free(rl_abs_paths[i]);
        for (int i = 0; i < IRON_WEB_SRC_COUNT; i++) free(abs_paths[i]);
        free(emcc_path);
        return 1;
    }

    int emcc_rc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;

    /* 8. Cleanup — unlink temp shell on BOTH success and failure paths so
     * no /tmp/iron_web_shell_*.html stragglers are left behind. */
    if (use_temp_shell) unlink(shell_path_buf);
    for (int pi = 0; pi < preload_count; pi++) free(preload_mappings[pi]);
    free(stdlib_i_flag); free(vendor_i_flag);
    free(src_i_flag);
    free(rl_i_flag);
    for (int i = 0; i < IRON_WEB_RAYLIB_SRC_COUNT; i++) free(rl_abs_paths[i]);
    for (int i = 0; i < IRON_WEB_SRC_COUNT; i++) free(abs_paths[i]);
    free(emcc_path);

    if (emcc_rc != 0) {
        fprintf(stderr, "error: emcc exited with status %d\n", emcc_rc);
        return 1;
    }

    (void)max_argv; /* suppress unused-variable warning */
    fprintf(stderr, "Built: dist/web/index.html\n");
    return 0;
}
