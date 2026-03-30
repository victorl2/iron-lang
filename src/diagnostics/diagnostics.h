#ifndef IRON_DIAGNOSTICS_H
#define IRON_DIAGNOSTICS_H

#include "util/arena.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Source span ─────────────────────────────────────────────────────────── */

/* Identifies a range of source text from start (line:col) to end (end_line:end_col).
 * filename is an interned string (arena-allocated, compare by pointer is valid within
 * a single compilation unit).
 * Lines and columns are 1-indexed. Columns are byte-based.
 */
typedef struct {
    const char *filename;
    uint32_t    line;
    uint32_t    col;
    uint32_t    end_line;
    uint32_t    end_col;
} Iron_Span;

/* Construct a span from explicit components. */
Iron_Span iron_span_make(const char *filename,
                          uint32_t line, uint32_t col,
                          uint32_t end_line, uint32_t end_col);

/* Merge two spans: result spans from the start of `start` to the end of `end`.
 * filename is taken from `start`.
 */
Iron_Span iron_span_merge(Iron_Span start, Iron_Span end);

/* ── Diagnostic level ────────────────────────────────────────────────────── */

typedef enum {
    IRON_DIAG_ERROR,
    IRON_DIAG_WARNING,
    IRON_DIAG_NOTE
} Iron_DiagLevel;

/* ── Single diagnostic ───────────────────────────────────────────────────── */

typedef struct {
    Iron_DiagLevel  level;
    int             code;         /* E-code number, e.g. 1 for E0001 */
    Iron_Span       span;
    const char     *message;     /* arena-allocated */
    const char     *suggestion;  /* arena-allocated, NULL if none */
} Iron_Diagnostic;

/* ── Diagnostic list ─────────────────────────────────────────────────────── */

typedef struct {
    Iron_Diagnostic *items;        /* stb_ds dynamic array */
    int              count;
    int              error_count;
    int              warning_count;
} Iron_DiagList;

Iron_DiagList iron_diaglist_create(void);

void iron_diag_emit(Iron_DiagList *list,
                    Iron_Arena    *arena,
                    Iron_DiagLevel level,
                    int            code,
                    Iron_Span      span,
                    const char    *message,
                    const char    *suggestion);

/* Print a single diagnostic with optional source context.
 * source_text may be NULL; if non-NULL, a 3-line context window is shown.
 */
void iron_diag_print(const Iron_Diagnostic *d, const char *source_text);

/* Print all diagnostics in the list. */
void iron_diag_print_all(const Iron_DiagList *list, const char *source_text);

void iron_diaglist_free(Iron_DiagList *list);

/* ── Error codes ─────────────────────────────────────────────────────────── */

/* Lexer errors */
#define IRON_ERR_UNTERMINATED_STRING   1
#define IRON_ERR_INVALID_CHAR          2
#define IRON_ERR_INVALID_NUMBER        3

/* Parser errors */
#define IRON_ERR_UNEXPECTED_TOKEN    101
#define IRON_ERR_EXPECTED_EXPR       102
#define IRON_ERR_EXPECTED_RBRACE     103
#define IRON_ERR_EXPECTED_RPAREN     104
#define IRON_ERR_EXPECTED_COLON      105
#define IRON_ERR_EXPECTED_ARROW      106

/* Semantic errors */
#define IRON_ERR_UNDEFINED_VAR        200
#define IRON_ERR_DUPLICATE_DECL       201
#define IRON_ERR_TYPE_MISMATCH        202
#define IRON_ERR_VAL_REASSIGN         203
#define IRON_ERR_NULLABLE_ACCESS      204
#define IRON_ERR_MISSING_IFACE_METHOD 205
#define IRON_ERR_GENERIC_CONSTRAINT   206
#define IRON_ERR_ESCAPE_NO_FREE       207
#define IRON_ERR_PARALLEL_MUTATION    208
#define IRON_ERR_IMPORT_NOT_FOUND     209
#define IRON_ERR_SELF_OUTSIDE_METHOD  210
#define IRON_ERR_SUPER_NO_PARENT      211
#define IRON_ERR_FREE_NON_HEAP        212
#define IRON_ERR_LEAK_NON_HEAP        213
#define IRON_ERR_LEAK_RC              214
#define IRON_ERR_RETURN_TYPE          215
#define IRON_ERR_ARG_COUNT            216
#define IRON_ERR_ARG_TYPE             217
#define IRON_ERR_NOT_CALLABLE         218
#define IRON_ERR_NO_SUCH_FIELD        219
#define IRON_ERR_NO_SUCH_METHOD       220
#define IRON_ERR_PRIVATE_ACCESS       221
#define IRON_ERR_NUMERIC_CONVERSION   222
#define IRON_ERR_CIRCULAR_TYPE        223

/* Comptime errors */
#define IRON_ERR_COMPTIME_STEP_LIMIT  230
#define IRON_ERR_COMPTIME_RESTRICTION 231
#define IRON_ERR_COMPTIME_ERROR       232

/* IR verifier errors */
#define IRON_ERR_LIR_MISSING_TERMINATOR     300
#define IRON_ERR_LIR_INVALID_BRANCH_TARGET  301
#define IRON_ERR_LIR_USE_BEFORE_DEF         302
#define IRON_ERR_LIR_INSTR_AFTER_TERMINATOR 303
#define IRON_ERR_LIR_NO_ENTRY_BLOCK         304
#define IRON_ERR_LIR_RETURN_TYPE_MISMATCH   305

/* Lowering error codes (400 range) */
#define IRON_ERR_LOWER_UNSUPPORTED         400
#define IRON_ERR_LOWER_UNRESOLVED_IDENT    401
#define IRON_ERR_LOWER_INVALID_ASSIGN      402
#define IRON_ERR_LOWER_INVALID_MATCH       403

#endif /* IRON_DIAGNOSTICS_H */
