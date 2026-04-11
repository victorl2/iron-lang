#ifdef _WIN32
#error "build_web.c is not supported on Windows in Phase 2 (deferred until Iron gains Windows base support)"
#endif

#include "cli/build_web.h"
#include "cli/toml.h"
#include "cli/web_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>       /* access() */
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <libgen.h>       /* dirname() */
#include <errno.h>

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
    (void)output_path;  /* Phase 2: unused, Phase 7 consumes */
    (void)opts;         /* Phase 2: release flag stored for Phase 7 */

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

    /* 8. Phase 2 stub: real compilation lands in Phase 7. */
    printf("Phase 2: CLI + TOML scaffold complete; real compilation in Phase 7\n");

    /* 9. Cleanup. */
    free(emcc_version);
    free(emcc_path);
    free(pinned_version);
    if (proj) iron_toml_free(proj);
    return 0;
}
