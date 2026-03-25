#include "diagnostics/diagnostics.h"
#include "stb_ds.h"

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
    default: break;
    }
}

/* ── Printing ────────────────────────────────────────────────────────────── */

/* Returns 1 if stderr supports ANSI color (isatty). */
static int use_color(void) {
    return isatty(STDERR_FILENO);
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

    /* Header: "error[E0001]: message" */
    fprintf(stderr, "%s%s[E%04d]%s: %s\n",
            color_start, level_str, d->code, color_end, d->message);

    /* Location: "  --> file.iron:5:10" */
    fprintf(stderr, "  --> %s:%u:%u\n",
            d->span.filename ? d->span.filename : "<unknown>",
            d->span.line,
            d->span.col);

    /* Source context: up to 3 lines (line above, error line, line below). */
    if (source_text != NULL) {
        char line_buf[512];
        uint32_t error_line = d->span.line;

        /* Line above (if it exists). */
        if (error_line > 1) {
            if (get_source_line(source_text, error_line - 1, line_buf, sizeof(line_buf))) {
                fprintf(stderr, "%5u | %s\n", error_line - 1, line_buf);
            }
        }

        /* Error line. */
        if (get_source_line(source_text, error_line, line_buf, sizeof(line_buf))) {
            fprintf(stderr, "%5u | %s\n", error_line, line_buf);

            /* Caret pointing to the column. */
            fprintf(stderr, "      | ");
            uint32_t col = d->span.col > 0 ? d->span.col : 1;
            for (uint32_t i = 1; i < col; i++) {
                fprintf(stderr, " ");
            }
            fprintf(stderr, "%s^%s\n", color_start, color_end);
        }

        /* Line below. */
        if (get_source_line(source_text, error_line + 1, line_buf, sizeof(line_buf))) {
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
