/* Phase 2 Plan 04 Task 01 (CORE-10) -- line_starts[] index.
 *
 * Walks the buffer once per edit, pushing a new offset on every '\n'.
 * Binary search for (byte -> line). Inverse (line -> byte) is a direct
 * index. Zero allocation on the steady state once the stb_ds array
 * reaches its peak size.
 *
 * Analogs:
 *   - src/diagnostics/diagnostics.c:83-106 (line-extraction walk pattern)
 *   - src/lexer/lexer.c:205-233 (byte-walking). */
#include "lsp/store/line_index.h"
#include "vendor/stb_ds.h"

#include <string.h>

void ilsp_line_index_init(IronLsp_LineIndex *idx) {
    if (!idx) return;
    idx->starts = NULL;
}

void ilsp_line_index_destroy(IronLsp_LineIndex *idx) {
    if (!idx) return;
    if (idx->starts) {
        arrfree(idx->starts);
        idx->starts = NULL;
    }
}

void ilsp_line_index_rebuild(IronLsp_LineIndex *idx,
                              const char *text, size_t len) {
    if (!idx) return;
    /* Discard previous state. stb_ds arrfree + rebuild is cheaper than a
     * per-element reset because the peak capacity is typically stable. */
    if (idx->starts) {
        arrsetlen(idx->starts, 0);
    }

    /* Line 0 always starts at offset 0 (invariant). */
    arrput(idx->starts, (uint32_t)0u);

    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            /* Next line starts at byte (i + 1). If that's past the end
             * (trailing '\n'), push it anyway -- the trailing empty line
             * is semantically addressable and clients may place the
             * cursor there. */
            arrput(idx->starts, (uint32_t)(i + 1));
        }
    }
}

uint32_t ilsp_line_of_byte(const IronLsp_LineIndex *idx, size_t byte_offset) {
    if (!idx || !idx->starts) return 0;
    ptrdiff_t n = arrlen(idx->starts);
    if (n <= 0) return 0;

    /* Clamp past-end to last line. */
    uint32_t last = idx->starts[n - 1];
    if (byte_offset >= last) return (uint32_t)(n - 1);

    /* Binary search for largest i with starts[i] <= byte_offset. */
    ptrdiff_t lo = 0;
    ptrdiff_t hi = n - 1;
    while (lo < hi) {
        ptrdiff_t mid = lo + (hi - lo + 1) / 2;   /* Upper-mid to avoid infinite loops. */
        if (idx->starts[mid] <= byte_offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return (uint32_t)lo;
}

size_t ilsp_byte_of_line(const IronLsp_LineIndex *idx, uint32_t line) {
    if (!idx || !idx->starts) return 0;
    ptrdiff_t n = arrlen(idx->starts);
    if (n <= 0) return 0;
    if ((ptrdiff_t)line >= n) return (size_t)idx->starts[n - 1];
    return (size_t)idx->starts[line];
}
