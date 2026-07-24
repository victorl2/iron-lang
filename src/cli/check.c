#include "cli/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <unistd.h>
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
#include "diagnostics/diagnostics.h"
#include "util/arena.h"
#include "vendor/stb_ds.h"
#include "cli/iron_import_detect.h"

/* Phase 93 VIS-03 stdlib carve-out: count '\n' bytes in a buffer. Mirrors
 * the helper in build.c so check.c can plumb an equivalent
 * user_source_start_line to the parser without depending on build.c. */
static int check_count_newlines(const char *buf, size_t len) {
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') n++;
    }
    return n;
}

/* ── Helper: read a file into a heap-allocated string ────────────────────── */

static char *check_read_file(const char *path) {
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
    return buf;
}

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

/* iron_lib_dir_has_stdlib: a candidate base directory only counts as the
   Iron lib root when the stdlib is actually inside it (sentinel:
   stdlib/string.iron). Existence of the directory alone is not enough —
   a binary living in a filesystem-root build dir (e.g. /build) would
   otherwise resolve ../lib to the system /lib and mask the
   IRON_SOURCE_DIR fallback. */
static int iron_lib_dir_has_stdlib(const char *base) {
    char probe[4096];
    int n = snprintf(probe, sizeof(probe), "%s/stdlib/string.iron", base);
    if (n < 0 || (size_t)n >= sizeof(probe)) return 0;
    struct stat st;
    return stat(probe, &st) == 0 && S_ISREG(st.st_mode);
}

/* get_iron_lib_dir: returns malloc'd path to the lib/ or src/ base directory.
   Resolution order: $IRON_LIB_DIR override, sibling ../lib/ of the binary
   directory (installed layout), then the IRON_SOURCE_DIR compile-time
   define (dev builds). Every candidate must contain the stdlib; returns
   NULL (with a diagnostic) when none does. Caller must free(). */
static char *get_iron_lib_dir(void) {
    const char *env_dir = getenv("IRON_LIB_DIR");
    if (env_dir && *env_dir) {
        if (iron_lib_dir_has_stdlib(env_dir)) return strdup(env_dir);
        fprintf(stderr,
                "warning: IRON_LIB_DIR='%s' does not contain stdlib/string.iron; "
                "falling back to auto-detection\n",
                env_dir);
    }
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
            if (iron_lib_dir_has_stdlib(lib_path)) {
                return lib_path;
            }
            free(lib_path);
        }
    }
    /* Fallback: compile-time IRON_SOURCE_DIR (dev builds) */
    if (iron_lib_dir_has_stdlib(IRON_SOURCE_DIR)) {
        return strdup(IRON_SOURCE_DIR);
    }
    fprintf(stderr,
            "error: cannot locate the Iron stdlib: no stdlib/string.iron under "
            "$IRON_LIB_DIR, next to the binary (../lib), or in '%s'.\n"
            "Reinstall Iron or set IRON_LIB_DIR to a directory containing "
            "stdlib/.\n",
            IRON_SOURCE_DIR);
    return NULL;
}

/* ── Helper: build a path from a base directory and relative path ─────────── */

static char *check_make_path(const char *base, const char *rel) {
    size_t base_len = strlen(base);
    size_t rel_len  = strlen(rel);
    char *out = (char *)malloc(base_len + 1 + rel_len + 1);
    if (!out) return NULL;
    memcpy(out, base, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, rel, rel_len + 1);
    return out;
}

/* ── Helper: read a file with size output ────────────────────────────────── */

static char *check_read_stdlib(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)(size + 1));
    if (!buf) { fclose(f); return NULL; }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[nread] = '\0';
    if (out_size) *out_size = (long)nread;
    return buf;
}

/* ── 2026-07 diagnostics remediation: stdlib-prepend line mapping ──────────
 * Mirrors build.c (see the block comment above prepend_marked_file there):
 * every prepended stdlib file is wrapped in a `-- @file: "<path>" @line: 1`
 * marker and one final marker naming the user's own file re-enters user
 * code, so the lexer reports per-file 1-based line numbers instead of
 * concatenated-TU numbers. Prepending logic is duplicated per the
 * established build.c/check.c mirroring convention (Pitfall 4 comments
 * below). */

static int check_path_marker_safe(const char *path) {
    return path && *path && strchr(path, '"') == NULL &&
           strchr(path, '\n') == NULL && strchr(path, '\r') == NULL;
}

/* Prepend `-- @file: "<source_path>" @line: 1` to *source_io. Returns lines
 * added (1) or 0 on failure/unsafe path (source left unchanged). */
static int check_prepend_user_line_marker(char **source_io,
                                          const char *source_path) {
    if (!source_io || !*source_io || !check_path_marker_safe(source_path)) return 0;
    size_t mlen = strlen("-- @file: \"") + strlen(source_path) + strlen("\" @line: 1\n");
    char *combined = (char *)malloc(mlen + strlen(*source_io) + 1);
    if (!combined) return 0;
    int n = snprintf(combined, mlen + 1, "-- @file: \"%s\" @line: 1\n", source_path);
    if (n < 0 || (size_t)n != mlen) { free(combined); return 0; }
    strcpy(combined + mlen, *source_io);
    free(*source_io);
    *source_io = combined;
    return 1;
}

/* Read base_dir/rel and prepend it (marker line first when `markers`) to
 * *source_io. Returns physical lines added (marker + content + separator),
 * or 0 on read/alloc failure (source left unchanged — legacy skip). */
static int check_prepend_marked_file(char **source_io, const char *base_dir,
                                     const char *rel, bool markers) {
    char *path = check_make_path(base_dir, rel);
    if (!path) return 0;
    long sz = 0;
    char *src = check_read_stdlib(path, &sz);
    if (!src) { free(path); return 0; }

    char marker[4224];
    size_t mlen = 0;
    if (markers && check_path_marker_safe(path)) {
        int n = snprintf(marker, sizeof(marker), "-- @file: \"%s\" @line: 1\n", path);
        if (n > 0 && (size_t)n < sizeof(marker)) mlen = (size_t)n;
    }

    size_t combined_len = mlen + (size_t)sz + 1 + strlen(*source_io) + 1;
    char *combined = (char *)malloc(combined_len);
    int added = 0;
    if (combined) {
        if (mlen > 0) memcpy(combined, marker, mlen);
        memcpy(combined + mlen, src, (size_t)sz);
        combined[mlen + (size_t)sz] = '\n';
        strcpy(combined + mlen + (size_t)sz + 1, *source_io);
        free(*source_io);
        *source_io = combined;
        added = (mlen > 0 ? 1 : 0) + check_count_newlines(src, (size_t)sz) + 1;
    }
    free(src);
    free(path);
    return added;
}

/* ── Check: lex + parse + analyze, no codegen ────────────────────────────── */

int iron_check(const char *source_path, bool verbose, bool strict_v3) {
    /* Phase 9 D-11 (Option A): the strict_v3 flag is now threaded into
     * iron_analyze_buffer via the IronAnalysisMode enum. CLI mode keeps
     * the legacy strict-v3 grammar enforcement; CLI_LENIENT honors
     * `ironc check --lenient`. Mapping below; consumed at the
     * iron_analyze_buffer call site near the bottom of this function. */
    IronAnalysisMode analysis_mode = strict_v3 ? IRON_ANALYSIS_MODE_CLI
                                                : IRON_ANALYSIS_MODE_CLI_LENIENT;
    /* Resolve runtime lib/src base directory once for this check */
    char *base_dir = get_iron_lib_dir();
    if (!base_dir) return 1;

    /* 1. Read source file */
    char *source = check_read_file(source_path);
    if (!source) { free(base_dir); return 1; }

    /* Detect stdlib imports and prepend .iron wrappers (same as build.c).
     * Use a temporary arena for the token-level import detection. */
    Iron_Arena detect_arena = iron_arena_create(32 * 1024);

    /* Phase 93 VIS-03 stdlib carve-out: total lines added to the prefix by
     * all stdlib prepends. Mirrors build.c. */
    int stdlib_prepended_lines = 0;

    /* 2026-07 diagnostics remediation: seat the user-file line-mapping marker
     * directly above the user's source BEFORE any stdlib prepend (mirrors
     * build.c — see prepend_user_line_marker there). */
    bool line_markers = check_prepend_user_line_marker(&source, source_path) == 1;
    if (line_markers) stdlib_prepended_lines += 1;

    if (iron_detect_import(source, source_path, "raylib", &detect_arena)) {
        stdlib_prepended_lines +=
            check_prepend_marked_file(&source, base_dir, "stdlib/raylib.iron",
                                      line_markers);
    }

    if (iron_detect_import(source, source_path, "math", &detect_arena)) {
        stdlib_prepended_lines +=
            check_prepend_marked_file(&source, base_dir, "stdlib/math.iron",
                                      line_markers);
    }

    if (iron_detect_import(source, source_path, "io", &detect_arena)) {
        stdlib_prepended_lines +=
            check_prepend_marked_file(&source, base_dir, "stdlib/io.iron",
                                      line_markers);
    }

    if (iron_detect_import(source, source_path, "time", &detect_arena)) {
        stdlib_prepended_lines +=
            check_prepend_marked_file(&source, base_dir, "stdlib/time.iron",
                                      line_markers);
    }

    if (iron_detect_import(source, source_path, "log", &detect_arena)) {
        stdlib_prepended_lines +=
            check_prepend_marked_file(&source, base_dir, "stdlib/log.iron",
                                      line_markers);
    }

    /* Phase 59 02: detect "import net" and prepend net.iron so `iron check`
     * sees the Net/TcpSocket/TcpListener/NetError decls that live in stdlib
     * rather than in user source. */
    if (iron_detect_import(source, source_path, "net", &detect_arena)) {
        stdlib_prepended_lines +=
            check_prepend_marked_file(&source, base_dir, "stdlib/net.iron",
                                      line_markers);
    }

    /* Phase 59 05: detect "import url" and prepend url.iron (pure Iron,
     * no C backing) so `iron check` sees the Url/Url_Builder/UrlError
     * decls. String primitives rindex_of / byte_at / from_byte used by
     * url.iron come from the unconditional string.iron prepend below. */
    if (iron_detect_import(source, source_path, "url", &detect_arena)) {
        stdlib_prepended_lines +=
            check_prepend_marked_file(&source, base_dir, "stdlib/url.iron",
                                      line_markers);
    }

    iron_arena_free(&detect_arena);

    /* Always prepend string.iron — String methods are available without import */
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/string.iron",
                                  line_markers);

    /* Phase 78 FMT: always prepend int.iron — Int.to_string / Int32.to_string
     * available without an explicit import. Mirror of build.c block 1j. */
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/int.iron",
                                  line_markers);

    /* Phase 78 FMT: always prepend float.iron — Float.to_string available
     * without an explicit import. Mirror of build.c block 1k. */
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/float.iron",
                                  line_markers);

    /* Phase 25 STDLIB-05: always prepend box.iron — Box[T] is available on
     * any source file that uses Box.new / Box.unwrap / Box.free / Box.null /
     * Box.is_null without an explicit import. Mirror of build.c arm 1l.
     * ANTI-PATTERN: prepending ONLY in build.c misses the check.c arm;
     * iron_analyze_buffer (CORE-22 LSP facade) calls check.c path and would
     * fail to resolve Box[T] usage if this block is absent. */
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/box.iron",
                                  line_markers);

    /* Phase 28 STDLIB-06 (Plan 28-03): always prepend arena.iron — Arena /
     * ArenaSave + Arena.new / new_threadsafe / with_capacity / save / restore /
     * reset / used / capacity are available on any source file that uses an
     * arena without an explicit import. Mirror of build.c arm 1m.
     * ANTI-PATTERN (Pitfall 4): prepending ONLY in build.c misses the check.c
     * arm; iron_analyze_buffer (CORE-22 LSP facade) routes through this path
     * and would fail to resolve Arena usage if this block is absent. */
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/arena.iron",
                                  line_markers);

    /* Phase 33 OQ-01 (Plan 33-02): always prepend hashable.iron — the Hashable
     * constraint interface must resolve as IRON_SYM_INTERFACE BEFORE any Map/Set
     * instantiation is checked, else the K: Hashable bound silently passes
     * (type_satisfies_constraint returns true for an unresolved constraint).
     * Ordered before map.iron/set.iron below. Mirror of build.c arm 1n.
     * ANTI-PATTERN (Pitfall 4, Phase 25-03 / 28-03): prepending ONLY in build.c
     * misses the check.c arm; iron_analyze_buffer (CORE-22 LSP facade) routes
     * through this path and would fail to resolve the Hashable bound. */
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/hashable.iron",
                                  line_markers);

    /* Phase 33 STDLIB-03/04 (Plan 33-02): always prepend map.iron + set.iron —
     * Map[K: Hashable, V] / Set[T: Hashable] declare the generic bound so the
     * OQ-01 constraint check fires at user instantiation sites. MUST come after
     * hashable.iron above (the Hashable interface must resolve first). Mirror of
     * build.c arms 1o/1p. */
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/map.iron",
                                  line_markers);
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/set.iron",
                                  line_markers);

    /* Phase 33 STDLIB-07/08/09 (Plan 33-05): always prepend the nocopy resource
     * types — Mutex[T] / RWLock[T] / Channel[T] / FileHandle. Each is a nocopy
     * object surface whose C backing is synthesized by emit_ensure_* in
     * emit_helpers.c; the by-name dispatch in typecheck.c recognizes
     * Mutex.new / m.lock / Channel.new / FileHandle.open / etc. Mirror of build.c.
     * ANTI-PATTERN (Pitfall 4): prepending ONLY in build.c misses the check.c arm;
     * iron_analyze_buffer (CORE-22 LSP facade) routes through this path. */
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/mutex.iron",
                                  line_markers);
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/rwlock.iron",
                                  line_markers);
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/channel.iron",
                                  line_markers);
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/filehandle.iron",
                                  line_markers);

    /* Phase 33 STDLIB-10 (Plan 33-06): always prepend rawptr.iron — RawPtr is
     * the type-erased member of the *unchecked T regime. Its `RawPtr.of(x)`
     * compiler-builtin is dispatched in typecheck.c (mirrors Box.new / Ptr.cast
     * precedent) and the by-name dispatch needs the `RawPtr` symbol in scope to
     * resolve the type annotation `val raw: RawPtr`. Mirror of build.c arm.
     * ANTI-PATTERN (Pitfall 4): prepending ONLY in build.c misses the check.c
     * arm; iron_analyze_buffer (CORE-22 LSP facade) routes through this path. */
    stdlib_prepended_lines +=
        check_prepend_marked_file(&source, base_dir, "stdlib/rawptr.iron",
                                  line_markers);

    /* 2. Set up arena and diagnostics */
    Iron_Arena arena = iron_arena_create(64 * 1024);
    Iron_DiagList diags = iron_diaglist_create();

    /* 3. Analyze — single call, no bypass paths (HARD-01).
     * Phase 9 D-11: analysis_mode encodes strict_v3 via the
     * IronAnalysisMode enum (CLI for strict, CLI_LENIENT for lenient). */
    Iron_AnalyzeResult result = iron_analyze_buffer(
        source, strlen(source), source_path,
        analysis_mode,
        &arena, &diags,
        NULL,
        stdlib_prepended_lines + 1);

    /* 4. Print all diagnostics */
    iron_diag_print_all(&diags, source);

    /* 7. Verbose: print analysis summary */
    if (verbose) {
        fprintf(stderr, "check: %s\n", source_path);
        fprintf(stderr, "  errors:   %d\n", diags.error_count);
        fprintf(stderr, "  warnings: %d\n", diags.warning_count);
        if (result.global_scope) {
            fprintf(stderr, "  analysis: complete (global scope established)\n");
        }
    }

    int exit_code = (diags.error_count > 0 || result.has_errors) ? 1 : 0;

    iron_diaglist_free(&diags);
    iron_arena_free(&arena);
    free(source);
    free(base_dir);
    return exit_code;
}
