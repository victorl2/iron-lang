#ifndef IRON_JSON_H
#define IRON_JSON_H

/* iron_json.h -- native half of the `json` module (src/stdlib/json.iron).
 *
 * The parser validates the text and lowers it to a flat tape that the Iron
 * side turns into a Json tree; the writer appends encoded JSON to a growable
 * byte buffer behind an opaque handle. Neither touches compiler-generated
 * types: the tape only uses the runtime-owned list types ([Int], [Float],
 * [String]) and the writer is addressed by an Int handle. */

#include <stdint.h>
#include <stdbool.h>
#include "runtime/iron_runtime.h"

/* Tape opcodes. An OP_INT word is followed by the value word; OP_FLOAT,
 * OP_STR and OP_KEY take the next element of `floats` / `strs`. */
enum {
    IRON_JSON_OP_NULL  = 0,
    IRON_JSON_OP_FALSE = 1,
    IRON_JSON_OP_TRUE  = 2,
    IRON_JSON_OP_INT   = 3,
    IRON_JSON_OP_FLOAT = 4,
    IRON_JSON_OP_STR   = 5,
    IRON_JSON_OP_ARR   = 6,
    IRON_JSON_OP_OBJ   = 7,
    IRON_JSON_OP_END   = 8,
    IRON_JSON_OP_KEY   = 9
};

/* Must match `object JsonTape` in src/stdlib/json.iron field for field. */
typedef struct Iron_JsonTape {
    Iron_List_int64_t ops;
    Iron_List_double floats;
    Iron_List_Iron_String strs;
    int64_t error;
    Iron_String error_message;
    int64_t line;
    int64_t column;
    int64_t offset;
} Iron_JsonTape;

/* The deepest nesting any parse accepts, whatever limit is asked for.
 * Dropping a Json tree recurses once per level (about 80 bytes of stack
 * each); 4096 levels stay far inside the smallest stack an Iron program
 * runs on (the 1 MiB main thread on Windows). */
#define IRON_JSON_MAX_DEPTH 4096

/* Parse `text` (strict RFC 8259). On failure `error` is an IRON_ERR_JSON_*
 * code, the lists are empty and line/column (1-based, column in characters)
 * and offset (0-based byte) locate the problem. */
Iron_JsonTape Iron_jsonnative_scan(Iron_String text, int64_t max_depth, int64_t max_bytes);

/* The message for an IRON_ERR_JSON_* code. */
Iron_String Iron_jsonnative_error_text(int64_t code);

/* `s` encoded as a JSON string literal, quotes included. */
Iron_String Iron_jsonnative_quote(Iron_String s);

/* Shortest round-trip text of a float as a JSON number ("1.0", "0.1",
 * "1e+300"); non-finite values give "null". */
Iron_String Iron_jsonnative_format_float(double f);

/* ── Writer ───────────────────────────────────────────────────────────── */
int64_t     Iron_jsonnative_w_new(int64_t indent);
void        Iron_jsonnative_w_free(int64_t h);
void        Iron_jsonnative_w_begin_object(int64_t h);
void        Iron_jsonnative_w_end_object(int64_t h);
void        Iron_jsonnative_w_begin_array(int64_t h);
void        Iron_jsonnative_w_end_array(int64_t h);
void        Iron_jsonnative_w_key(int64_t h, Iron_String key);
void        Iron_jsonnative_w_string(int64_t h, Iron_String s);
void        Iron_jsonnative_w_int(int64_t h, int64_t v);
void        Iron_jsonnative_w_float(int64_t h, double v);
void        Iron_jsonnative_w_bool(int64_t h, bool v);
void        Iron_jsonnative_w_null(int64_t h);
void        Iron_jsonnative_w_raw(int64_t h, Iron_String json);
void        Iron_jsonnative_w_field_string(int64_t h, Iron_String key, Iron_String s);
void        Iron_jsonnative_w_field_int(int64_t h, Iron_String key, int64_t v);
void        Iron_jsonnative_w_field_float(int64_t h, Iron_String key, double v);
void        Iron_jsonnative_w_field_bool(int64_t h, Iron_String key, bool v);
void        Iron_jsonnative_w_field_null(int64_t h, Iron_String key);
int64_t     Iron_jsonnative_w_len(int64_t h);
int64_t     Iron_jsonnative_w_depth(int64_t h);
Iron_String Iron_jsonnative_w_finish(int64_t h);
void        Iron_jsonnative_w_reset(int64_t h);

#endif /* IRON_JSON_H */
