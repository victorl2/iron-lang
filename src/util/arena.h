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

    /* Phase 50: Pointer registry for tracked allocations (collection sub-arrays).
     * Tracks malloc'd pointers so iron_arena_free() can release them all at once.
     * Uses a simple dynamic array (not stb_ds, to avoid header dependency). */
    void   **tracked_ptrs;     /* dynamic array of tracked pointers */
    int      tracked_count;    /* number of tracked pointers */
    int      tracked_cap;      /* capacity of tracked_ptrs array */

    /* Cleanup hooks run by iron_arena_free before the chunks go away, for
     * heap structures (stb_ds arrays and maps) stored in arena objects. */
    struct Iron_ArenaHook *hooks;
    int      hook_count;
    int      hook_cap;
} Iron_Arena;

typedef struct Iron_ArenaHook {
    void (*fn)(void *ctx);
    void *ctx;
} Iron_ArenaHook;

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

/* Track an externally malloc'd pointer for bulk free.
 * The pointer will be free'd when iron_arena_free() is called.
 * Returns the same pointer for convenience. */
void *iron_arena_track(Iron_Arena *a, void *ptr);

/* Realloc a tracked pointer, updating the registry entry.
 * If old_ptr is NULL, equivalent to malloc + track.
 * Returns the new pointer (or NULL on failure). */
void *iron_arena_realloc_tracked(Iron_Arena *a, void *old_ptr, size_t new_size);

/* Run fn(ctx) when the arena is freed, before its memory is released, so
 * ctx may point into the arena. Hooks run in reverse registration order.
 * Returns false (and the hook is not registered) on allocation failure. */
bool iron_arena_on_free(Iron_Arena *a, void (*fn)(void *ctx), void *ctx);

/* Free the stb_ds array held in *slot when the arena is freed. The slot,
 * not the array, is recorded: the array may grow (and move) afterwards,
 * and whatever *slot holds at free time is released. *slot must hold an
 * stb_ds array or NULL for the arena's whole lifetime, and no other slot
 * may own the same array. */
void iron_arena_own_arr(Iron_Arena *a, void **slot);

/* Arena arrays: the storage for every AST node array (block statements,
 * call arguments, parameters, ...). The elements live in the arena behind a
 * header laid out like stb_ds's, with length == capacity == count, so
 * arrlen() on one reads the right length. They must never be passed to an
 * stb_ds mutator: with the capacity always full, arrput reallocs and
 * arrfree frees arena memory, and both abort. Grow one with
 * IRON_ARENA_ARR_PUSH, which copies it into a new arena block; the old
 * block goes with the arena. */

/* A zeroed arena array of `count` elements; NULL when count is 0 or the
 * allocation fails. */
void *iron_arena_arr_new(Iron_Arena *a, size_t count, size_t elem_size);

/* Copy the stb_ds array `stb_arr` into a new arena array and arrfree it.
 * Returns NULL for a NULL or empty array (and on allocation failure, after
 * freeing it all the same). */
void *iron_arena_arr_adopt(Iron_Arena *a, void *stb_arr, size_t elem_size);

/* A new arena array holding the first `count` elements of `arr` (an arena
 * array, plain arena memory or NULL) followed by `*elem`. */
void *iron_arena_arr_append(Iron_Arena *a, const void *arr, size_t count,
                            const void *elem, size_t elem_size);

/* Replace the stb_ds array in `arr` with an arena copy of it. */
#define IRON_ARENA_ARR_ADOPT(arena, arr) \
    ((arr) = iron_arena_arr_adopt((arena), (void *)(arr), sizeof *(arr)))

/* Append the pointer `v` to the arena array of pointers `arr`, which holds
 * `count` elements. The caller updates its count field. */
#define IRON_ARENA_ARR_PUSH(arena, arr, count, v) do {                       \
        _Static_assert(sizeof *(arr) == sizeof(void *),                      \
                       "IRON_ARENA_ARR_PUSH appends pointers");             \
        const void *iron_arr_push_v_ = (v);                                  \
        (arr) = iron_arena_arr_append((arena), (arr), (size_t)(count),       \
                                      &iron_arr_push_v_, sizeof(void *));    \
    } while (0)

/* Typed allocation helper — allocates sizeof(T) aligned to _Alignof(T). */
#define ARENA_ALLOC(arena, T) ((T*)iron_arena_alloc((arena), sizeof(T), _Alignof(T)))

#endif /* IRON_ARENA_H */
