#ifndef IRON_ARENA_H
#define IRON_ARENA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Arena allocator — linear bump allocator with growth via realloc.
 * All allocations are bump-pointer; no individual frees.
 * iron_arena_free() releases all memory at once.
 */
typedef struct {
    uint8_t *base;
    size_t   used;
    size_t   capacity;
} Iron_Arena;

/* Create a new arena with the given initial capacity in bytes. */
Iron_Arena iron_arena_create(size_t capacity);

/* Allocate `size` bytes with `align` alignment.
 * Grows the arena via realloc if needed (doubles capacity).
 * Returns NULL only if malloc/realloc fails.
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
