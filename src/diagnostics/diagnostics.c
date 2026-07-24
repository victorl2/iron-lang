#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "stb_ds.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Span helpers ────────────────────────────────────────────────────────── */

Iron_Span iron_span_make(const char *filename,
                          uint32_t line, uint32_t col,
                          uint32_t end_line, uint32_t end_col) {
    Iron_Span s;
    s.filename = filename;
    s.line     = line;
    s.col      = col;
    s.end_line = end_line;
    s.end_col  = end_col;
    return s;
}

Iron_Span iron_span_merge(Iron_Span start, Iron_Span end) {
    Iron_Span s;
    s.filename = start.filename;
    s.line     = start.line;
    s.col      = start.col;
    s.end_line = end.end_line;
    s.end_col  = end.end_col;
    return s;
}

/* ── DiagList ────────────────────────────────────────────────────────────── */

Iron_DiagList iron_diaglist_create(void) {
    Iron_DiagList list;
    list.items         = NULL; /* stb_ds empty array */
    list.count         = 0;
    list.error_count   = 0;
    list.warning_count = 0;
    return list;
}

void iron_diag_emit(Iron_DiagList *list,
                    Iron_Arena    *arena,
                    Iron_DiagLevel level,
                    int            code,
                    Iron_Span      span,
                    const char    *message,
                    const char    *suggestion) {
    Iron_Diagnostic d;
    d.level      = level;
    d.code       = code;
    d.span       = span;
    d.message    = iron_arena_strdup(arena, message, strlen(message));
    d.suggestion = (suggestion != NULL)
                       ? iron_arena_strdup(arena, suggestion, strlen(suggestion))
                       : NULL;

    arrput(list->items, d);
    list->count += 1;

    switch (level) {
    case IRON_DIAG_ERROR:   list->error_count   += 1; break;
    case IRON_DIAG_WARNING: list->warning_count += 1; break;
    case IRON_DIAG_NOTE:    break; /* notes do not affect error/warning counts */
    }
}

/* ── Printing ────────────────────────────────────────────────────────────── */

/* Returns 1 if stderr supports ANSI color (isatty). */
static int use_color(void) {
    return isatty(STDERR_FILENO);
}

/* ── 2026-07 diagnostics remediation: stdlib-prepend line mapping ──────────
 *
 * The CLI passes the CONCATENATED compile buffer (prepended stdlib sources +
 * user source) as source_text, while spans now carry per-file identity: the
 * lexer re-tags filename AND resets its logical line counter at the
 * `-- @file: "<path>" @line: <n>` markers build.c / check.c wrap around each
 * prepended file (plain Phase 93 `-- @file: <name>` markers re-tag the
 * filename only and resync logical numbering to the physical count). To show
 * the right context/caret excerpt, the renderer replays those marker
 * semantics over source_text and maps the span's (filename, logical line)
 * back to the physical buffer line.
 *
 * Sources without markers resolve to physical == logical, i.e. the exact
 * pre-remediation behavior (unit tests, LSP buffers, fmt). */

/* Parse one physical line as a `-- @file:` marker.
 * Grammar (whitespace-tolerant):  -- @file: <name> [@line: <n>]
 * where <name> is bare (up to whitespace) or quoted ("..." — may contain
 * spaces). On success returns 1 and fills *name_out, *name_len_out and
 * *line_reset_out (0 when no `@line:` suffix). */
static int parse_file_marker(const char *line, size_t len,
                             const char **name_out, size_t *name_len_out,
                             uint32_t *line_reset_out) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i + 2 > len || line[i] != '-' || line[i + 1] != '-') return 0;
    i += 2;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    static const char k_file[] = "@file:";
    const size_t k_file_len = sizeof(k_file) - 1;
    if (i + k_file_len > len || memcmp(line + i, k_file, k_file_len) != 0) return 0;
    i += k_file_len;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;

    size_t name_start, name_len;
    if (i < len && line[i] == '"') {
        i++;
        name_start = i;
        while (i < len && line[i] != '"' && line[i] != '\r') i++;
        name_len = i - name_start;
        if (i < len && line[i] == '"') i++;
        else return 0; /* unterminated quote: not a well-formed marker */
    } else {
        name_start = i;
        while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') i++;
        name_len = i - name_start;
    }
    if (name_len == 0) return 0;

    uint32_t reset = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    static const char k_line[] = "@line:";
    const size_t k_line_len = sizeof(k_line) - 1;
    if (i + k_line_len <= len && memcmp(line + i, k_line, k_line_len) == 0) {
        i += k_line_len;
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        uint32_t n = 0;
        int any = 0;
        while (i < len && line[i] >= '0' && line[i] <= '9') {
            if (n < 100000000u) n = n * 10u + (uint32_t)(line[i] - '0');
            any = 1;
            i++;
        }
        if (any && n > 0) reset = n;
    }

    *name_out       = line + name_start;
    *name_len_out   = name_len;
    *line_reset_out = reset;
    return 1;
}

/* Map (filename, logical line) to the physical line number in source_text by
 * replaying the lexer's marker semantics. Falls back to `logical` (the exact
 * legacy behavior) when no matching line exists. */
static uint32_t resolve_physical_line(const char *source_text,
                                      const char *filename,
                                      uint32_t logical) {
    if (!source_text || logical == 0) return logical;

    const char *p           = source_text;
    uint32_t    phys        = 1;
    uint32_t    cur_logical = 1;
    const char *cur_name     = NULL; /* NULL = entry region: match any file */
    size_t      cur_name_len = 0;

    while (*p != '\0') {
        const char *eol     = strchr(p, '\n');
        size_t      linelen = eol ? (size_t)(eol - p) : strlen(p);

        const char *mname;
        size_t      mlen;
        uint32_t    lreset;
        if (parse_file_marker(p, linelen, &mname, &mlen, &lreset)) {
            cur_name     = mname;
            cur_name_len = mlen;
            /* @line marker: next physical line reports as line <n>.
             * Plain marker: physical resync (lexer sets line = phys_line at
             * the marker line; the newline bump makes the next line phys+1). */
            cur_logical = (lreset > 0) ? lreset : phys + 1;
        } else {
            int name_match =
                (filename == NULL) || (cur_name == NULL) ||
                (strlen(filename) == cur_name_len &&
                 strncmp(filename, cur_name, cur_name_len) == 0);
            if (name_match && cur_logical == logical) return phys;
            cur_logical++;
        }

        if (!eol) break;
        p = eol + 1;
        phys++;
    }
    return logical;
}

/* Is the physical line `lineno` of source_text a `-- @file:` marker line?
 * Used to suppress marker lines from the context window (they always sit on
 * file boundaries, so the "line above"/"line below" of a first/last file
 * line would otherwise leak the synthetic marker). */
static int physical_line_is_marker(const char *line_text) {
    const char *mname;
    size_t      mlen;
    uint32_t    lreset;
    return parse_file_marker(line_text, strlen(line_text),
                             &mname, &mlen, &lreset);
}

/* Extract the Nth line (1-indexed) from source_text.
 * Writes up to buf_size-1 chars into buf and null-terminates.
 * Returns 0 if line not found.
 */
static int get_source_line(const char *source_text, uint32_t lineno,
                            char *buf, size_t buf_size) {
    const char *p     = source_text;
    uint32_t    cur   = 1;

    while (*p != '\0' && cur < lineno) {
        if (*p == '\n') {
            cur++;
        }
        p++;
    }

    if (*p == '\0' && cur < lineno) {
        return 0; /* line not found */
    }

    /* Copy until newline or end. */
    size_t i = 0;
    while (*p != '\0' && *p != '\n' && i < buf_size - 1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    return 1;
}

void iron_diag_print(const Iron_Diagnostic *d, const char *source_text) {
    int color = use_color();

    const char *level_str;
    const char *color_start = "";
    const char *color_end   = "";

    if (color) {
        color_end = "\033[0m";
    }

    switch (d->level) {
    case IRON_DIAG_ERROR:
        level_str   = "error";
        if (color) color_start = "\033[1;31m";
        break;
    case IRON_DIAG_WARNING:
        level_str   = "warning";
        if (color) color_start = "\033[1;33m";
        break;
    case IRON_DIAG_NOTE:
        level_str   = "note";
        if (color) color_start = "\033[1;36m";
        break;
    default:
        level_str = "unknown";
        break;
    }

    /* Header: "error[E0001]: message" / "warning[W0605]: message".
     * Phase 28 (Plan 28-03): warnings carry the `W` code prefix (not `E`) so
     * the W06xx warning range is distinguishable in tool output and matches
     * the fixture/ctest contract (W0605 ARENA-09). Errors and notes keep `E`. */
    char code_letter = (d->level == IRON_DIAG_WARNING) ? 'W' : 'E';
    fprintf(stderr, "%s%s[%c%04d]%s: %s\n",
            color_start, level_str, code_letter, d->code, color_end, d->message);

    /* Location: "  --> file.iron:5:10" */
    fprintf(stderr, "  --> %s:%u:%u\n",
            d->span.filename ? d->span.filename : "<unknown>",
            d->span.line,
            d->span.col);

    /* Source context: up to 3 lines (line above, error line, line below).
     * 2026-07 diagnostics remediation: spans carry per-file line numbers
     * while source_text is the concatenated compile buffer, so the span's
     * (filename, line) is first mapped back to the physical buffer line
     * (identity for marker-less sources). Labels stay in the span's own
     * (per-file) numbering; `-- @file:` marker lines are suppressed from
     * the context window (they sit on file boundaries). */
    if (source_text != NULL) {
        char line_buf[512];
        uint32_t error_line = d->span.line;
        uint32_t phys_line  = resolve_physical_line(source_text,
                                                    d->span.filename,
                                                    error_line);

        /* Line above (if it exists and is not a file-boundary marker). */
        if (error_line > 1 && phys_line > 1) {
            if (get_source_line(source_text, phys_line - 1, line_buf, sizeof(line_buf)) &&
                !physical_line_is_marker(line_buf)) {
                fprintf(stderr, "%5u | %s\n", error_line - 1, line_buf);
            }
        }

        /* Error line. */
        if (get_source_line(source_text, phys_line, line_buf, sizeof(line_buf))) {
            fprintf(stderr, "%5u | %s\n", error_line, line_buf);

            /* Caret pointing to the column. */
            fprintf(stderr, "      | ");
            uint32_t col = d->span.col > 0 ? d->span.col : 1;
            for (uint32_t i = 1; i < col; i++) {
                fprintf(stderr, " ");
            }
            fprintf(stderr, "%s^%s\n", color_start, color_end);
        }

        /* Line below (unless it is a file-boundary marker). */
        if (get_source_line(source_text, phys_line + 1, line_buf, sizeof(line_buf)) &&
            !physical_line_is_marker(line_buf)) {
            fprintf(stderr, "%5u | %s\n", error_line + 1, line_buf);
        }
    }

    /* Suggestion. */
    if (d->suggestion != NULL) {
        fprintf(stderr, "      = help: %s\n", d->suggestion);
    }

    fprintf(stderr, "\n");
}

void iron_diag_print_all(const Iron_DiagList *list, const char *source_text) {
    int n = arrlen(list->items);
    for (int i = 0; i < n; i++) {
        iron_diag_print(&list->items[i], source_text);
    }
}

void iron_diaglist_free(Iron_DiagList *list) {
    arrfree(list->items);
    list->items         = NULL;
    list->count         = 0;
    list->error_count   = 0;
    list->warning_count = 0;
}

/* ── Internal compiler error (PROT-03) ───────────────────────────────────── */

void iron_ice(const char *fmt, ...) {
    fprintf(stderr, "iron: internal compiler error: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
    abort();
}

/* ── AST node kind assertion impl (PROT-03) ──────────────────────────────── */

/* Forward declared in src/parser/ast.h. Lives here (not in ast.h) so the impl
 * can call iron_ice without pulling the diagnostics surface into ast.h as a
 * transitive dependency. ast.h already includes diagnostics.h for Iron_Span,
 * so the declaration of iron_node_assert_kind_impl piggybacks on that include. */
void iron_node_assert_kind_impl(const Iron_Node *node,
                                Iron_NodeKind expected,
                                const char *file,
                                int line,
                                const char *func) {
    if (!node) {
        iron_ice("iron_node_assert_kind: NULL node (expected kind %d) at %s:%d in %s",
                 (int)expected, file, line, func);
    }
    if (node->kind != expected) {
        iron_ice("iron_node_assert_kind: expected kind %d, got %d at %s:%d in %s",
                 (int)expected, (int)node->kind, file, line, func);
    }
}
