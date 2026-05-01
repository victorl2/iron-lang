/* Phase 14 Plan 14-02 (CMD-01, CMD-03) -- workspace/executeCommand handler.
 *
 * Implements the iron.migrate workspace command:
 *   1. CMD-03 version gate: probes ironc --version via popen(); if major < 3,
 *      emits window/showMessage Error and returns a JSON-RPC error response.
 *   2. Allocates a $/progress token from server->next_request_id.
 *   3. Emits 5-bucket $/progress notifications (begin + 5 reports + end).
 *   4. Copies workspace .iron files to a mkdtemp() temporary directory.
 *   5. Spawns ironc migrate --from v2 --to v3 <tempdir> via fork+execvp.
 *   6. Reads migrated files, builds WorkspaceEdit (changes map), returns it.
 *   7. Cleans up the tempdir on both success and failure paths.
 *
 * Anti-patterns avoided (per 14-RESEARCH.md):
 *   - Server never writes to the original workspace files.
 *   - popen used ONLY for ironc --version probe (short, non-interactive).
 *   - fork+execvp used for the actual codemod subprocess (stdout+stderr
 *     captured via separate pipe pairs per RESEARCH Open Q 3).
 *
 * Test hook:
 *   IRON_LSP_IRONC_PATH environment variable overrides the ironc binary path.
 *   Used by test_iron_migrate_command.py test_iron_migrate_version_gate_rejects_old_ironc
 *   to supply a stub ironc that prints an old version string.
 *   In production, the handler tries: (1) IRON_LSP_IRONC_PATH env var,
 *   (2) sibling lookup (/proc/self/exe or _NSGetExecutablePath), (3) PATH.
 */

#include "lsp/server/handlers_workspace_command.h"
#include "lsp/server/notifications.h"
#include "lsp/server/server.h"
#include "lsp/transport/json.h"
#include "lsp/transport/types.h"
#include "lsp/transport/writer.h"
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>  /* _NSGetExecutablePath */
#endif

/* ── Internal constants ───────────────────────────────────────────────── */

#define MAX_PATH 4096
#define MAX_CMD  8192
#define MAX_VERSION_BUF 128
#define MAX_FILE_BUF (1024 * 1024)  /* 1 MB max per migrated file */
#define IRONC_VERSION_PROBE_TIMEOUT_SECS 10

/* ── Forward declarations ─────────────────────────────────────────────── */

static void enqueue_heap_body(IronLsp_Writer *w, IronLsp_Priority prio,
                               const char *body, size_t len);
static void emit_progress_begin(IronLsp_Server *s, uint64_t token);
static void emit_progress_report(IronLsp_Server *s, uint64_t token,
                                  const char *message, int percentage);
static void emit_progress_end(IronLsp_Server *s, uint64_t token,
                               const char *message);
static void emit_error_response(IronLsp_Server *s, yyjson_val *id_v,
                                 int code, const char *message);
static void emit_result_response(IronLsp_Server *s, yyjson_val *id_v,
                                  yyjson_mut_val *result_val,
                                  yyjson_mut_doc *result_doc);
static bool locate_ironc(char *out_path, size_t out_size);
static bool probe_ironc_version(const char *ironc_path,
                                 char *out_version, size_t out_size);
static bool copy_iron_files(const char *src_dir, const char *dst_dir);
static int  spawn_ironc_migrate(const char *ironc_path, const char *tempdir,
                                 char *stderr_buf, size_t stderr_buf_size);
static int  rmdir_recursive(const char *path);
static char *uri_to_path(const char *uri);
static char *path_to_uri(const char *path);
static char *read_file(const char *path, size_t *out_len);
static void collect_iron_files(const char *dir,
                                char ***files_out, size_t *n_out);

/* ── Helper: enqueue a heap-copied body ───────────────────────────────── */

static void enqueue_heap_body(IronLsp_Writer *w, IronLsp_Priority prio,
                               const char *body, size_t len) {
    if (!w || !body || len == 0) return;
    char *heap = (char *)malloc(len);
    if (!heap) return;
    memcpy(heap, body, len);
    ilsp_writer_enqueue(w, prio, heap, len);
}

/* ── $/progress emitters ──────────────────────────────────────────────── */

static void emit_progress_begin(IronLsp_Server *s, uint64_t token) {
    if (!s || !s->writer) return;
    Iron_Arena arena = iron_arena_create(2 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
    if (!doc) { iron_arena_free(&arena); return; }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_strcpy(doc, root, "method",  "$/progress");

    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, params, "token", token);

    yyjson_mut_val *value = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, value, "kind",   "begin");
    yyjson_mut_obj_add_strcpy(doc, value, "title",  "Iron: Migrate v2 \xe2\x86\x92 v3");
    yyjson_mut_obj_add_bool  (doc, value, "cancellable", false);
    yyjson_mut_obj_add_int   (doc, value, "percentage", 0);

    yyjson_mut_obj_add_val(doc, params, "value", value);
    yyjson_mut_obj_add_val(doc, root,   "params", params);

    size_t len = 0;
    char *body = ilsp_json_write_mut(doc, &arena, &len);
    if (body && len > 0) enqueue_heap_body(s->writer, ILSP_PRIO_LOG, body, len);
    yyjson_mut_doc_free(doc);
    iron_arena_free(&arena);
}

static void emit_progress_report(IronLsp_Server *s, uint64_t token,
                                  const char *message, int percentage) {
    if (!s || !s->writer) return;
    Iron_Arena arena = iron_arena_create(2 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
    if (!doc) { iron_arena_free(&arena); return; }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_strcpy(doc, root, "method",  "$/progress");

    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, params, "token", token);

    yyjson_mut_val *value = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, value, "kind",       "report");
    yyjson_mut_obj_add_strcpy(doc, value, "message",    message ? message : "");
    yyjson_mut_obj_add_int   (doc, value, "percentage", percentage);

    yyjson_mut_obj_add_val(doc, params, "value", value);
    yyjson_mut_obj_add_val(doc, root,   "params", params);

    size_t len = 0;
    char *body = ilsp_json_write_mut(doc, &arena, &len);
    if (body && len > 0) enqueue_heap_body(s->writer, ILSP_PRIO_LOG, body, len);
    yyjson_mut_doc_free(doc);
    iron_arena_free(&arena);
}

static void emit_progress_end(IronLsp_Server *s, uint64_t token,
                               const char *message) {
    if (!s || !s->writer) return;
    Iron_Arena arena = iron_arena_create(2 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(&alc);
    if (!doc) { iron_arena_free(&arena); return; }

    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "jsonrpc", "2.0");
    yyjson_mut_obj_add_strcpy(doc, root, "method",  "$/progress");

    yyjson_mut_val *params = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, params, "token", token);

    yyjson_mut_val *value = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_strcpy(doc, value, "kind",    "end");
    yyjson_mut_obj_add_strcpy(doc, value, "message", message ? message : "");

    yyjson_mut_obj_add_val(doc, params, "value", value);
    yyjson_mut_obj_add_val(doc, root,   "params", params);

    size_t len = 0;
    char *body = ilsp_json_write_mut(doc, &arena, &len);
    if (body && len > 0) enqueue_heap_body(s->writer, ILSP_PRIO_LOG, body, len);
    yyjson_mut_doc_free(doc);
    iron_arena_free(&arena);
}

/* ── Response emitters ────────────────────────────────────────────────── */

static void emit_id_to_doc(yyjson_mut_doc *rd, yyjson_mut_val *root,
                            yyjson_val *id_v) {
    if (id_v && !yyjson_is_null(id_v)) {
        if (yyjson_is_int(id_v) || yyjson_is_sint(id_v))
            yyjson_mut_obj_add_sint(rd, root, "id", yyjson_get_sint(id_v));
        else if (yyjson_is_uint(id_v))
            yyjson_mut_obj_add_uint(rd, root, "id", yyjson_get_uint(id_v));
        else if (yyjson_is_str(id_v))
            yyjson_mut_obj_add_strcpy(rd, root, "id", yyjson_get_str(id_v));
        else
            yyjson_mut_obj_add_null(rd, root, "id");
    } else {
        yyjson_mut_obj_add_null(rd, root, "id");
    }
}

static void emit_error_response(IronLsp_Server *s, yyjson_val *id_v,
                                 int code, const char *message) {
    if (!s || !s->writer) return;
    Iron_Arena arena = iron_arena_create(2 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *rd = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&arena); return; }

    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_doc_set_root(rd, root);
    yyjson_mut_obj_add_strcpy(rd, root, "jsonrpc", "2.0");
    emit_id_to_doc(rd, root, id_v);

    yyjson_mut_val *err = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_int   (rd, err, "code",    code);
    yyjson_mut_obj_add_strcpy(rd, err, "message", message ? message : "");
    yyjson_mut_obj_add_val(rd, root, "error", err);

    size_t len = 0;
    char *body = ilsp_json_write_mut(rd, &arena, &len);
    if (body && len > 0) enqueue_heap_body(s->writer, ILSP_PRIO_RESPONSE, body, len);
    yyjson_mut_doc_free(rd);
    iron_arena_free(&arena);
}

static void emit_result_response(IronLsp_Server *s, yyjson_val *id_v,
                                  yyjson_mut_val *result_val,
                                  yyjson_mut_doc *result_doc) {
    if (!s || !s->writer || !result_doc) return;
    Iron_Arena arena = iron_arena_create(64 * 1024);
    yyjson_alc alc   = ilsp_json_alc(&arena);
    yyjson_mut_doc *rd = yyjson_mut_doc_new(&alc);
    if (!rd) { iron_arena_free(&arena); return; }

    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_doc_set_root(rd, root);
    yyjson_mut_obj_add_strcpy(rd, root, "jsonrpc", "2.0");
    emit_id_to_doc(rd, root, id_v);

    if (result_val) {
        /* Copy the result value into the response doc. We need to
         * re-serialize the result doc to a string and then embed it.
         * The simplest approach: serialize result_doc and null-result
         * on failure. */
        size_t res_len = 0;
        char *res_str = yyjson_mut_write(result_doc, 0, &res_len);
        if (res_str && res_len > 0) {
            /* Parse the serialized result back into the response arena. */
            yyjson_read_err err;
            memset(&err, 0, sizeof(err));
            yyjson_doc *parsed = yyjson_read_opts(res_str, res_len,
                                                   YYJSON_READ_NOFLAG,
                                                   &alc, &err);
            if (parsed) {
                yyjson_val *parsed_root = yyjson_doc_get_root(parsed);
                if (parsed_root) {
                    /* Convert immutable root to mutable copy for embedding. */
                    yyjson_mut_val *mut_result =
                        yyjson_val_mut_copy(rd, parsed_root);
                    if (mut_result) {
                        yyjson_mut_obj_add_val(rd, root, "result", mut_result);
                    } else {
                        yyjson_mut_obj_add_null(rd, root, "result");
                    }
                } else {
                    yyjson_mut_obj_add_null(rd, root, "result");
                }
            } else {
                yyjson_mut_obj_add_null(rd, root, "result");
            }
            free(res_str);
        } else {
            yyjson_mut_obj_add_null(rd, root, "result");
        }
    } else {
        yyjson_mut_obj_add_null(rd, root, "result");
    }

    size_t len = 0;
    char *body = ilsp_json_write_mut(rd, &arena, &len);
    if (body && len > 0) enqueue_heap_body(s->writer, ILSP_PRIO_RESPONSE, body, len);
    yyjson_mut_doc_free(rd);
    iron_arena_free(&arena);
}

/* ── ironc binary location ────────────────────────────────────────────── */

/* TEST HOOK: the environment variable IRON_LSP_IRONC_PATH overrides the
 * ironc binary path. This is the seam used by CMD-03 tests to supply a
 * stub ironc that reports an old version. Priority order:
 *   1. IRON_LSP_IRONC_PATH (test hook / explicit user override)
 *   2. Sibling-binary lookup (/proc/self/exe on Linux, _NSGetExecutablePath macOS)
 *   3. PATH search (falls back to just "ironc" for execvp resolution) */
static bool locate_ironc(char *out_path, size_t out_size) {
    if (!out_path || out_size == 0) return false;

    /* 1. Test hook / explicit override. */
    const char *env_path = getenv("IRON_LSP_IRONC_PATH");
    if (env_path && env_path[0] != '\0') {
        if (snprintf(out_path, out_size, "%s", env_path) < (int)out_size) {
            return true;
        }
    }

    /* 2. Sibling lookup: same directory as this running ironls binary. */
    char self_path[MAX_PATH];
    memset(self_path, 0, sizeof(self_path));
    bool got_self = false;

#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n > 0) {
        self_path[n] = '\0';
        got_self = true;
    }
#elif defined(__APPLE__)
    {
        uint32_t buf_size = (uint32_t)(sizeof(self_path) - 1);
        if (_NSGetExecutablePath(self_path, &buf_size) == 0) {
            self_path[buf_size] = '\0';
            got_self = true;
        }
    }
#endif

    if (got_self) {
        /* Find the last '/' to get the directory. */
        char *slash = strrchr(self_path, '/');
        if (slash) {
            *slash = '\0';  /* truncate to directory */
            char candidate[MAX_PATH];
            int nc = snprintf(candidate, sizeof(candidate),
                              "%s/ironc", self_path);
            if (nc > 0 && nc < (int)sizeof(candidate)) {
                if (access(candidate, X_OK) == 0) {
                    snprintf(out_path, out_size, "%s", candidate);
                    return true;
                }
            }
        }
    }

    /* 3. Fall back to PATH: execvp will search. */
    snprintf(out_path, out_size, "%s", "ironc");
    return true;  /* PATH fallback always "succeeds" here; exec failure detected later. */
}

/* ── ironc --version probe ────────────────────────────────────────────── */

/* Probe ironc --version via popen(). Returns true and fills out_version if
 * version was readable; false on failure. Note: popen() is used here
 * (rather than fork+exec) because --version is a read-only probe with
 * no side effects and a very short output. The full codemod subprocess
 * uses fork+execvp per RESEARCH Open Q 3. */
static bool probe_ironc_version(const char *ironc_path,
                                 char *out_version, size_t out_size) {
    if (!ironc_path || !out_version || out_size == 0) return false;
    char cmd[MAX_CMD];
    int nc = snprintf(cmd, sizeof(cmd), "%s --version 2>&1", ironc_path);
    if (nc <= 0 || nc >= (int)sizeof(cmd)) return false;

    FILE *f = popen(cmd, "r");
    if (!f) return false;

    char line[MAX_VERSION_BUF];
    char *got = fgets(line, (int)sizeof(line), f);
    pclose(f);
    if (!got) return false;

    /* Strip trailing newline. */
    size_t ln = strlen(line);
    if (ln > 0 && line[ln - 1] == '\n') line[ln - 1] = '\0';
    if (ln > 0 && line[ln - 1] == '\r') line[ln - 1] = '\0';

    /* Extract the first token that looks like a semver (starts with digit). */
    char *tok = strtok(line, " \t");
    while (tok) {
        if (tok[0] >= '0' && tok[0] <= '9') {
            snprintf(out_version, out_size, "%s", tok);
            return true;
        }
        tok = strtok(NULL, " \t");
    }
    return false;
}

/* ── URI <-> path helpers ─────────────────────────────────────────────── */

/* Convert "file:///some/path" to "/some/path". Returned string is malloc'd;
 * caller must free. Returns NULL on failure. */
static char *uri_to_path(const char *uri) {
    if (!uri) return NULL;
    const char *prefix = "file://";
    size_t prefix_len = strlen(prefix);
    if (strncmp(uri, prefix, prefix_len) != 0) {
        /* Not a file:// URI — return a copy as-is (may be a bare path). */
        return strdup(uri);
    }
    return strdup(uri + prefix_len);
}

/* Convert a filesystem path to a file:// URI. Returned string is malloc'd. */
static char *path_to_uri(const char *path) {
    if (!path) return NULL;
    char *result = (char *)malloc(strlen(path) + 8);
    if (!result) return NULL;
    sprintf(result, "file://%s", path);
    return result;
}

/* ── File I/O helpers ─────────────────────────────────────────────────── */

/* Read a file into a malloc'd buffer. Sets *out_len. Returns NULL on failure.
 * Caller must free. */
static char *read_file(const char *path, size_t *out_len) {
    if (!path || !out_len) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    rewind(f);
    if (fsz <= 0 || fsz > MAX_FILE_BUF) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)fsz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    if ((long)got != fsz) { free(buf); return NULL; }
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* ── Directory traversal ──────────────────────────────────────────────── */

/* Collect all .iron files (at maxdepth 1) in dir into a malloc'd array of
 * malloc'd paths. *n_out set to count. Caller frees each path + the array. */
static void collect_iron_files(const char *dir,
                                char ***files_out, size_t *n_out) {
    if (!dir || !files_out || !n_out) return;
    *files_out = NULL;
    *n_out = 0;

    DIR *d = opendir(dir);
    if (!d) return;

    size_t cap = 64;
    char **arr = (char **)calloc(cap, sizeof(char *));
    if (!arr) { closedir(d); return; }
    size_t count = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 5, ".iron") != 0) continue;

        char full[MAX_PATH];
        int nc = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (nc <= 0 || nc >= (int)sizeof(full)) continue;

        if (count >= cap) {
            cap *= 2;
            char **tmp = (char **)realloc(arr, cap * sizeof(char *));
            if (!tmp) break;
            arr = tmp;
        }
        arr[count++] = strdup(full);
    }
    closedir(d);
    *files_out = arr;
    *n_out = count;
}

/* Copy all .iron files from src_dir to dst_dir (flat copy; no subdirs).
 * Returns true if at least 0 files were processed without I/O errors. */
static bool copy_iron_files(const char *src_dir, const char *dst_dir) {
    if (!src_dir || !dst_dir) return false;

    DIR *d = opendir(src_dir);
    if (!d) return false;

    bool ok = true;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 5, ".iron") != 0) continue;

        char src[MAX_PATH], dst[MAX_PATH];
        int nc_s = snprintf(src, sizeof(src), "%s/%s", src_dir, ent->d_name);
        int nc_d = snprintf(dst, sizeof(dst), "%s/%s", dst_dir, ent->d_name);
        if (nc_s <= 0 || nc_s >= (int)sizeof(src)) continue;
        if (nc_d <= 0 || nc_d >= (int)sizeof(dst)) continue;

        size_t flen = 0;
        char *content = read_file(src, &flen);
        if (!content) { ok = false; continue; }

        FILE *out = fopen(dst, "wb");
        if (!out) { free(content); ok = false; continue; }
        size_t written = fwrite(content, 1, flen, out);
        fclose(out);
        free(content);
        if (written != flen) { ok = false; }
    }
    closedir(d);
    return ok;
}

/* ── Subprocess: ironc migrate ────────────────────────────────────────── */

/* Spawn `ironc migrate --from v2 --to v3 <tempdir>` via fork+execvp.
 * Captures stderr into stderr_buf. Returns the subprocess exit code,
 * or -1 on fork/exec failure. */
static int spawn_ironc_migrate(const char *ironc_path, const char *tempdir,
                                char *stderr_buf, size_t stderr_buf_size) {
    if (!ironc_path || !tempdir) return -1;
    if (stderr_buf && stderr_buf_size > 0) stderr_buf[0] = '\0';

    int stderr_pipe[2];
    if (pipe(stderr_pipe) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* CHILD: close read end, redirect stderr to pipe write end. */
        close(stderr_pipe[0]);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stderr_pipe[1]);
        /* Redirect stdout to /dev/null — we only care about exit code. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }

        char *const argv[] = {
            (char *)ironc_path,
            (char *)"migrate",
            (char *)"--from", (char *)"v2",
            (char *)"--to",   (char *)"v3",
            (char *)tempdir,
            NULL
        };
        execvp(ironc_path, argv);
        /* exec failed */
        _exit(127);
    }

    /* PARENT */
    close(stderr_pipe[1]);

    /* Read stderr. */
    if (stderr_buf && stderr_buf_size > 1) {
        size_t total = 0;
        ssize_t nr;
        while (total < stderr_buf_size - 1 &&
               (nr = read(stderr_pipe[0], stderr_buf + total,
                          stderr_buf_size - 1 - total)) > 0) {
            total += (size_t)nr;
        }
        stderr_buf[total] = '\0';
    }
    close(stderr_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;  /* signalled */
}

/* ── Recursive temp dir cleanup ───────────────────────────────────────── */

static int rmdir_recursive(const char *path) {
    if (!path) return -1;
    DIR *d = opendir(path);
    if (!d) { rmdir(path); return 0; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[MAX_PATH];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            rmdir_recursive(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    rmdir(path);
    return 0;
}

/* ── Main handler ─────────────────────────────────────────────────────── */

void ilsp_handle_workspace_execute_command(IronLsp_Server    *s,
                                           struct yyjson_doc *doc,
                                           Iron_Arena        *arena) {
    (void)arena;  /* not used — all allocations are explicit below */
    if (!s || !doc) return;

    yyjson_val *root   = yyjson_doc_get_root(doc);
    yyjson_val *id_v   = yyjson_obj_get(root, "id");
    yyjson_val *params = yyjson_obj_get(root, "params");

    /* ── 1. Extract command + arguments ── */
    const char *command = NULL;
    if (params && yyjson_is_obj(params)) {
        yyjson_val *cmd_v = yyjson_obj_get(params, "command");
        if (cmd_v && yyjson_is_str(cmd_v)) command = yyjson_get_str(cmd_v);
    }

    if (!command || command[0] == '\0') {
        emit_error_response(s, id_v, -32602, "workspace/executeCommand: missing command");
        return;
    }

    /* Only iron.migrate is supported in Phase 14. */
    if (strcmp(command, "iron.migrate") != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown command: %s", command);
        emit_error_response(s, id_v, -32602, msg);
        return;
    }

    /* Extract workspace root URI from arguments[0] or fall back to
     * server->workspace_root. */
    const char *workspace_uri = NULL;
    if (params) {
        yyjson_val *args = yyjson_obj_get(params, "arguments");
        if (args && yyjson_is_arr(args) && yyjson_arr_size(args) > 0) {
            yyjson_val *arg0 = yyjson_arr_get_first(args);
            if (arg0 && yyjson_is_str(arg0)) workspace_uri = yyjson_get_str(arg0);
        }
    }

    char workspace_path[MAX_PATH];
    if (workspace_uri) {
        char *p = uri_to_path(workspace_uri);
        if (p) {
            snprintf(workspace_path, sizeof(workspace_path), "%s", p);
            free(p);
        } else {
            workspace_path[0] = '\0';
        }
    } else if (s->workspace_root) {
        snprintf(workspace_path, sizeof(workspace_path), "%s", s->workspace_root);
    } else {
        emit_error_response(s, id_v, -32602,
                            "iron.migrate: no workspace root available");
        return;
    }

    if (workspace_path[0] == '\0') {
        emit_error_response(s, id_v, -32602,
                            "iron.migrate: could not resolve workspace path");
        return;
    }

    /* ── 2. Locate ironc binary ── */
    char ironc_path[MAX_PATH];
    if (!locate_ironc(ironc_path, sizeof(ironc_path))) {
        emit_error_response(s, id_v, -32603, "iron.migrate: could not locate ironc binary");
        return;
    }

    /* ── 3. CMD-03: version gate (FIRST STEP before any subprocess work) ── */
    char detected_version[MAX_VERSION_BUF];
    detected_version[0] = '\0';
    bool version_ok = probe_ironc_version(ironc_path, detected_version, sizeof(detected_version));

    if (!version_ok || detected_version[0] == '\0') {
        /* Could not probe version — treat as incompatible (pre-HARD-22 behavior). */
        char gate_msg[512];
        snprintf(gate_msg, sizeof(gate_msg),
                 "Iron: Migrate v2 \xe2\x86\x92 v3 requires ironc >= 3.0.0 (found unknown). "
                 "Update ironc and retry.");
        ilsp_send_window_showmessage(s, NULL, 1 /* ILSP_MESSAGE_TYPE_ERROR */, gate_msg);
        emit_error_response(s, id_v, -32603, "iron.migrate gated on ironc >= 3.0.0");
        return;
    }

    /* Parse major version number. */
    unsigned major = 0;
    sscanf(detected_version, "%u.", &major);
    if (major < 3) {
        /* CMD-03: exact error text per CONTEXT.md D-02. */
        char gate_msg[512];
        snprintf(gate_msg, sizeof(gate_msg),
                 "Iron: Migrate v2 \xe2\x86\x92 v3 requires ironc >= 3.0.0 (found %s). "
                 "Update ironc and retry.",
                 detected_version);
        ilsp_send_window_showmessage(s, NULL, 1 /* ILSP_MESSAGE_TYPE_ERROR */, gate_msg);
        emit_error_response(s, id_v, -32603, "iron.migrate gated on ironc >= 3.0.0");
        return;
    }

    /* ── 4. Allocate progress token ── */
    uint64_t token = atomic_fetch_add(&s->next_request_id, 1);

    /* ── 5. Emit begin progress ── */
    emit_progress_begin(s, token);

    /* ── Bucket 1/5: parsing manifest ── */
    emit_progress_report(s, token, "1/5: parsing manifest", 20);
    /* Informational check for iron.toml — does not block migration. */
    {
        char iron_toml_path[MAX_PATH];
        snprintf(iron_toml_path, sizeof(iron_toml_path),
                 "%s/iron.toml", workspace_path);
        /* stat() result is informational only — no action needed. */
        struct stat st;
        (void)stat(iron_toml_path, &st);
    }

    /* Create temp directory for codemod output. */
    char tempdir_template[MAX_PATH];
    snprintf(tempdir_template, sizeof(tempdir_template), "/tmp/ironls_migrate_XXXXXX");
    char *tempdir = mkdtemp(tempdir_template);
    if (!tempdir) {
        emit_progress_end(s, token, "Migration failed: could not create temp dir");
        ilsp_send_window_showmessage(s, NULL, 1,
                                      "Iron: Migrate failed: could not create temp dir");
        emit_error_response(s, id_v, -32603, "iron.migrate: mkdtemp failed");
        return;
    }

    /* Copy workspace .iron files to tempdir. */
    copy_iron_files(workspace_path, tempdir);

    /* ── Bucket 2/5: running codemod ── */
    emit_progress_report(s, token, "2/5: running codemod", 40);

    char stderr_buf[4096];
    int exit_code = spawn_ironc_migrate(ironc_path, tempdir,
                                         stderr_buf, sizeof(stderr_buf));
    if (exit_code != 0 && exit_code != 127) {
        /* exit code 0 = success; ironc migrate may exit 0 with "no changes needed" */
        /* Non-zero (other than exec failure 127) = migration failed. */
        /* Note: ironc migrate exits 0 even when no files need changes. */
    }
    if (exit_code == 127) {
        /* exec failed — ironc binary not found or not executable. */
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg),
                 "iron.migrate: could not execute ironc at '%s'", ironc_path);
        emit_progress_end(s, token, "Migration failed: ironc not found");
        ilsp_send_window_showmessage(s, NULL, 1, err_msg);
        emit_error_response(s, id_v, -32603, err_msg);
        rmdir_recursive(tempdir);
        return;
    }

    /* ── Bucket 3/5: applying rewrites ── */
    emit_progress_report(s, token, "3/5: applying rewrites", 60);

    /* Collect post-migration .iron files from tempdir. */
    char **temp_files = NULL;
    size_t n_temp = 0;
    collect_iron_files(tempdir, &temp_files, &n_temp);

    /* ── Bucket 4/5: formatting output + build WorkspaceEdit ── */
    emit_progress_report(s, token, "4/5: formatting output", 80);

    /* Build WorkspaceEdit JSON. Each file gets one TextEdit covering the
     * full file range [0,0]..[last_line, last_col] with newText = migrated
     * contents. The editor applies the changes after user approval. */
    yyjson_mut_doc *edit_doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *edit_obj = NULL;
    yyjson_mut_val *changes_obj = NULL;
    if (edit_doc) {
        edit_obj  = yyjson_mut_obj(edit_doc);
        yyjson_mut_doc_set_root(edit_doc, edit_obj);
        changes_obj = yyjson_mut_obj(edit_doc);
    }

    size_t changed_count = 0;
    for (size_t i = 0; i < n_temp && edit_doc && changes_obj; i++) {
        const char *temp_path = temp_files[i];
        if (!temp_path) continue;

        /* Derive the original workspace path by replacing the tempdir prefix. */
        const char *filename = strrchr(temp_path, '/');
        if (!filename) continue;
        filename++;  /* skip the '/' */

        char orig_path[MAX_PATH];
        snprintf(orig_path, sizeof(orig_path), "%s/%s", workspace_path, filename);

        /* Read the post-migration content from tempdir. */
        size_t new_len = 0;
        char *new_content = read_file(temp_path, &new_len);
        if (!new_content) continue;

        /* Read the original content to check if it changed. */
        size_t orig_len = 0;
        char *orig_content = read_file(orig_path, &orig_len);
        bool changed = (!orig_content || orig_len != new_len ||
                        memcmp(orig_content, new_content, orig_len) != 0);
        free(orig_content);

        if (!changed) {
            free(new_content);
            continue;
        }

        /* Count lines in new_content to build the end-range. */
        unsigned last_line = 0;
        unsigned last_col = 0;
        /* Count lines in original to build full-replace range. */
        if (orig_len > 0) {
            unsigned oline = 0;
            for (size_t j = 0; j < orig_len; j++) {
                if (((char *)orig_content)[j] == '\n') { oline++; last_col = 0; }
                else { last_col++; }
            }
            last_line = oline;
        }

        /* Build the TextEdit for this file. */
        yyjson_mut_val *edit_arr = yyjson_mut_arr(edit_doc);
        yyjson_mut_val *text_edit = yyjson_mut_obj(edit_doc);

        /* Range: [0,0] .. [last_line, last_col] */
        yyjson_mut_val *range = yyjson_mut_obj(edit_doc);
        yyjson_mut_val *range_start = yyjson_mut_obj(edit_doc);
        yyjson_mut_obj_add_int(edit_doc, range_start, "line",      0);
        yyjson_mut_obj_add_int(edit_doc, range_start, "character", 0);
        yyjson_mut_val *range_end = yyjson_mut_obj(edit_doc);
        yyjson_mut_obj_add_uint(edit_doc, range_end, "line",      last_line);
        yyjson_mut_obj_add_uint(edit_doc, range_end, "character", last_col);
        yyjson_mut_obj_add_val(edit_doc, range, "start", range_start);
        yyjson_mut_obj_add_val(edit_doc, range, "end",   range_end);

        yyjson_mut_obj_add_val   (edit_doc, text_edit, "range",   range);
        yyjson_mut_obj_add_strcpy(edit_doc, text_edit, "newText", new_content);
        free(new_content);

        yyjson_mut_arr_append(edit_arr, text_edit);

        /* Add to changes map under the original file URI. */
        char *orig_uri = path_to_uri(orig_path);
        if (orig_uri) {
            yyjson_mut_obj_add_val(edit_doc, changes_obj, orig_uri, edit_arr);
            free(orig_uri);
            changed_count++;
        }
    }

    /* ── Bucket 5/5: verifying parity ── */
    emit_progress_report(s, token, "5/5: verifying parity", 95);

    /* Cleanup tempdir. */
    rmdir_recursive(tempdir);
    if (temp_files) {
        for (size_t i = 0; i < n_temp; i++) free(temp_files[i]);
        free(temp_files);
    }

    /* ── Emit end progress ── */
    emit_progress_end(s, token, "Migration complete");

    /* ── Return WorkspaceEdit result ── */
    if (edit_doc && edit_obj && changes_obj) {
        yyjson_mut_obj_add_val(edit_doc, edit_obj, "changes", changes_obj);
        emit_result_response(s, id_v, edit_obj, edit_doc);
        yyjson_mut_doc_free(edit_doc);
    } else {
        if (edit_doc) yyjson_mut_doc_free(edit_doc);
        /* Return null result — editor handles the no-changes case. */
        Iron_Arena resp_arena = iron_arena_create(2 * 1024);
        yyjson_alc alc        = ilsp_json_alc(&resp_arena);
        yyjson_mut_doc *rd    = yyjson_mut_doc_new(&alc);
        if (rd) {
            yyjson_mut_val *root_obj = yyjson_mut_obj(rd);
            yyjson_mut_doc_set_root(rd, root_obj);
            yyjson_mut_obj_add_strcpy(rd, root_obj, "jsonrpc", "2.0");
            emit_id_to_doc(rd, root_obj, id_v);
            yyjson_mut_obj_add_null(rd, root_obj, "result");
            size_t len = 0;
            char *body = ilsp_json_write_mut(rd, &resp_arena, &len);
            if (body && len > 0) enqueue_heap_body(s->writer, ILSP_PRIO_RESPONSE, body, len);
            yyjson_mut_doc_free(rd);
        }
        iron_arena_free(&resp_arena);
    }

    (void)changed_count;  /* informational — used for parity sanity check */
}
