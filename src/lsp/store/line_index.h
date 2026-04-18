#ifndef IRON_LSP_STORE_LINE_INDEX_H
#define IRON_LSP_STORE_LINE_INDEX_H

/* Phase 2 Plan 04 Task 01 (CORE-10) -- line_starts[] index.
 *
 * Flat stb_ds dynamic array of byte offsets: starts[i] is the byte
 * position of the first byte of line i. starts[0] is always 0 (even for
 * empty buffers -- an empty buffer has one zero-length line).
 *
 * Rebuilt from scratch on every document edit (CORE-09). Incremental
 * maintenance is a Phase 7 optimization; Phase 2 correctness prefers a
 * linear walk that is trivially verifiable.
 *
 * Binary search: ilsp_line_of_byte uses a bracketed loop over starts[],
 * returning the largest i with starts[i] <= byte_offset. Clamps both
 * sides. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IronLsp_LineIndex {
    uint32_t *starts;   /* stb_ds dynamic array; starts[0] == 0 always. */
} IronLsp_LineIndex;

/* Lifecycle. init zero-initializes; destroy frees the stb_ds array. */
void ilsp_line_index_init   (IronLsp_LineIndex *idx);
void ilsp_line_index_destroy(IronLsp_LineIndex *idx);

/* Rebuild starts[] from scratch. Walks text byte-by-byte; on each '\n'
 * appends (i + 1) to starts. CRLF handling: the '\n' drives the push;
 * the preceding '\r' remains part of the prior line (matches LSP 3.17
 * §textDocument synchronization). */
void ilsp_line_index_rebuild(IronLsp_LineIndex *idx,
                              const char *text, size_t len);

/* 0-indexed line containing the given byte offset.
 *   - If byte_offset >= last line's start, returns last line index.
 *   - Binary search O(log n). */
uint32_t ilsp_line_of_byte(const IronLsp_LineIndex *idx, size_t byte_offset);

/* Byte offset of the start of the given 0-indexed line. Clamps to the
 * end of starts[] (returns last line's start) if line is out of range. */
size_t   ilsp_byte_of_line(const IronLsp_LineIndex *idx, uint32_t line);

#ifdef __cplusplus
}
#endif

#endif /* IRON_LSP_STORE_LINE_INDEX_H */
