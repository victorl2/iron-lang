#include "arena.h"

#include <stdlib.h>
#include <string.h>

Iron_Arena iron_arena_create(size_t capacity) {
    Iron_Arena a;
    a.base     = (uint8_t *)malloc(capacity);
    a.used     = 0;
    a.capacity = (a.base != NULL) ? capacity : 0;
    return a;
}

void *iron_arena_alloc(Iron_Arena *a, size_t size, size_t align) {
    if (size == 0) {
        return NULL;
    }

    /* Align up: round `used` to nearest multiple of `align`. */
    size_t aligned = (a->used + align - 1) & ~(align - 1);
    size_t new_used = aligned + size;

    /* Grow if needed — double capacity until large enough. */
    if (new_used > a->capacity) {
        size_t new_cap = a->capacity > 0 ? a->capacity * 2 : 64;
        while (new_cap < new_used) {
            new_cap *= 2;
        }
        uint8_t *new_base = (uint8_t *)realloc(a->base, new_cap);
        if (new_base == NULL) {
            return NULL;
        }
        a->base     = new_base;
        a->capacity = new_cap;
    }

    a->used = new_used;
    void *ptr = a->base + aligned;
    memset(ptr, 0, size);
    return ptr;
}

char *iron_arena_strdup(Iron_Arena *a, const char *src, size_t len) {
    char *dst = (char *)iron_arena_alloc(a, len + 1, 1);
    if (dst == NULL) {
        return NULL;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return dst;
}

void iron_arena_free(Iron_Arena *a) {
    free(a->base);
    a->base     = NULL;
    a->used     = 0;
    a->capacity = 0;
}
