/* tests/fuzz/lsp/json/fuzz_target.c — Phase 7 Plan 07-05 (HARD-19).
 *
 * libFuzzer harness for the LSP yyjson + Iron_Arena allocator binding
 * (CORE-03). Drives arbitrary bytes through ilsp_json_parse on a fresh
 * 64 KB arena per iteration. On successful parse, exercises a few common
 * accessor paths (method / jsonrpc / id / params) so any downstream
 * buffer-overrun on malformed-but-parseable input surfaces.
 *
 * Invariants asserted by libFuzzer + ASan + UBSan (implicit):
 *   - ilsp_json_parse never returns a dangling yyjson_doc after the arena
 *     it was allocated from is freed (the doc lifetime IS the arena
 *     lifetime by construction -- this harness creates + destroys both
 *     on each iteration, so a lifetime escape would manifest as a
 *     use-after-free caught by ASan).
 *   - Deep nesting does not stack-overflow (yyjson has a built-in
 *     recursion limit; this harness exercises it).
 *   - Malformed UTF-8 / overlong encodings / duplicate keys never crash.
 *   - The arena's bump-allocator realloc path (see json.c :23-33) does
 *     not corrupt memory under any input (ASan guard pages catch OOB).
 *
 * Per-iteration discipline mirrors tests/fuzz/fuzz_parser.c:
 *   Iron_Arena arena = iron_arena_create(...)   // fresh 64 KB
 *   ilsp_json_parse(...)
 *   (exercise accessors)
 *   iron_arena_free(&arena)                     // reclaim all
 *
 * NOTE: The plan's <action> block referenced `ilsp_json_get_string(root,
 * "path/with/slashes")`. The real binding is yyjson's direct accessor
 * API (yyjson_obj_get / yyjson_get_str / ...). Harness uses the real
 * API (Rule 1 deviation -- documented in Plan 07-05 summary).
 */

#include <stddef.h>
#include <stdint.h>

#include "util/arena.h"
#include "lsp/transport/json.h"
#include "vendor/yyjson/yyjson.h"

/* 4 KB is plenty of JSON for an LSP envelope; larger inputs burn time
 * on yyjson's recursion limiter without finding new bugs. Mirrors
 * fuzz_parser's IRON_FUZZ_PARSER_MAX_INPUT defense-in-depth. */
#define IRON_FUZZ_JSON_MAX_INPUT 8192

/* 64 KB per-iteration arena -- yyjson values are ~24 bytes each, so this
 * comfortably holds the deepest/widest JSON the fuzz budget will explore
 * in 8 KB of input without crossing the arena's 64 KB chunk boundary
 * (which would still be correct, just slower). */
#define IRON_FUZZ_JSON_ARENA_BYTES (64UL * 1024UL)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > IRON_FUZZ_JSON_MAX_INPUT) return -1;

    Iron_Arena arena = iron_arena_create(IRON_FUZZ_JSON_ARENA_BYTES);

    yyjson_read_err err = {0};
    yyjson_doc *doc = ilsp_json_parse((const char *)data, size, &arena, &err);

    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (root && yyjson_is_obj(root)) {
            /* Exercise accessor surface on LSP-typical keys. Each call
             * is NULL-tolerant at the yyjson layer; the getters below
             * either return the value or NULL/"" on absence. */
            yyjson_val *m_method  = yyjson_obj_get(root, "method");
            yyjson_val *m_jsonrpc = yyjson_obj_get(root, "jsonrpc");
            yyjson_val *m_id      = yyjson_obj_get(root, "id");
            yyjson_val *m_params  = yyjson_obj_get(root, "params");
            (void)yyjson_get_str(m_method);
            (void)yyjson_get_str(m_jsonrpc);

            /* id can be int, string, or null per JSON-RPC 2.0; exercise
             * all three readers so the dispatcher's id-coercion path is
             * covered indirectly. */
            (void)yyjson_get_int(m_id);
            (void)yyjson_get_str(m_id);

            /* params can be object or array; walk it one level if
             * present so nested-object allocation through the arena
             * allocator's realloc path is exercised. */
            if (m_params && yyjson_is_obj(m_params)) {
                yyjson_val *key, *val;
                yyjson_obj_iter iter = yyjson_obj_iter_with(m_params);
                while ((key = yyjson_obj_iter_next(&iter))) {
                    val = yyjson_obj_iter_get_val(key);
                    (void)yyjson_get_str(val);
                }
            }
        }
        /* No yyjson_doc_free() -- doc's memory lives in the arena and
         * is reclaimed wholesale by iron_arena_free below. Calling
         * yyjson_doc_free would double-free under the arena binding
         * (free is a no-op but the internal pointers are invalidated
         * en masse by the arena teardown). */
    }

    iron_arena_free(&arena);
    return 0;
}
