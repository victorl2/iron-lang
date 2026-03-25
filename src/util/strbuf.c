#include "strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void strbuf_grow(Iron_StrBuf *sb, size_t needed) {
    size_t new_cap = sb->capacity > 0 ? sb->capacity * 2 : 64;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    char *new_data = (char *)realloc(sb->data, new_cap);
    if (new_data == NULL) {
        return; /* allocation failure — silent; callers check get() */
    }
    sb->data     = new_data;
    sb->capacity = new_cap;
}

Iron_StrBuf iron_strbuf_create(size_t initial_capacity) {
    Iron_StrBuf sb;
    sb.data     = (char *)malloc(initial_capacity);
    sb.len      = 0;
    sb.capacity = initial_capacity;
    if (sb.data != NULL) {
        sb.data[0] = '\0';
    }
    return sb;
}

void iron_strbuf_append(Iron_StrBuf *sb, const char *str, size_t len) {
    size_t needed = sb->len + len + 1;
    if (needed > sb->capacity) {
        strbuf_grow(sb, needed);
    }
    if (sb->data == NULL) {
        return;
    }
    memcpy(sb->data + sb->len, str, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
}

void iron_strbuf_appendf(Iron_StrBuf *sb, const char *fmt, ...) {
    va_list args;

    /* First pass: measure the output length. */
    va_start(args, fmt);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (needed < 0) {
        return; /* encoding error */
    }

    size_t new_len = sb->len + (size_t)needed + 1;
    if (new_len > sb->capacity) {
        strbuf_grow(sb, new_len);
    }
    if (sb->data == NULL) {
        return;
    }

    /* Second pass: write into the buffer. */
    va_start(args, fmt);
    vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, args);
    va_end(args);

    sb->len += (size_t)needed;
}

void iron_strbuf_append_char(Iron_StrBuf *sb, char c) {
    size_t needed = sb->len + 2; /* char + null terminator */
    if (needed > sb->capacity) {
        strbuf_grow(sb, needed);
    }
    if (sb->data == NULL) {
        return;
    }
    sb->data[sb->len] = c;
    sb->len += 1;
    sb->data[sb->len] = '\0';
}

const char *iron_strbuf_get(Iron_StrBuf *sb) {
    return sb->data;
}

void iron_strbuf_free(Iron_StrBuf *sb) {
    free(sb->data);
    sb->data     = NULL;
    sb->len      = 0;
    sb->capacity = 0;
}

void iron_strbuf_reset(Iron_StrBuf *sb) {
    sb->len = 0;
    if (sb->data != NULL) {
        sb->data[0] = '\0';
    }
}
