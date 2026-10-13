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

/* Length of the valid UTF-8 sequence at s[i] (1..4), or 0 when the byte
 * there does not start one. */
static size_t utf8_seq(const unsigned char *s, size_t i, size_t n) {
    unsigned char c = s[i];
    size_t need;
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) need = 1;
    else if (c >= 0xE0 && c <= 0xEF) need = 2;
    else if (c >= 0xF0 && c <= 0xF4) need = 3;
    else return 0;
    if (n - i <= need) return 0;
    for (size_t k = 1; k <= need; k++)
        if ((s[i + k] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && s[i + 1] < 0xA0) return 0;   /* overlong */
    if (c == 0xED && s[i + 1] > 0x9F) return 0;   /* surrogate */
    if (c == 0xF0 && s[i + 1] < 0x90) return 0;   /* overlong */
    if (c == 0xF4 && s[i + 1] > 0x8F) return 0;   /* past U+10FFFF */
    return need + 1;
}

/* Replace every invalid UTF-8 byte in the string values under v with
 * U+FFFD (the copies live in `arena`, as the document does). */
static void sanitize_strings(yyjson_mut_val *v, Iron_Arena *arena) {
    if (!v) return;
    if (yyjson_mut_is_str(v)) {
        const unsigned char *s = (const unsigned char *)yyjson_mut_get_str(v);
        size_t n = yyjson_mut_get_len(v), bad = 0;
        if (!s) return;
        for (size_t i = 0; i < n;) {
            size_t k = utf8_seq(s, i, n);
            if (k == 0) { bad++; i++; } else i += k;
        }
        if (bad == 0) return;
        char *out = (char *)iron_arena_alloc(arena, n + bad * 2 + 1, 1);
        if (!out) return;
        size_t o = 0;
        for (size_t i = 0; i < n;) {
            size_t k = utf8_seq(s, i, n);
            if (k == 0) {
                out[o++] = (char)0xEF; out[o++] = (char)0xBF; out[o++] = (char)0xBD;
                i++;
            } else {
                memcpy(out + o, s + i, k);
                o += k;
                i += k;
            }
        }
        out[o] = '\0';
        yyjson_mut_set_strn(v, out, o);
        return;
    }
    size_t idx, max;
    if (yyjson_mut_is_arr(v)) {
        yyjson_mut_val *e;
        yyjson_mut_arr_foreach(v, idx, max, e) sanitize_strings(e, arena);
    } else if (yyjson_mut_is_obj(v)) {
        yyjson_mut_val *key, *e;
        yyjson_mut_obj_foreach(v, idx, max, key, e) sanitize_strings(e, arena);
        (void)key;
    }
}

char *ilsp_json_write_mut(yyjson_mut_doc *doc, Iron_Arena *arena,
                          size_t *out_len) {
    if (!arena) return NULL;
    yyjson_alc alc = ilsp_json_alc(arena);
    size_t n = 0;
    yyjson_write_err err;
    char *s = yyjson_mut_write_opts(doc, 0, &alc, &n, &err);
    if (!s && err.code == YYJSON_WRITE_ERROR_INVALID_STRING) {
        /* Text taken from the document (a name, a doc comment) can hold
         * invalid UTF-8, or a slice can cut a character; the write then
         * failed and the response or notification was silently dropped,
         * leaving the client waiting. Replace the bad bytes and retry. */
        sanitize_strings(yyjson_mut_doc_get_root(doc), arena);
        s = yyjson_mut_write_opts(doc, 0, &alc, &n, NULL);
    }
    if (out_len) *out_len = n;
    return s;
}
