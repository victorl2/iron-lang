/* Phase 2 Plan 02 Task 02 -- yyjson-to-Iron_Arena wrapper.
 *
 * malloc  -> iron_arena_alloc (aligned 8 -- yyjson_val/yyjson_mut_val are
 *            pointer-sized, so 8-byte alignment is sufficient on all
 *            platforms we target)
 * realloc -> arena has no true realloc. If the request shrinks or keeps
 *            the same size, we return the original pointer (the arena is
 *            a bump allocator: the tail bytes are still valid). If it
 *            grows, we allocate a fresh block and memcpy the old bytes.
 * free    -> no-op. Arena reclaims everything at iron_arena_free time.
 *
 * The arena lifetime is the request lifetime (HARD-06): a per-request
 * arena means a parse failure drops the doc AND frees everything via the
 * same mechanism -- no partial-cleanup paths to audit. */
#include "lsp/transport/json.h"

#include <string.h>

static void *ilsp_alc_malloc(void *ctx, size_t size) {
    return iron_arena_alloc((Iron_Arena *)ctx, size, 8);
}

static void *ilsp_alc_realloc(void *ctx, void *ptr, size_t old_size,
                              size_t new_size) {
    /* Bump allocator: if caller wants to shrink or keep size, reuse the
     * original allocation. yyjson's realloc paths commonly shrink a buffer
     * after writing; we must never fail that. */
    if (new_size <= old_size) return ptr;
    Iron_Arena *a = (Iron_Arena *)ctx;
    void *np = iron_arena_alloc(a, new_size, 8);
    if (!np) return NULL;
    if (ptr && old_size > 0) memcpy(np, ptr, old_size);
    return np;
}

static void ilsp_alc_free(void *ctx, void *ptr) {
    /* No-op: arena reclaims on iron_arena_free. */
    (void)ctx;
    (void)ptr;
}

yyjson_alc ilsp_json_alc(Iron_Arena *arena) {
    yyjson_alc a;
    a.malloc  = ilsp_alc_malloc;
    a.realloc = ilsp_alc_realloc;
    a.free    = ilsp_alc_free;
    a.ctx     = arena;
    return a;
}

yyjson_doc *ilsp_json_parse(const char *body, size_t len,
                            Iron_Arena *arena, yyjson_read_err *err_out) {
    if (!arena) return NULL;
    yyjson_alc alc = ilsp_json_alc(arena);
    yyjson_read_err local;
    memset(&local, 0, sizeof(local));
    yyjson_read_err *err = err_out ? err_out : &local;
    /* yyjson_read_opts takes (char *) but documents that with
     * YYJSON_READ_INSITU unset, the input buffer is read-only in practice.
     * We cast-away-const with explicit intent: body bytes must survive
     * until the doc is no longer used (the caller's framer buffer). */
    return yyjson_read_opts((char *)body, len, 0, &alc, err);
}

char *ilsp_json_write_mut(yyjson_mut_doc *doc, Iron_Arena *arena,
                          size_t *out_len) {
    if (!arena) return NULL;
    yyjson_alc alc = ilsp_json_alc(arena);
    size_t n = 0;
    char *s = yyjson_mut_write_opts(doc, 0, &alc, &n, NULL);
    if (out_len) *out_len = n;
    return s;
}
