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
    /* Phase 50: Initialize tracked pointer registry */
    a.tracked_ptrs  = NULL;
    a.tracked_count = 0;
    a.tracked_cap   = 0;
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
    /* Phase 50: Free all tracked pointers (collection sub-arrays) */
    for (int i = 0; i < a->tracked_count; i++) {
        free(a->tracked_ptrs[i]);
    }
    free(a->tracked_ptrs);
    a->tracked_ptrs = NULL;
    a->tracked_count = 0;
    a->tracked_cap = 0;

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

/* FIX-03 / AUDIT-04 §16: SAFETY — iron_arena_track aliasing contract.
 *
 * `iron_arena_track` adds `ptr` to the arena's tracked-pointer registry
 * (`a->tracked_ptrs`). At `iron_arena_free` time, every tracked pointer is
 * passed to the libc `free()` function (see loop at lines ~86-88 above).
 *
 * CALLER CONTRACT (enforced by convention, NOT by runtime assertion):
 *
 *   1. `ptr` MUST be a pointer returned by `malloc`, `calloc`, `realloc`,
 *      or `strdup` (anything free()-compatible). Passing a pointer
 *      obtained from any other allocator (stack, arena chunk data, mmap,
 *      aligned_alloc with non-power-of-two alignment, etc.) is undefined
 *      behavior at iron_arena_free time.
 *
 *   2. `ptr` MUST NOT alias any arena-internal memory — specifically, it
 *      MUST NOT point into any `Iron_ArenaChunk::data[]` byte range of
 *      this arena or any other arena. Aliasing would cause free() to be
 *      called on bytes that are ALSO freed by the chunk-walk loop at
 *      lines ~97-101, producing a double-free SIGSEGV.
 *
 *   3. `ptr` MUST have exclusive ownership handed to the arena: callers
 *      MUST NOT call free(ptr) themselves after tracking, MUST NOT pass
 *      the same pointer to two different arenas, and MUST NOT retain the
 *      pointer past iron_arena_free.
 *
 * Current callers (audited 2026-04-13):
 *   - Phase 50 SplitList sub-arrays allocated via `malloc` in the Iron
 *     runtime's collection scaffolding (src/runtime/iron_runtime.h
 *     IRON_LIST_IMPL / IRON_SPLITLIST sites) and tracked at insertion
 *     time so they're reclaimed when the compilation arena is freed.
 *   - Phase 50 iron_arena_realloc_tracked wraps this same registry for
 *     realloc-by-pointer substitution (see function below).
 *
 * Every current caller uses pure heap-malloc'd pointers disjoint from
 * arena chunks. The contract is upheld by convention. */
void *iron_arena_track(Iron_Arena *a, void *ptr) {
    if (!ptr) return NULL;
    if (a->tracked_count >= a->tracked_cap) {
        a->tracked_cap = a->tracked_cap ? a->tracked_cap * 2 : 8;
        a->tracked_ptrs = (void **)realloc(a->tracked_ptrs,
            (size_t)a->tracked_cap * sizeof(void *));
    }
    a->tracked_ptrs[a->tracked_count++] = ptr;
    return ptr;
}

void *iron_arena_realloc_tracked(Iron_Arena *a, void *old_ptr, size_t new_size) {
    void *new_ptr = realloc(old_ptr, new_size);
    if (!new_ptr) return NULL;
    if (old_ptr) {
        /* Update existing tracked entry */
        for (int i = 0; i < a->tracked_count; i++) {
            if (a->tracked_ptrs[i] == old_ptr) {
                a->tracked_ptrs[i] = new_ptr;
                return new_ptr;
            }
        }
    }
    /* Not found (or old_ptr was NULL) -- track as new */
    return iron_arena_track(a, new_ptr);
}
