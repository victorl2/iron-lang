#ifndef IRON_LSP_TRANSPORT_JSON_H
#define IRON_LSP_TRANSPORT_JSON_H

/* Phase 2 Plan 02 Task 02 -- yyjson-to-Iron_Arena wrapper.
 *
 * All JSON parse/write traffic on the LSP hot path goes through an
 * Iron_Arena-backed allocator. yyjson's malloc/realloc/free callbacks are
 * routed to iron_arena_alloc; free is a no-op (the arena frees everything
 * at once when the request completes). This is the T-02-03 DoS mitigation
 * -- parse failures drop to NULL and the arena frees with the request,
 * so a malicious deeply-nested payload cannot leak memory. */

#include <stddef.h>

#include "util/arena.h"
#include "vendor/yyjson/yyjson.h"

/* Build a yyjson_alc whose ctx is a pointer to an Iron_Arena. All three
 * callbacks are bound; free is a no-op so yyjson's internal shrink paths
 * do not try to reclaim arena bytes. */
yyjson_alc ilsp_json_alc(Iron_Arena *arena);

/* Parse JSON body bytes through the arena allocator. Returns a non-NULL
 * doc on success; NULL on parse error (err_out, if non-NULL, is populated
 * with yyjson's diagnostic). The returned doc's memory lives inside the
 * arena -- iron_arena_free(arena) releases it. */
yyjson_doc *ilsp_json_parse(const char *body, size_t len,
                            Iron_Arena *arena, yyjson_read_err *err_out);

/* Serialize a mutable yyjson doc through the arena allocator. Returns an
 * arena-allocated NUL-terminated string; *out_len (if non-NULL) is set to
 * the written byte count (excluding the NUL). Returns NULL on failure. */
char *ilsp_json_write_mut(yyjson_mut_doc *doc, Iron_Arena *arena,
                          size_t *out_len);

#endif /* IRON_LSP_TRANSPORT_JSON_H */
