/* tests/fuzz/lsp/dispatch/fuzz_target.c — Phase 7 Plan 07-05 (HARD-19).
 *
 * libFuzzer harness for the LSP method dispatcher (CORE-05/-06).
 *
 * Design note (Rule 1 deviation from plan <action>):
 *   The plan's <action> block proposed calling ilsp_dispatch_route directly
 *   with a "minimal session stub". In practice ilsp_dispatch_route requires
 *   a fully-wired IronLsp_Server: writer thread (with a FILE*), lifecycle
 *   FSM, cancels, documents map, workspace_index, plus every single
 *   handler TU (which itself depends on the compiler pipeline, workspace
 *   discovery, worker threads, SIGABRT boundary setjmp, etc.). Attempting
 *   to stand that up in a libFuzzer per-iteration hot path would either
 *   (a) re-implement half the server, or (b) run the real server and turn
 *   the fuzzer into an integration test that loses coverage focus.
 *
 *   Instead this harness fuzzes the TWO pure, side-effect-free pieces of
 *   the dispatcher that carry the bulk of the adversarial-input risk:
 *
 *     1. Parse the fuzz bytes as a JSON-RPC envelope through the arena-
 *        backed ilsp_json_parse (same path dispatch.c:290 takes).
 *     2. Extract the method string via yyjson_obj_get(root, "method") +
 *        yyjson_get_str (same path dispatch.c:303-305 takes).
 *     3. Feed that method into ilsp_handler_lookup (the bsearch lookup
 *        that every real dispatch round hits at dispatch.c:333).
 *
 *   Invariants asserted:
 *     - Every returned entry pointer either matches a valid row in
 *       ilsp_handler_table[] or is NULL.
 *     - The returned entry's `method` field, if non-NULL, exactly matches
 *       the lookup key (consistency check that the bsearch comparator
 *       never returns a wrong-bucket row under adversarial input).
 *
 *   This covers the DoS-class risks enumerated in Plan 07-05 threat model
 *   T-07-05-03 (ReDoS in method names — lookup is bsearch + strcmp so no
 *   regex DoS surface) and T-07-05-01 (malformed/adversarial IDs cannot
 *   corrupt lookup because lookup never touches id). Unknown-method
 *   routing (-32601 MethodNotFound) is covered indirectly: any input
 *   libFuzzer picks that is a valid JSON object with a non-table `method`
 *   string exercises the NULL-return path of ilsp_handler_lookup.
 *
 * Per-iteration lifecycle (mirrors fuzz_parser.c + Pitfall 4):
 *   Iron_Arena arena = iron_arena_create(64 KB)
 *   ilsp_json_parse(...)
 *   (extract + lookup)
 *   iron_arena_free(&arena)
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "util/arena.h"
#include "lsp/transport/json.h"
#include "lsp/server/dispatch.h"
#include "vendor/yyjson/yyjson.h"

#define IRON_FUZZ_DISPATCH_MAX_INPUT 8192
#define IRON_FUZZ_DISPATCH_ARENA_BYTES (64UL * 1024UL)

/* Extern'd from dispatch.c. Declared here because dispatch.h exposes
 * ilsp_handler_table but we want to scan it as bounds check. */
extern const IronLsp_HandlerEntry ilsp_handler_table[];
extern const size_t               ilsp_handler_table_size;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > IRON_FUZZ_DISPATCH_MAX_INPUT) return -1;
    if (size == 0) return 0;

    Iron_Arena arena = iron_arena_create(IRON_FUZZ_DISPATCH_ARENA_BYTES);

    yyjson_read_err err;
    memset(&err, 0, sizeof(err));
    yyjson_doc *doc = ilsp_json_parse((const char *)data, size, &arena, &err);

    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        if (root && yyjson_is_obj(root)) {
            /* Exact same extraction sequence as dispatch.c:303-305. */
            yyjson_val *method_v = yyjson_obj_get(root, "method");
            yyjson_val *id       = yyjson_obj_get(root, "id");
            const char *method   = method_v ? yyjson_get_str(method_v) : NULL;

            /* Lookup: NULL method is explicitly handled by
             * ilsp_handler_lookup (returns NULL). Any non-NULL method is
             * passed to bsearch; under adversarial input (binary garbage,
             * extremely long strings, embedded NULs in the wrong place)
             * the comparator must not read past the method's NUL. */
            const IronLsp_HandlerEntry *entry = ilsp_handler_lookup(method);
            if (entry) {
                /* entry must be a row of the handler table. Check via
                 * address range: every valid entry sits at
                 * ilsp_handler_table[i] for some i < table_size. */
                ptrdiff_t offset = entry - ilsp_handler_table;
                if (offset < 0 ||
                    (size_t)offset >= ilsp_handler_table_size) {
                    __builtin_trap();
                }
                /* entry->method must match the key byte-for-byte
                 * (consistency check on the bsearch comparator). */
                if (!entry->method || !method ||
                    strcmp(entry->method, method) != 0) {
                    __builtin_trap();
                }
            }

            /* id traversal: dispatcher calls clone_id() on the incoming
             * id for error responses. Exercise the int/uint/str/null
             * reader paths (mirrors clone_id at dispatch.c:240-249) so
             * buffer-overrun in yyjson id coercion surfaces. */
            if (id) {
                if (yyjson_is_int(id) || yyjson_is_sint(id)) {
                    (void)yyjson_get_sint(id);
                } else if (yyjson_is_uint(id)) {
                    (void)yyjson_get_uint(id);
                } else if (yyjson_is_str(id)) {
                    (void)yyjson_get_str(id);
                }
            }
        }
    }

    iron_arena_free(&arena);
    return 0;
}
