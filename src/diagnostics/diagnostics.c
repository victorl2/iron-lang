#include "diagnostics/diagnostics.h"
#include "parser/ast.h"
#include "stb_ds.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/os.h"

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

static const char *diag_strdup(Iron_Arena *arena, const char *s) {
    return s ? iron_arena_strdup(arena, s, strlen(s)) : NULL;
}

void iron_diag_emit(Iron_DiagList *list,
                    Iron_Arena    *arena,
                    Iron_DiagLevel level,
                    int            code,
                    Iron_Span      span,
                    const char    *message,
                    const char    *suggestion) {
    Iron_Span none = {0};
    iron_diag_emit_labeled(list, arena, level, code, span, message,
                           NULL, none, NULL, NULL, suggestion);
}

void iron_diag_emit_labeled(Iron_DiagList *list,
                            Iron_Arena    *arena,
                            Iron_DiagLevel level,
                            int            code,
                            Iron_Span      span,
                            const char    *message,
                            const char    *label,
                            Iron_Span      related_span,
                            const char    *related_label,
                            const char    *note,
                            const char    *suggestion) {
    /* The same error at the same place once is enough: the resolver and the
     * checker both report some errors (an unknown enum variant was printed
     * twice). Only the last 1024 entries are compared, so a runaway error
     * stream stays linear. */
    for (int i = list->count - 1; i >= 0 && i >= list->count - 1024; i--) {
        const Iron_Diagnostic *o = &list->items[i];
        if (o->code == code && o->level == level &&
            o->span.line == span.line && o->span.col == span.col &&
            o->span.end_line == span.end_line && o->span.end_col == span.end_col &&
            ((o->span.filename == span.filename) ||
             (o->span.filename && span.filename && strcmp(o->span.filename, span.filename) == 0)) &&
            o->message && message && strcmp(o->message, message) == 0)
            return;
    }
    Iron_Diagnostic d;
    memset(&d, 0, sizeof(d));
    d.level      = level;
    d.code       = code;
    d.span       = span;
    d.message    = iron_arena_strdup(arena, message, strlen(message));
    d.suggestion = diag_strdup(arena, suggestion);
    d.label         = diag_strdup(arena, label);
    d.related_span  = related_span;
    d.related_label = diag_strdup(arena, related_label);
    d.note          = diag_strdup(arena, note);

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

/* Process wide, set once at startup (or around a unit test). */
static bool     g_marker_test_mode;
static bool     g_marker_packed_source;
static uint32_t g_diag_user_start_line = 1;

void iron_file_markers_set_test_mode(bool on) { g_marker_test_mode = on; }
bool iron_file_markers_test_mode(void) { return g_marker_test_mode; }
void iron_file_markers_set_packed_source(bool on) { g_marker_packed_source = on; }
bool iron_file_markers_packed_source(void) { return g_marker_packed_source; }
void iron_diag_set_user_start_line(uint32_t line) {
    g_diag_user_start_line = line > 0 ? line : 1;
}

size_t iron_comment_intro_len(const char *text, size_t len) {
    if (!text || len < 2) return 0;
    if (text[0] == '-' && text[1] == '-') return 2;
    /* `///` opens a doc comment, which is a token of its own. */
    if (text[0] == '/' && text[1] == '/' && !(len > 2 && text[2] == '/')) return 2;
    return 0;
}

static size_t marker_skip_blanks(const char *s, size_t i, size_t len) {
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* `<key> <digits>` at s[*i]: reads the number and moves *i past it. */
static bool marker_number(const char *s, size_t *i, size_t len,
                          const char *key, uint32_t *out) {
    size_t klen = strlen(key);
    size_t j = *i;
    if (j + klen > len || memcmp(s + j, key, klen) != 0) return false;
    j = marker_skip_blanks(s, j + klen, len);
    uint32_t n = 0;
    bool any = false;
    while (j < len && s[j] >= '0' && s[j] <= '9') {
        if (n < 100000000u) n = n * 10u + (uint32_t)(s[j] - '0');
        any = true;
        j++;
    }
    if (!any) return false;
    *i = j;
    *out = n;
    return true;
}

bool iron_file_marker_parse(const char *text, size_t len, Iron_FileMarker *out) {
    static const char k_file[] = "@file:";
    const size_t k_file_len = sizeof(k_file) - 1;
    if (!text || !out) return false;

    size_t i = marker_skip_blanks(text, 0, len);
    if (i + k_file_len > len || memcmp(text + i, k_file, k_file_len) != 0) return false;
    i = marker_skip_blanks(text, i + k_file_len, len);

    size_t name_start, name_len;
    if (i < len && text[i] == '"') {
        i++;
        name_start = i;
        while (i < len && text[i] != '"' && text[i] != '\n' && text[i] != '\r') i++;
        name_len = i - name_start;
        if (i < len && text[i] == '"') i++;
        else return false; /* unterminated quote: not a well-formed marker */
    } else {
        name_start = i;
        while (i < len && text[i] != ' ' && text[i] != '\t' &&
               text[i] != '\n' && text[i] != '\r') i++;
        name_len = i - name_start;
    }
    if (name_len == 0) return false;

    out->name       = text + name_start;
    out->name_len   = name_len;
    out->line_reset = 0;
    out->has_lines  = false;
    out->lines      = 0;
    out->packed     = false;

    uint32_t n = 0;
    i = marker_skip_blanks(text, i, len);
    if (marker_number(text, &i, len, "@line:", &n)) {
        out->line_reset = n; /* 0 is not a line: no reset */
        i = marker_skip_blanks(text, i, len);
    }
    if (marker_number(text, &i, len, "@lines:", &n)) {
        out->has_lines = true;
        out->lines     = n;
        i = marker_skip_blanks(text, i, len);
    }
    static const char k_packed[] = "@packed";
    const size_t k_packed_len = sizeof(k_packed) - 1;
    if (i + k_packed_len <= len && memcmp(text + i, k_packed, k_packed_len) == 0 &&
        (i + k_packed_len == len || text[i + k_packed_len] == ' ' ||
         text[i + k_packed_len] == '\t' || text[i + k_packed_len] == '\r')) {
        out->packed = true;
    }
    return true;
}

Iron_FileMarkerGate iron_file_marker_gate(uint32_t user_start_line) {
    Iron_FileMarkerGate g;
    g.test_mode  = g_marker_test_mode;
    g.packed     = g_marker_packed_source;
    g.user_start = user_start_line > 0 ? user_start_line : 1;
    g.trust_from = g.user_start;
    return g;
}

bool iron_file_marker_admit(Iron_FileMarkerGate *gate, uint32_t phys_line,
                            const Iron_FileMarker *m) {
    if (!gate || !m) return false;
    if (gate->test_mode) return true;
    /* The prelude above the user's source is the compiler's own text. Its
     * marker for the user's file says when that source is a packed one. */
    if (phys_line < gate->user_start) {
        if (m->packed) gate->packed = true;
        return true;
    }
    if (!gate->packed || phys_line < gate->trust_from) return false;
    /* Between the files of a packed source. The file under this marker is
     * `lines` long; without a length it runs to the end of the buffer. */
    if (!m->has_lines || m->lines >= UINT32_MAX - phys_line - 1) {
        gate->trust_from = UINT32_MAX;
    } else {
        gate->trust_from = phys_line + 1 + m->lines;
    }
    return true;
}

/* The honored marker on one physical line of the buffer, if there is one.
 * Only a comment that starts its line counts here (the markers the compiler
 * and the harness write always do). */
static bool honored_marker_on_line(Iron_FileMarkerGate *gate, const char *line,
                                   size_t len, uint32_t phys,
                                   Iron_FileMarker *out) {
    size_t i = marker_skip_blanks(line, 0, len);
    size_t intro = iron_comment_intro_len(line + i, len - i);
    if (intro == 0) return false;
    i += intro;
    if (!iron_file_marker_parse(line + i, len - i, out)) return false;
    return iron_file_marker_admit(gate, phys, out);
}

/* Replay the lexer's marker semantics over source_text.
 *
 * Maps (filename, logical line) to the physical line number: the return
 * value, `logical` itself (the exact legacy behavior) when no line matches.
 * When `query_phys` is not 0, *query_is_marker says whether that physical
 * line is an honored marker line. */
static uint32_t replay_file_markers(const char *source_text,
                                    const char *filename, uint32_t logical,
                                    uint32_t query_phys, bool *query_is_marker) {
    if (query_is_marker) *query_is_marker = false;
    if (!source_text) return logical;

    Iron_FileMarkerGate gate = iron_file_marker_gate(g_diag_user_start_line);
    const char *p           = source_text;
    uint32_t    phys        = 1;
    uint32_t    cur_logical = 1;
    const char *cur_name     = NULL; /* NULL = entry region: match any file */
    size_t      cur_name_len = 0;
    uint32_t    found        = 0;

    while (*p != '\0') {
        const char *eol     = strchr(p, '\n');
        size_t      linelen = eol ? (size_t)(eol - p) : strlen(p);

        Iron_FileMarker m;
        if (honored_marker_on_line(&gate, p, linelen, phys, &m)) {
            if (query_is_marker && phys == query_phys) *query_is_marker = true;
            cur_name     = m.name;
            cur_name_len = m.name_len;
            /* @line marker: next physical line reports as line <n>.
             * Plain marker: physical resync (lexer sets line = phys_line at
             * the marker line; the newline bump makes the next line phys+1). */
            cur_logical = (m.line_reset > 0) ? m.line_reset : phys + 1;
        } else {
            int name_match =
                (filename == NULL) || (cur_name == NULL) ||
                (strlen(filename) == cur_name_len &&
                 strncmp(filename, cur_name, cur_name_len) == 0);
            if (found == 0 && logical != 0 && name_match && cur_logical == logical) {
                found = phys;
            }
            cur_logical++;
        }
        if ((found != 0 || logical == 0) &&
            (query_phys == 0 || phys >= query_phys)) break;

        if (!eol) break;
        p = eol + 1;
        phys++;
    }
    return found != 0 ? found : logical;
}

static uint32_t resolve_physical_line(const char *source_text,
                                      const char *filename,
                                      uint32_t logical) {
    if (logical == 0) return logical;
    return replay_file_markers(source_text, filename, logical, 0, NULL);
}

/* Is the physical line `lineno` of source_text an honored marker line?
 * Used to suppress marker lines from the context window (they always sit on
 * file boundaries, so the "line above"/"line below" of a first/last file
 * line would otherwise leak the synthetic marker). */
static int physical_line_is_marker(const char *source_text, uint32_t lineno) {
    bool is_marker = false;
    /* Logical line 0 never matches, so the walk runs up to `lineno`. */
    replay_file_markers(source_text, NULL, 0, lineno, &is_marker);
    return is_marker ? 1 : 0;
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

/* "      = help: text": a text of several lines keeps them aligned under the
 * first. */
static void print_multiline(const char *kind, const char *text) {
    fprintf(stderr, "      = %s: ", kind);
    size_t pad = 10 + strlen(kind);
    for (const char *p = text; *p; p++) {
        fputc(*p, stderr);
        if (*p == '\n' && p[1]) fprintf(stderr, "%*s", (int)pad, "");
    }
    fputc('\n', stderr);
}

/* One source line with a marker (`^` for the primary span, `-` for the
 * related one) under its column and the label after it. */
static void print_marked_line(const char *source_text, Iron_Span sp,
                              char mark, const char *label,
                              const char *color_start, const char *color_end) {
    char line_buf[512];
    uint32_t phys = resolve_physical_line(source_text, sp.filename, sp.line);
    if (!get_source_line(source_text, phys, line_buf, sizeof(line_buf))) return;
    fprintf(stderr, "%5u | %s\n", sp.line, line_buf);
    fprintf(stderr, "      | ");
    uint32_t col = sp.col > 0 ? sp.col : 1;
    for (uint32_t i = 1; i < col; i++) fputc(' ', stderr);
    if (mark == '^') fprintf(stderr, "%s^%s", color_start, color_end);
    else fputc(mark, stderr);
    if (label) fprintf(stderr, " %s", label);
    fputc('\n', stderr);
}

/* The source of a labeled diagnostic: the related line (a declaration,
 * usually above the use) and the primary line, each with its label. */
static void print_labeled_source(const Iron_Diagnostic *d, const char *source_text,
                                 const char *color_start, const char *color_end) {
    bool related = d->related_span.line > 0 &&
        (d->related_span.filename == d->span.filename ||
         (d->related_span.filename && d->span.filename &&
          strcmp(d->related_span.filename, d->span.filename) == 0));
    fprintf(stderr, "      |\n");
    if (related && d->related_span.line < d->span.line) {
        print_marked_line(source_text, d->related_span, '-', d->related_label,
                          color_start, color_end);
        if (d->span.line > d->related_span.line + 1) fprintf(stderr, "  ... |\n");
    }
    print_marked_line(source_text, d->span, '^', d->label, color_start, color_end);
    if (related && d->related_span.line >= d->span.line) {
        if (d->related_span.line > d->span.line + 1) fprintf(stderr, "  ... |\n");
        if (d->related_span.line == d->span.line) {
            fprintf(stderr, "      | ");
            uint32_t col = d->related_span.col > 0 ? d->related_span.col : 1;
            for (uint32_t i = 1; i < col; i++) fputc(' ', stderr);
            fprintf(stderr, "- %s\n", d->related_label ? d->related_label : "");
        } else {
            print_marked_line(source_text, d->related_span, '-', d->related_label,
                              color_start, color_end);
        }
    }
    fprintf(stderr, "      |\n");
}

const char *iron_diag_full_text(const Iron_Diagnostic *d, Iron_Arena *arena) {
    if (!d) return "";
    /* The related label names "here": an editor shows it at its own
     * location (relatedInformation), not in this text. */
    const char *parts[4] = { d->message, d->label, d->note, d->suggestion };
    const char *prefix[4] = { "", "", "note: ", "help: " };
    size_t n = 1;
    for (int i = 0; i < 4; i++) if (parts[i]) n += strlen(parts[i]) + strlen(prefix[i]) + 1;
    char *out = (char *)iron_arena_alloc(arena, n, 1);
    if (!out) return d->message ? d->message : "";
    out[0] = '\0';
    size_t len = 0;
    for (int i = 0; i < 4; i++) {
        if (!parts[i] || !parts[i][0]) continue;
        if (len > 0) out[len++] = '\n';
        size_t pl = strlen(prefix[i]), tl = strlen(parts[i]);
        memcpy(out + len, prefix[i], pl); len += pl;
        memcpy(out + len, parts[i], tl); len += tl;
        out[len] = '\0';
    }
    return out;
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
    if (source_text != NULL && (d->label || d->related_label || d->note)) {
        print_labeled_source(d, source_text, color_start, color_end);
        if (d->note) print_multiline("note", d->note);
    } else if (source_text != NULL) {
        char line_buf[512];
        uint32_t error_line = d->span.line;
        uint32_t phys_line  = resolve_physical_line(source_text,
                                                    d->span.filename,
                                                    error_line);

        /* Line above (if it exists and is not a file-boundary marker). */
        if (error_line > 1 && phys_line > 1) {
            if (get_source_line(source_text, phys_line - 1, line_buf, sizeof(line_buf)) &&
                !physical_line_is_marker(source_text, phys_line - 1)) {
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
            !physical_line_is_marker(source_text, phys_line + 1)) {
            fprintf(stderr, "%5u | %s\n", error_line + 1, line_buf);
        }
    }

    if (source_text == NULL && d->note) print_multiline("note", d->note);

    /* Suggestion. */
    if (d->suggestion != NULL) {
        print_multiline("help", d->suggestion);
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

void iron_diaglist_truncate(Iron_DiagList *list, int count) {
    if (!list || count < 0 || count >= list->count) return;
    arrsetlen(list->items, count);
    list->count = count;
    list->error_count = 0;
    list->warning_count = 0;
    for (int i = 0; i < count; i++) {
        if (list->items[i].level == IRON_DIAG_ERROR) list->error_count++;
        else if (list->items[i].level == IRON_DIAG_WARNING) list->warning_count++;
    }
}
