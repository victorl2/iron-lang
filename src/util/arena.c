#include "arena.h"

#include <stdlib.h>
#include <string.h>

/* Allocate a new chunk with at least `min_capacity` usable bytes. */
static Iron_ArenaChunk *arena_new_chunk(size_t min_capacity, size_t default_chunk_size) {
    size_t cap = default_chunk_size > min_capacity ? default_chunk_size : min_capacity;
    /* Double until large enough (for single large allocations). */
    while (cap < min_capacity) cap *= 2;
    size_t alloc_size = offsetof(Iron_ArenaChunk, data) + cap;
    Iron_ArenaChunk *chunk = (Iron_ArenaChunk *)malloc(alloc_size);
    if (!chunk) return NULL;
    chunk->next     = NULL;
    chunk->used     = 0;
    chunk->capacity = cap;
    return chunk;
}

Iron_Arena iron_arena_create(size_t capacity) {
    Iron_Arena a;
    /* Initial chunk uses the requested capacity.
     * Subsequent chunks use IRON_ARENA_CHUNK_SIZE for amortized growth. */
    Iron_ArenaChunk *chunk = arena_new_chunk(capacity, capacity);
    a.head       = chunk;
    a.first      = chunk;
    a.chunk_size = IRON_ARENA_CHUNK_SIZE;  /* default size for future chunks */
    /* Sync legacy fields */
    a.base     = chunk ? chunk->data : NULL;
    a.used     = 0;
    a.capacity = chunk ? chunk->capacity : 0;
    return a;
}

void *iron_arena_alloc(Iron_Arena *a, size_t size, size_t align) {
    if (size == 0) {
        return NULL;
    }

    Iron_ArenaChunk *chunk = a->head;
    if (!chunk) return NULL;

    /* Align up within current chunk */
    size_t aligned = (chunk->used + align - 1) & ~(align - 1);
    size_t new_used = aligned + size;

    if (new_used > chunk->capacity) {
        /* Current chunk is full — allocate a new one and prepend as head */
        Iron_ArenaChunk *new_chunk = arena_new_chunk(size + align, a->chunk_size);
        if (!new_chunk) return NULL;
        new_chunk->next = chunk;
        a->head         = new_chunk;
        chunk           = new_chunk;
        aligned         = (chunk->used + align - 1) & ~(align - 1);
        new_used        = aligned + size;
    }

    chunk->used = new_used;
    void *ptr = chunk->data + aligned;
    memset(ptr, 0, size);

    /* Sync legacy fields to head chunk */
    a->base     = a->head->data;
    a->used     = a->head->used;
    a->capacity = a->head->capacity;

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
    /* Walk from head — new chunks are prepended, so head is newest, last is oldest.
     * The chain is: head -> newer -> ... -> first (oldest).
     * We simply walk and free every chunk in the chain. */
    Iron_ArenaChunk *chunk = a->head;
    while (chunk) {
        Iron_ArenaChunk *next = chunk->next;
        free(chunk);
        chunk = next;
    }
    a->head     = NULL;
    a->first    = NULL;
    a->base     = NULL;
    a->used     = 0;
    a->capacity = 0;
}
