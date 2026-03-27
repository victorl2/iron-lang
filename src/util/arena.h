#ifndef IRON_ARENA_H
#define IRON_ARENA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Arena allocator — linked-list of fixed-size chunks (bump pointer per chunk).
 * Never moves existing allocations, so pointers remain valid after growth.
 * All allocations are bump-pointer; no individual frees.
 * iron_arena_free() releases all memory at once.
 */

#define IRON_ARENA_CHUNK_SIZE (256 * 1024)  /* 256 KB per chunk */

typedef struct Iron_ArenaChunk {
    struct Iron_ArenaChunk *next;
    size_t   used;
    size_t   capacity;
    uint8_t  data[1];  /* flexible array — actual size is capacity */
} Iron_ArenaChunk;

typedef struct {
    Iron_ArenaChunk *head;   /* current (most recently allocated) chunk */
    Iron_ArenaChunk *first;  /* first chunk in chain (for free) */
    size_t   chunk_size;     /* default chunk size for new chunks */
    /* Legacy fields kept for any code that reads them (treated as 0/capacity of head) */
    uint8_t *base;      /* points into head->data, or NULL */
    size_t   used;      /* mirrors head->used */
    size_t   capacity;  /* mirrors head->capacity */
} Iron_Arena;

/* Create a new arena with the given initial capacity in bytes. */
Iron_Arena iron_arena_create(size_t capacity);

/* Allocate `size` bytes with `align` alignment.
 * Grows the arena by adding a new chunk if needed.
 * Returns NULL only if malloc fails.
 * Existing pointers are NEVER invalidated.
 */
void *iron_arena_alloc(Iron_Arena *a, size_t size, size_t align);

/* Copy `len` bytes of `src` into the arena as a null-terminated string.
 * Returns pointer to the copied string.
 */
char *iron_arena_strdup(Iron_Arena *a, const char *src, size_t len);

/* Free all memory held by the arena. Sets all fields to zero. */
void iron_arena_free(Iron_Arena *a);

/* Typed allocation helper — allocates sizeof(T) aligned to _Alignof(T). */
#define ARENA_ALLOC(arena, T) ((T*)iron_arena_alloc((arena), sizeof(T), _Alignof(T)))

#endif /* IRON_ARENA_H */
