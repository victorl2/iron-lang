#ifndef IRON_STRBUF_H
#define IRON_STRBUF_H

#include <stddef.h>

/* Dynamic string builder — grows on demand, always null-terminated.
 * Suitable for diagnostic output, pretty-printing, and code generation.
 */
typedef struct {
    char  *data;
    size_t len;
    size_t capacity;
} Iron_StrBuf;

/* Create a new string builder with the given initial capacity. */
Iron_StrBuf iron_strbuf_create(size_t initial_capacity);

/* Append `len` bytes from `str` to the buffer. */
void iron_strbuf_append(Iron_StrBuf *sb, const char *str, size_t len);

/* Append a printf-style formatted string to the buffer. */
void iron_strbuf_appendf(Iron_StrBuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Append a single character to the buffer. */
void iron_strbuf_append_char(Iron_StrBuf *sb, char c);

/* Return the current null-terminated string contents. */
const char *iron_strbuf_get(Iron_StrBuf *sb);

/* Free the underlying buffer and zero all fields. */
void iron_strbuf_free(Iron_StrBuf *sb);

/* Reset the buffer to empty without freeing memory. */
void iron_strbuf_reset(Iron_StrBuf *sb);

#endif /* IRON_STRBUF_H */
