/* iron_collections.c — Pre-instantiated collection implementations
 *
 * This file provides the function bodies for the most common monomorphized
 * collection types.  The Iron compiler's monomorphization pass (gen_types.c
 * ensure_monomorphized_type) emits struct typedefs + prototype stubs.  These
 * IRON_*_IMPL macro expansions provide the matching definitions.
 *
 * Naming follows gen_types.c mangle_generic() exactly:
 *   List[Int]        -> Iron_List_int64_t
 *   Map[String, Int] -> Iron_Map_Iron_String_int64_t
 *   Set[Int]         -> Iron_Set_int64_t
 *
 * For new element types the caller can expand the macros in any .c file that
 * includes iron_runtime.h — only one TU may do so per type to avoid ODR
 * violations.
 */

#include <stdlib.h>
#include <string.h>

#include "runtime/iron_runtime.h"

/* ── Equality helpers ────────────────────────────────────────────────────── */

static bool iron_string_eq_ptr(const Iron_String *a, const Iron_String *b) {
    return iron_string_equals(a, b);
}

static bool int64_eq_ptr(const int64_t *a, const int64_t *b) {
    return *a == *b;
}

/* ── List implementations ─────────────────────────────────────────────────── */

IRON_LIST_IMPL(int64_t,     int64_t)
IRON_LIST_IMPL(int32_t,     int32_t)
IRON_LIST_IMPL(double,      double)
IRON_LIST_IMPL(bool,        bool)
/* A list of strings owns its elements' shares of their characters (#182):
 * a copy of the list retains each element, clear and free release them. */
IRON_LIST_IMPL_CORE(Iron_String, Iron_String)
Iron_List_Iron_String Iron_List_Iron_String_clone(const Iron_List_Iron_String *src) {
    Iron_List_Iron_String dst;
    dst.count = src->count;
    dst.capacity = src->count;
    dst.items = NULL;
    if (src->count > 0) {
        dst.items = (Iron_String *)malloc((size_t)src->count * sizeof(Iron_String));
        if (!dst.items) iron_oom_abort("Iron_List_Iron_String_clone");
        memcpy(dst.items, src->items, (size_t)src->count * sizeof(Iron_String));
        for (int64_t i = 0; i < dst.count; i++) iron_string_retain(&dst.items[i]);
    }
    return dst;
}
void Iron_List_Iron_String_clear(Iron_List_Iron_String *self) {
    for (int64_t i = 0; i < self->count; i++) iron_string_release(&self->items[i]);
    self->count = 0;
}
void Iron_List_Iron_String_free(Iron_List_Iron_String *self) {
    for (int64_t i = 0; i < self->count; i++) iron_string_release(&self->items[i]);
    free(self->items);
    self->items = NULL; self->count = 0; self->capacity = 0;
}
/* Closure envs are counted (#190). */
typedef struct { _Atomic uint64_t rc; void (*drop)(void *env); } IronClosureEnvHdr;

void *iron_closure_env_alloc(size_t env_size, void (*drop)(void *env)) {
    IronClosureEnvHdr *h = (IronClosureEnvHdr *)malloc(sizeof(IronClosureEnvHdr) + env_size);
    if (!h) iron_oom_abort("iron_closure_env_alloc");
    atomic_init(&h->rc, 1);
    h->drop = drop;
    return (void *)(h + 1);
}

void iron_closure_env_free(void *env) {
    if (env) free(((IronClosureEnvHdr *)env) - 1);
}

void iron_closure_retain(Iron_Closure c) {
    if (!c.env) return;
    (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED((((IronClosureEnvHdr *)c.env) - 1)->rc, 1);
}

void iron_closure_release(Iron_Closure c) {
    if (!c.env) return;
    IronClosureEnvHdr *h = ((IronClosureEnvHdr *)c.env) - 1;
    uint64_t prev = IRON_ATOMIC_U64_FETCH_SUB_RELEASE(h->rc, 1);
    if (prev == 1) {
        IRON_ATOMIC_FENCE_ACQUIRE();
        if (h->drop) h->drop(c.env); else iron_closure_env_free(c.env);
    }
}

/* A list of closures owns a share of each element's env. */
IRON_LIST_IMPL_CORE(Iron_Closure, Iron_Closure)
Iron_List_Iron_Closure Iron_List_Iron_Closure_clone(const Iron_List_Iron_Closure *src) {
    Iron_List_Iron_Closure dst;
    dst.count = src->count;
    dst.capacity = src->count;
    dst.items = NULL;
    if (src->count > 0) {
        dst.items = (Iron_Closure *)malloc((size_t)src->count * sizeof(Iron_Closure));
        if (!dst.items) iron_oom_abort("Iron_List_Iron_Closure_clone");
        memcpy(dst.items, src->items, (size_t)src->count * sizeof(Iron_Closure));
        for (int64_t i = 0; i < dst.count; i++) iron_closure_retain(dst.items[i]);
    }
    return dst;
}
void Iron_List_Iron_Closure_clear(Iron_List_Iron_Closure *self) {
    for (int64_t i = 0; i < self->count; i++) iron_closure_release(self->items[i]);
    self->count = 0;
}
void Iron_List_Iron_Closure_free(Iron_List_Iron_Closure *self) {
    for (int64_t i = 0; i < self->count; i++) iron_closure_release(self->items[i]);
    free(self->items);
    self->items = NULL; self->count = 0; self->capacity = 0;
}
/* Phase 68 (Plan 68-01): ABI-FLOAT32 + ABI-UINT8 implementations.
 * Suffix matches ironc emit_type_to_c output: Float32 → "float",
 * UInt8 → "uint8_t". */
IRON_LIST_IMPL(float,       float)
IRON_LIST_IMPL(uint8_t,     uint8_t)

/* ── Collection method implementations (map, filter, reduce, forEach, sum) ── */

IRON_LIST_COLL_IMPL(int64_t, int64_t, 0)
IRON_LIST_COLL_IMPL(int32_t, int32_t, 0)
IRON_LIST_COLL_IMPL(double,  double,  0.0)
/* Phase 68 (Plan 68-01): map/filter/reduce/forEach/sum for float +
 * uint8_t.  Audio consumers rarely call these on raw sample buffers, but
 * keeping the COLL_IMPL in parity with the primitive types avoids future
 * surprise if users do. */
IRON_LIST_COLL_IMPL(float,   float,   0.0f)
IRON_LIST_COLL_IMPL(uint8_t, uint8_t, 0)

/* ── Map implementations ──────────────────────────────────────────────────── */

IRON_MAP_IMPL(Iron_String, int64_t,     Iron_String, int64_t,     iron_string_eq_ptr)
IRON_MAP_IMPL(Iron_String, Iron_String, Iron_String, Iron_String, iron_string_eq_ptr)

/* ── Set implementations ──────────────────────────────────────────────────── */

IRON_SET_IMPL(int64_t,     int64_t,     int64_eq_ptr)
IRON_SET_IMPL(Iron_String, Iron_String, iron_string_eq_ptr)
