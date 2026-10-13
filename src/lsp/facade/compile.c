/* Phase 2 Plan 05 Task 02 -- Compilation facade.
 *
 * This file is the SINGLE call site for iron_analyze_buffer from
 * anywhere under src/lsp. Every hover / goto / diagnostic path in the
 * LSP must flow through one of the three entry points below. The
 * CORE-22 parity harness enforces this structurally (grep check).
 *
 *   - ilsp_facade_compile_pure  : analyzer-only; caller owns arena + diags.
 *   - ilsp_facade_compile       : + translate Iron_Diagnostic[] -> LSP
 *                                  Diagnostic[] + enqueue publishDiagnostics.
 *   - ilsp_facade_pull_diagnostic : synchronous pull; builds
 *                                   DocumentDiagnosticReport response
 *                                   and enqueues at ILSP_PRIO_RESPONSE.
 *
 * Arena discipline (HARD-06): the facade owns a per-request 64 KB arena
 * that is created + freed within each call. No arena outlives a single
 * compile. */

#include "lsp/facade/compile.h"
#include "lsp/facade/diagnostics.h"
#include "lsp/store/document.h"
#include "lsp/server/server.h"
#include "lsp/transport/writer.h"
#include "lsp/transport/types.h"
#include "lsp/transport/json.h"
#include "analyzer/analyzer.h"
#include "analyzer/stdlib_prepend.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"
#include "vendor/stb_ds.h"

#include "util/pthread_compat.h"
#include "util/os.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Internal helper: enqueue a heap-copied JSON body onto the writer. */
static void enqueue_body(IronLsp_Writer *w, IronLsp_Priority prio,
                          const char *arena_body, size_t body_len) {
    if (!w || !arena_body || body_len == 0) return;
    char *heap = (char *)malloc(body_len);
    if (!heap) return;
    memcpy(heap, arena_body, body_len);
    ilsp_writer_enqueue(w, prio, heap, body_len);
    /* Ownership transferred; do not touch `heap` after this. */
}

/* Keep only the diagnostics of the edited document: the stdlib prelude's
 * carry their own file names. (The analyzer orders the program's decls the
 * same way; see Iron_Program.prelude_decl_count.) */
static bool same_diag(const Iron_Diagnostic *a, const Iron_Diagnostic *b) {
    return a->code == b->code && a->level == b->level &&
           a->span.line == b->span.line && a->span.col == b->span.col &&
           a->span.end_line == b->span.end_line && a->span.end_col == b->span.end_col &&
           strcmp(a->message ? a->message : "", b->message ? b->message : "") == 0;
}

/* Also drops exact repeats: the parser reports every recovery error in LSP
 * mode, and a recovery path that meets the same token twice reports it
 * twice; editors show both. */
static void keep_document_diags(Iron_DiagList *diags, const char *doc_file) {
    int n = 0, errors = 0, warnings = 0;
    for (int i = 0; i < diags->count; i++) {
        Iron_Diagnostic *dg = &diags->items[i];
        if (dg->span.filename && strcmp(dg->span.filename, doc_file) != 0) continue;
        bool repeat = false;
        for (int k = n - 1; k >= 0 && !repeat; k--) repeat = same_diag(&diags->items[k], dg);
        if (repeat) continue;
        if (dg->level == IRON_DIAG_ERROR) errors++;
        else if (dg->level == IRON_DIAG_WARNING) warnings++;
        diags->items[n++] = *dg;
    }
    if (diags->items) arrsetlen(diags->items, n);
    diags->count = n;
    diags->error_count = errors;
    diags->warning_count = warnings;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Lower case, '/' separators: Windows URIs spell the drive letter in lower
 * case (`file:///c%3A/...`) while the lib dir has `C:\...`. */
static char norm_path_char(char c) {
    if (c == '\\') return '/';
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

/* True when `uri` (a file:// URI or a path) names a file under
 * <lib_dir>/stdlib/. */
static bool is_stdlib_file(const char *uri, const char *lib_dir) {
    char path[4096];
    const char *src = uri;
    bool is_uri = strncmp(src, "file://", 7) == 0;
    if (is_uri) src += 7;
    size_t n = 0;
    for (; *src && n + 1 < sizeof(path); src++) {
        int hi, lo;
        if (is_uri && src[0] == '%' && (hi = hex_val(src[1])) >= 0 &&
            (lo = hex_val(src[2])) >= 0) {
            path[n++] = (char)(hi * 16 + lo);
            src += 2;
        } else {
            path[n++] = *src;
        }
    }
    path[n] = '\0';
    const char *p = path;
    if (p[0] == '/' && p[1] && p[2] == ':') p++;  /* /c:/... */

    size_t i = 0;
    for (; lib_dir[i]; i++) {
        if (!p[i] || norm_path_char(p[i]) != norm_path_char(lib_dir[i])) return false;
    }
    const char *rest = p + i;
    if (*rest == '/' || *rest == '\\') rest++;
    return strncmp(rest, "stdlib/", 7) == 0 || strncmp(rest, "stdlib\\", 7) == 0;
}

/* ── The other files of the document's package ───────────────────────────
 * `iron build` compiles a package as one unit: every .iron file under
 * vendor/ and src/ (target/combined.iron). A document of a package is
 * analyzed the same way, the other files ahead of it under `-- @file:`
 * markers, so a name declared in another file resolves and the visibility
 * rules between files apply as they do in a build. The other files are
 * read from disk (their saved contents). */

#define ILSP_PACKAGE_MAX_FILES 4000
#define ILSP_PACKAGE_MAX_BYTES (32u * 1024u * 1024u)

/* The file system path of a `file://` URI (percent escapes decoded, the
 * slash before a Windows drive letter dropped), or `uri` itself when it is
 * already a path. False when it does not fit. */
static bool doc_fs_path(const char *uri, char *out, size_t cap) {
    const char *src = uri;
    bool is_uri = strncmp(src, "file://", 7) == 0;
    if (is_uri) src += 7;
    size_t n = 0;
    for (; *src; src++) {
        if (n + 1 >= cap) return false;
        int hi, lo;
        if (is_uri && src[0] == '%' && (hi = hex_val(src[1])) >= 0 &&
            (lo = hex_val(src[2])) >= 0) {
            out[n++] = (char)(hi * 16 + lo);
            src += 2;
        } else {
            out[n++] = *src == '\\' ? '/' : *src;
        }
    }
    out[n] = '\0';
    if (is_uri && out[0] == '/' && out[1] && out[2] == ':') memmove(out, out + 1, n);
    return n > 0;
}

static bool path_is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool path_is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool same_path(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
#ifdef _WIN32
        if (norm_path_char(*a) != norm_path_char(*b)) return false;
#else
        if (*a != *b) return false;
#endif
    }
    return *a == *b;
}

static int path_ptr_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The .iron files under `dir`, as `iron build` collects them: hidden
 * directories skipped; under vendor/ also target, tests and examples, and a
 * vendored package (iron.toml and src/) gives its src/ only. */
static void collect_package_dir(const char *dir, bool vendor, char ***out) {
    char child[4096];
    if (vendor) {
        snprintf(child, sizeof(child), "%s/iron.toml", dir);
        if (path_is_file(child)) {
            snprintf(child, sizeof(child), "%s/src", dir);
            if (path_is_dir(child)) {
                collect_package_dir(child, true, out);
                return;
            }
        }
    }
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        const char *n = de->d_name;
        if (n[0] == '.') continue;
        if (arrlen(*out) >= ILSP_PACKAGE_MAX_FILES) break;
        int w = snprintf(child, sizeof(child), "%s/%s", dir, n);
        if (w <= 0 || (size_t)w >= sizeof(child)) continue;
        if (path_is_dir(child)) {
            if (vendor && (strcmp(n, "target") == 0 || strcmp(n, "tests") == 0 ||
                           strcmp(n, "examples") == 0)) continue;
            collect_package_dir(child, vendor, out);
        } else {
            size_t len = strlen(n);
            if (len > 5 && strcmp(n + len - 5, ".iron") == 0 && path_is_file(child)) {
                char *copy = strdup(child);
                if (copy) arrput(*out, copy);
            }
        }
    }
    closedir(dp);
}

/* When the document is a source file of a package (under the src/ or
 * vendor/ of the nearest directory with an iron.toml), prepend the
 * package's other files to *source_io, each under a marker naming its
 * path, with a marker for the document itself before its text. Returns
 * false (source untouched) when the document is not part of a package. */
static bool package_prepend(char **source_io, const char *doc_file) {
    char path[4096], root[4096];
    if (!doc_fs_path(doc_file, path, sizeof(path))) return false;
    if (strchr(doc_file, '"') || strchr(doc_file, '\n') || strchr(doc_file, '\r')) return false;
    /* The canonical path, as the workspace index names files: the other
     * files' paths derive from it, and a declaration must carry one name
     * whichever file's analysis reaches it. */
    {
        char resolved[PATH_MAX];
        if (realpath(path, resolved) != NULL && strlen(resolved) < sizeof(path)) {
            memcpy(path, resolved, strlen(resolved) + 1);
            for (char *c = path; *c; c++) if (*c == '\\') *c = '/';
        }
    }

    /* The nearest directory above the file that holds an iron.toml. */
    snprintf(root, sizeof(root), "%s", path);
    const char *rel = NULL;
    for (;;) {
        char *slash = strrchr(root, '/');
        if (!slash || slash == root) return false;
        *slash = '\0';
        char manifest[4200];
        snprintf(manifest, sizeof(manifest), "%s/iron.toml", root);
        if (path_is_file(manifest)) {
            rel = path + strlen(root) + 1;
            break;
        }
    }
    if (strncmp(rel, "src/", 4) != 0 && strncmp(rel, "vendor/", 7) != 0) return false;

    char **files = NULL;
    char dir[4200];
    snprintf(dir, sizeof(dir), "%s/vendor", root);
    if (path_is_dir(dir)) collect_package_dir(dir, true, &files);
    ptrdiff_t vendored = arrlen(files);
    snprintf(dir, sizeof(dir), "%s/src", root);
    collect_package_dir(dir, false, &files);
    if (vendored > 1) qsort(files, (size_t)vendored, sizeof(char *), path_ptr_cmp);
    if (arrlen(files) - vendored > 1)
        qsort(files + vendored, (size_t)(arrlen(files) - vendored), sizeof(char *), path_ptr_cmp);

    size_t cap = 4096, len = 0, total = 0;
    char *buf = (char *)malloc(cap);
    bool any = false;
    for (ptrdiff_t i = 0; buf && i < arrlen(files); i++) {
        const char *fp = files[i];
        if (same_path(fp, path) || strpbrk(fp, "\"\n\r")) continue;
        FILE *f = fopen(fp, "rb");
        if (!f) continue;
        long size = 0;
        if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
        rewind(f);
        if (size < 0 || total + (size_t)size > ILSP_PACKAGE_MAX_BYTES) { fclose(f); continue; }
        size_t need = len + strlen(fp) + (size_t)size + 64;
        if (need > cap) {
            while (cap < need) cap *= 2;
            char *grown = (char *)realloc(buf, cap);
            if (!grown) { fclose(f); free(buf); buf = NULL; break; }
            buf = grown;
        }
        len += (size_t)snprintf(buf + len, cap - len, "-- @file: \"%s\" @line: 1\n", fp);
        size_t got = fread(buf + len, 1, (size_t)size, f);
        fclose(f);
        /* A NUL would end the source early, before the document. */
        for (size_t k = 0; k < got; k++) if (buf[len + k] == '\0') buf[len + k] = ' ';
        len += got;
        buf[len++] = '\n';
        total += got;
        any = true;
    }
    for (ptrdiff_t i = 0; i < arrlen(files); i++) free(files[i]);
    arrfree(files);
    if (!buf) return false;
    if (!any) { free(buf); return false; }

    size_t doc_len = strlen(*source_io);
    size_t need = len + strlen(doc_file) + doc_len + 64;
    char *grown = (char *)realloc(buf, need);
    if (!grown) { free(buf); return false; }
    buf = grown;
    len += (size_t)snprintf(buf + len, need - len, "-- @file: \"%s\" @line: 1\n", doc_file);
    memcpy(buf + len, *source_io, doc_len + 1);
    free(*source_io);
    *source_io = buf;
    return true;
}

/* Where the stdlib is, resolved once per process: every document worker
 * analyzes with it, and a failed lookup should be reported once, not on
 * every keystroke. NULL when there is none; the buffer is then analyzed
 * alone. */
static pthread_once_t g_lib_dir_once = PTHREAD_ONCE_INIT;
static char          *g_lib_dir;

static void resolve_lib_dir(void) {
    g_lib_dir = iron_stdlib_lib_dir();
}

/* Shared analyze primitive -- the SINGLE iron_analyze_buffer call site
 * for the entire src/lsp tree. Both ilsp_facade_compile_pure (discards
 * the program pointer) and ilsp_facade_compile_for_nav (returns it)
 * route through this helper so the CORE-22 grep check stays at 1 hit.
 *
 * The buffer is analyzed with the same stdlib prelude `ironc check` uses
 * (iron_stdlib_prepend), so Map, String methods and imported modules
 * resolve in the editor as they do in the compiler. */
static Iron_Program *facade_analyze(struct IronLsp_Document      *doc,
                                      const IronLsp_CompileRequest *req,
                                      Iron_Arena                   *arena,
                                      Iron_DiagList                *diags) {
    if (!doc || !arena || !diags) return NULL;
    const _Atomic bool *cancel = req ? req->cancel_flag : NULL;
    const char *doc_file = doc->uri ? doc->uri : "<buffer>";

    char *source = NULL;
    int prepended = 0;
    pthread_once(&g_lib_dir_once, resolve_lib_dir);
    if (g_lib_dir) {
        size_t len = doc->text ? doc->text_len : 0;
        source = (char *)malloc(len + 1);
        if (source) {
            if (len) memcpy(source, doc->text, len);
            source[len] = '\0';
            /* A stdlib file is analyzed as itself, whatever directory with
             * an iron.toml lies above it. */
            if (!is_stdlib_file(doc_file, g_lib_dir)) (void)package_prepend(&source, doc_file);
            prepended = iron_stdlib_prepend(&source, doc_file, g_lib_dir);
        }
    }

    /* With no stdlib found, the buffer is analyzed alone. */
    bool with_prelude = source && prepended > 0;
    const char *text = with_prelude ? source : (doc->text ? doc->text : "");
    size_t      text_len = with_prelude ? strlen(source) : doc->text_len;
    Iron_AnalyzeResult r = iron_analyze_buffer(
        text, text_len, doc_file, IRON_ANALYSIS_MODE_LSP, arena, diags, cancel,
        with_prelude ? prepended + 1 : 0);
    /* The arena keeps no pointers into the source text (tokens and the AST
     * copy what they keep), so the prelude buffer can go now. */
    free(source);
    keep_document_diags(diags, doc_file);
    /* A stdlib file opened in the editor (go to definition lands there) is
     * analyzed with a prelude that already contains it, so every
     * declaration in it reports as a duplicate. It is not the user's
     * code: report nothing for it. */
    if (g_lib_dir && is_stdlib_file(doc_file, g_lib_dir)) {
        keep_document_diags(diags, "");
    }
    return r.program;
}

bool ilsp_facade_is_stdlib_path(const char *uri_or_path) {
    if (!uri_or_path || !*uri_or_path) return false;
    if (strncmp(uri_or_path, "stdlib://", 9) == 0) return true;
    pthread_once(&g_lib_dir_once, resolve_lib_dir);
    return g_lib_dir && is_stdlib_file(uri_or_path, g_lib_dir);
}

void ilsp_facade_compile_pure(struct IronLsp_Document      *doc,
                               const IronLsp_CompileRequest *req,
                               Iron_Arena                   *arena,
                               Iron_DiagList                *diags) {
    (void)facade_analyze(doc, req, arena, diags);
}

Iron_Program *ilsp_facade_compile_for_nav(struct IronLsp_Document      *doc,
                                            const IronLsp_CompileRequest *req,
                                            Iron_Arena                   *arena,
                                            Iron_DiagList                *diags) {
    return facade_analyze(doc, req, arena, diags);
}

void ilsp_facade_compile(struct IronLsp_Server   *server,
                          struct IronLsp_Document *doc,
                          const IronLsp_CompileRequest *req) {
    if (!server || !doc) return;
    /* #360: a client that pulls (textDocument/diagnostic) asks for the
     * diagnostics itself after every open and change. Pushing them as well
     * makes it show each one twice, and the analysis would only feed the
     * push, so skip it. */
    if (server->client_pulls_diagnostics) return;

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    ilsp_facade_compile_pure(doc, req, &arena, &diags);

    /* Cancellation check: if the atomic was tripped (HARD-05), drop the
     * payload silently. The client's next edit will schedule a fresh
     * compile. */
    bool cancelled = (req && req->cancel_flag
                       && atomic_load(req->cancel_flag));
    if (cancelled) {
        iron_diaglist_free(&diags);
        iron_arena_free(&arena);
        return;
    }

    /* Build + enqueue textDocument/publishDiagnostics. */
    Iron_Arena build_arena = iron_arena_create(8 * 1024);
    yyjson_mut_doc *mdoc = ilsp_build_publish_diagnostics(
        &diags, doc, server->position_encoding, &build_arena);
    if (mdoc) {
        size_t body_len = 0;
        char *body = ilsp_json_write_mut(mdoc, &build_arena, &body_len);
        enqueue_body(server->writer, ILSP_PRIO_NOTIFICATION, body, body_len);
        yyjson_mut_doc_free(mdoc);
    }

    iron_arena_free(&build_arena);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}

void ilsp_facade_publish_cleared(struct IronLsp_Server *server,
                                 const char            *uri) {
    if (!server || !uri || server->client_pulls_diagnostics) return;
    Iron_Arena build_arena = iron_arena_create(4 * 1024);
    yyjson_alc alc = ilsp_json_alc(&build_arena);
    yyjson_mut_doc *d = yyjson_mut_doc_new(&alc);
    if (d) {
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_doc_set_root(d, root);
        yyjson_mut_obj_add_strcpy(d, root, "jsonrpc", "2.0");
        yyjson_mut_obj_add_strcpy(d, root, "method", "textDocument/publishDiagnostics");
        yyjson_mut_val *params = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, params, "uri", uri);
        yyjson_mut_obj_add_val(d, params, "diagnostics", yyjson_mut_arr(d));
        yyjson_mut_obj_add_val(d, root, "params", params);
        size_t body_len = 0;
        char *body = ilsp_json_write_mut(d, &build_arena, &body_len);
        enqueue_body(server->writer, ILSP_PRIO_NOTIFICATION, body, body_len);
        yyjson_mut_doc_free(d);
    }
    iron_arena_free(&build_arena);
}

void ilsp_facade_pull_diagnostic_empty(struct IronLsp_Server *server,
                                        const char            *request_id) {
    if (!server || !request_id) return;
    Iron_Arena build_arena = iron_arena_create(4 * 1024);
    yyjson_mut_doc *mdoc = ilsp_build_pull_diagnostic_report(
        NULL, NULL, server->position_encoding, request_id, &build_arena);
    if (mdoc) {
        size_t body_len = 0;
        char *body = ilsp_json_write_mut(mdoc, &build_arena, &body_len);
        enqueue_body(server->writer, ILSP_PRIO_RESPONSE, body, body_len);
        yyjson_mut_doc_free(mdoc);
    }
    iron_arena_free(&build_arena);
}

void ilsp_facade_pull_diagnostic(struct IronLsp_Server   *server,
                                  struct IronLsp_Document *doc,
                                  const char              *request_id) {
    if (!server || !doc || !request_id) return;

    Iron_Arena    arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    /* Quarantined doc: return an empty-items report with the current
     * version's resultId. */
    if (!atomic_load(&doc->quarantined)) {
        IronLsp_CompileRequest req = { .version = doc->version,
                                        .cancel_flag = NULL };
        ilsp_facade_compile_pure(doc, &req, &arena, &diags);
    }

    Iron_Arena build_arena = iron_arena_create(8 * 1024);
    yyjson_mut_doc *mdoc = ilsp_build_pull_diagnostic_report(
        &diags, doc, server->position_encoding, request_id, &build_arena);
    if (mdoc) {
        size_t body_len = 0;
        char *body = ilsp_json_write_mut(mdoc, &build_arena, &body_len);
        enqueue_body(server->writer, ILSP_PRIO_RESPONSE, body, body_len);
        yyjson_mut_doc_free(mdoc);
    }

    iron_arena_free(&build_arena);
    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
}
