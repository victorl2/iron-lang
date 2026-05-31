/* Phase 34 Plan 34-04 — shared fixture runner for the 5 new memory-model
 * quickfixes (LSP-06..10). Each test_quickfix_lsp_NN_*.c file includes
 * this header, declares its (fixture, code, span coords, handler) tuple
 * via QF34_RUN_FIXTURE, and main() dispatches to the handler under test
 * with a synthesized Iron_Diagnostic.
 *
 * Why synthesized diagnostics?
 *   The handlers are CONSUMERS — they read a diag and produce TextEdits.
 *   Driving them through the full compile pipeline would couple the
 *   handler-correctness test to compiler-side emit-site readiness, which
 *   for LSP-06/07/08/10 lives in a future plan. The CORE-22 facade is
 *   still exercised (each handler calls ilsp_facade_compile_for_nav for
 *   the fresh-spans round-trip).
 *
 * Fixture format (.expected_edit):
 *   Single-action quickfix:
 *     title: <title>
 *     range: <line>:<col>-<endline>:<endcol>     (LSP 0-indexed)
 *     new_text: <text>
 *   Multi-action (LSP-10) extends with a `variant: N` prefix per block,
 *   separated by a blank line; the runner_compare_multi helper handles
 *   that shape.
 *
 * Conventions:
 *   - Trailing whitespace in `new_text:` is significant when present.
 *     The fixture parser captures all bytes between "new_text: " and
 *     the next '\n' verbatim. The literal "\n" escape inside that
 *     payload is decoded to a real newline.
 *   - Run from the build directory; fixtures resolved via
 *     IRON_SOURCE_TREE_ROOT macro (set by the CMake target). */

#ifndef PHASE34_QF_FIXTURE_RUNNER_H
#define PHASE34_QF_FIXTURE_RUNNER_H

#include "unity.h"

#include "lsp/facade/edit/codeaction/registry.h"
#include "lsp/store/document.h"
#include "diagnostics/diagnostics.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef IRON_SOURCE_TREE_ROOT
#define IRON_SOURCE_TREE_ROOT "."
#endif

static char *qf34_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Decode the literal "\n" escape sequence into a real newline byte.
 * Writes to a caller-provided buffer; returns the new length. The
 * fixture parser stores newlines as the 2-byte escape "\\n" so the
 * .expected_edit file stays single-line per field. */
static size_t qf34_decode_escapes(const char *src, size_t len,
                                     char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        if (i + 1 < len && src[i] == '\\' && src[i + 1] == 'n') {
            dst[o++] = '\n';
            i++;
        } else {
            dst[o++] = src[i];
        }
    }
    dst[o] = '\0';
    return o;
}

/* Parse a single-action .expected_edit. Writes the title, range, and
 * new_text into caller buffers (capacity 512 each). Returns true on
 * success. */
static bool qf34_parse_single(const char *content,
                                 char *title, size_t title_cap,
                                 uint32_t *sl, uint32_t *sc,
                                 uint32_t *el, uint32_t *ec,
                                 char *new_text, size_t new_text_cap) {
    const char *p = content;
    const char *line_end;
    bool got_title = false, got_range = false, got_text = false;
    while ((line_end = strchr(p, '\n')) != NULL) {
        size_t llen = (size_t)(line_end - p);
        if (strncmp(p, "title: ", 7) == 0) {
            size_t n = llen - 7;
            if (n >= title_cap) n = title_cap - 1;
            memcpy(title, p + 7, n);
            title[n] = '\0';
            got_title = true;
        } else if (strncmp(p, "range: ", 7) == 0) {
            if (sscanf(p + 7, "%u:%u-%u:%u", sl, sc, el, ec) == 4) {
                got_range = true;
            }
        } else if (strncmp(p, "new_text: ", 10) == 0) {
            size_t n = llen - 10;
            char raw[1024];
            if (n >= sizeof(raw)) n = sizeof(raw) - 1;
            memcpy(raw, p + 10, n);
            raw[n] = '\0';
            qf34_decode_escapes(raw, n, new_text, new_text_cap);
            got_text = true;
        }
        p = line_end + 1;
    }
    return got_title && got_range && got_text;
}

/* Build a synthesized Iron_Diagnostic at the given 1-indexed coords. */
static Iron_Diagnostic qf34_mk_diag(int code,
                                       uint32_t line_1, uint32_t col_1,
                                       uint32_t end_line_1, uint32_t end_col_1,
                                       const char *filename,
                                       const char *msg) {
    Iron_Diagnostic d;
    d.level         = IRON_DIAG_ERROR;
    d.code          = code;
    d.span.filename = filename;
    d.span.line     = line_1;
    d.span.col      = col_1;
    d.span.end_line = end_line_1;
    d.span.end_col  = end_col_1;
    d.message       = msg;
    d.suggestion    = NULL;
    return d;
}

/* Resolve a fixture path relative to the source tree root. */
static const char *qf34_fixture_path(char *buf, size_t cap, const char *name) {
    snprintf(buf, cap, "%s/tests/lsp/fixtures/%s",
             IRON_SOURCE_TREE_ROOT, name);
    return buf;
}

#endif /* PHASE34_QF_FIXTURE_RUNNER_H */
