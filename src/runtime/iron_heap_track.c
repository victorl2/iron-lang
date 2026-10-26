/* iron_heap_track.c — Phase 19 generational pointer tracker implementation.
 *
 * Header-prepended allocation: each iron_heap_alloc returns a pointer to
 * user payload; the IronAllocHdr immediately precedes that pointer.
 * Recovery is O(1) pointer arithmetic (no global table, no lock contention).
 *
 * Atomicity: generation increments use memory_order_relaxed (monotonic
 * counter; no surrounding writes to publish). Deref-check loads use
 * memory_order_acquire (synchronizes with the freeing thread's relaxed-
 * fetch-add on the same atomic). Pattern: Linux kernel refcount.h.
 *
 * Lifecycle: iron_alloc_id_counter is initialized to 0 in iron_runtime_init
 * (src/runtime/iron_string.c). Phase 31 debug allocator extends this with
 * leak detection / poison-on-free / double-free poisoning.
 *
 * Layout lock: see docs/dev/POINTER-LAYOUT.md (Plan 19-03 closeout).
 * Single-TU implementation discipline: this is the ONLY TU defining the
 * iron_heap_alloc / iron_heap_free symbols.
 */

#include "runtime/iron_runtime.h"
#include "runtime/iron_heap_track.h"
#include "diagnostics/diagnostics.h"  /* iron_oom_abort */

#include <stdint.h>
#include <stdio.h>   /* fprintf, fflush */
#include <stdlib.h>  /* malloc, free, abort */
#include <string.h>  /* memset (DBG-01 poison) */
#include <stdbool.h>

/* Process-global allocation-id counter; declaration in iron_runtime.h. */
iron_atomic_u64 iron_alloc_id_counter;

/* ── Generation table (iron_runtime.h, #539 #540 #413 #432) ───────────────
 *
 * Two levels: IRON_GEN_CHUNKS pointers to chunks of 2^16 entries, each chunk
 * allocated on first use and never freed, so a check reads only memory the
 * runtime owns for the life of the process. An entry holds the live gen of
 * its slot, or that gen with bit 63 set once it ended (bit 63 never appears
 * in a slot-form pointer gen, so a dead entry matches nothing).
 *
 * Free slots are cached per thread (up to IRON_GEN_TCACHE) and otherwise
 * kept on a global stack under a spin lock; fresh slots come from a
 * counter. A slot whose sequence number is exhausted is retired. */
_Atomic(iron_atomic_u64 *) iron_gen_chunks[IRON_GEN_CHUNKS];

#define IRON_GEN_TCACHE 64
static _Thread_local uint32_t t_gen_free[IRON_GEN_TCACHE];
static _Thread_local int      t_gen_nfree;
static atomic_flag            s_gen_lock = ATOMIC_FLAG_INIT;
static uint32_t              *s_gen_free;        /* global free stack */
static size_t                 s_gen_nfree, s_gen_cap;
static _Atomic uint32_t       s_gen_fresh = 1;   /* slot 0 is reserved */

static void gen_lock(void)   { while (atomic_flag_test_and_set_explicit(&s_gen_lock, memory_order_acquire)) { } }
static void gen_unlock(void) { atomic_flag_clear_explicit(&s_gen_lock, memory_order_release); }

static iron_atomic_u64 *gen_entry(uint32_t slot) {
    _Atomic(iron_atomic_u64 *) *cp = &iron_gen_chunks[slot >> IRON_GEN_CHUNK_BITS];
    iron_atomic_u64 *c = atomic_load_explicit(cp, memory_order_acquire);
    if (!c) {
        iron_atomic_u64 *nc = (iron_atomic_u64 *)calloc((size_t)1 << IRON_GEN_CHUNK_BITS,
                                                        sizeof(iron_atomic_u64));
        if (!nc) iron_oom_abort("generation table");
        iron_atomic_u64 *expect = NULL;
        if (atomic_compare_exchange_strong_explicit(cp, &expect, nc, memory_order_acq_rel,
                                                    memory_order_acquire)) {
            c = nc;
            /* Slot 0 never holds a live gen: a null pointer (gen 0) and a
             * zeroed entry must not match. */
            if ((slot >> IRON_GEN_CHUNK_BITS) == 0)
                atomic_store_explicit(&c[0], IRON_STACK_TAG, memory_order_relaxed);
        } else {
            free(nc);
            c = expect;
        }
    }
    return &c[slot & ((1u << IRON_GEN_CHUNK_BITS) - 1)];
}

static uint32_t gen_take_slot(void) {
    if (t_gen_nfree > 0) return t_gen_free[--t_gen_nfree];
    gen_lock();
    while (s_gen_nfree > 0 && t_gen_nfree < IRON_GEN_TCACHE / 2)
        t_gen_free[t_gen_nfree++] = s_gen_free[--s_gen_nfree];
    gen_unlock();
    if (t_gen_nfree > 0) return t_gen_free[--t_gen_nfree];
    uint32_t s = atomic_fetch_add_explicit(&s_gen_fresh, 1, memory_order_relaxed);
    if (s >= (1u << IRON_GEN_SLOT_BITS))
        iron_oom_abort("generation table: too many live allocations");
    return s;
}

static void gen_give_slot(uint32_t s) {
    if (t_gen_nfree == IRON_GEN_TCACHE) {
        gen_lock();
        if (s_gen_nfree + IRON_GEN_TCACHE / 2 > s_gen_cap) {
            size_t cap = s_gen_cap ? s_gen_cap * 2 : 1024;
            uint32_t *n = (uint32_t *)realloc(s_gen_free, cap * sizeof(uint32_t));
            if (!n) { gen_unlock(); iron_oom_abort("generation table"); }
            s_gen_free = n;
            s_gen_cap = cap;
        }
        while (t_gen_nfree > IRON_GEN_TCACHE / 2)
            s_gen_free[s_gen_nfree++] = t_gen_free[--t_gen_nfree];
        gen_unlock();
    }
    t_gen_free[t_gen_nfree++] = s;
}

uint64_t iron_gen_new(bool arena) {
    for (;;) {
        uint32_t s = gen_take_slot();
        iron_atomic_u64 *e = gen_entry(s);
        uint64_t old = atomic_load_explicit(e, memory_order_relaxed);
        uint64_t seq = (old & IRON_GEN_SEQ_MASK) + 1;
        if (seq > IRON_GEN_SEQ_MASK) continue;          /* exhausted: retire */
        uint64_t g = IRON_GEN_SLOT_TAG | (arena ? IRON_GEN_ARENA_BIT : 0) |
                     ((uint64_t)s << IRON_GEN_SLOT_SHIFT) | seq;
        atomic_store_explicit(e, g, memory_order_release);
        return g;
    }
}

/* Receiver pins (iron_runtime.h): few at a time, in an array under a
 * spin lock; iron_gen_pin_count is read without it on every end. */
_Atomic uint64_t iron_gen_pin_count;
static atomic_flag s_gpin_lock = ATOMIC_FLAG_INIT;
static uint64_t *s_gpins;
static size_t    s_ngpins, s_gpincap;

void iron_gen_pin(uint64_t gen) {
    if (!(gen & IRON_GEN_SLOT_TAG) || (gen & IRON_STACK_TAG)) return;
    while (atomic_flag_test_and_set_explicit(&s_gpin_lock, memory_order_acquire)) { }
    if (s_ngpins == s_gpincap) {
        size_t cap = s_gpincap ? s_gpincap * 2 : 16;
        uint64_t *n = (uint64_t *)realloc(s_gpins, cap * sizeof(uint64_t));
        if (!n) { atomic_flag_clear_explicit(&s_gpin_lock, memory_order_release);
                  iron_oom_abort("receiver pins"); }
        s_gpins = n;
        s_gpincap = cap;
    }
    s_gpins[s_ngpins++] = gen;
    atomic_fetch_add_explicit(&iron_gen_pin_count, 1, memory_order_relaxed);
    atomic_flag_clear_explicit(&s_gpin_lock, memory_order_release);
}

void iron_gen_unpin(uint64_t gen) {
    if (!(gen & IRON_GEN_SLOT_TAG) || (gen & IRON_STACK_TAG)) return;
    while (atomic_flag_test_and_set_explicit(&s_gpin_lock, memory_order_acquire)) { }
    for (size_t i = s_ngpins; i > 0; i--) {
        if (s_gpins[i - 1] != gen) continue;
        s_gpins[i - 1] = s_gpins[--s_ngpins];
        atomic_fetch_sub_explicit(&iron_gen_pin_count, 1, memory_order_relaxed);
        break;
    }
    atomic_flag_clear_explicit(&s_gpin_lock, memory_order_release);
}

static bool gen_is_pinned(uint64_t gen) {
    bool p = false;
    while (atomic_flag_test_and_set_explicit(&s_gpin_lock, memory_order_acquire)) { }
    for (size_t i = 0; i < s_ngpins && !p; i++) p = s_gpins[i] == gen;
    atomic_flag_clear_explicit(&s_gpin_lock, memory_order_release);
    return p;
}

bool iron_gen_end(uint64_t gen) {
    if (!(gen & IRON_GEN_SLOT_TAG) || (gen & IRON_STACK_TAG)) return false;
    if (atomic_load_explicit(&iron_gen_pin_count, memory_order_relaxed) && gen_is_pinned(gen) &&
        iron_gen_live(gen))
        iron_panic_receiver_pinned(gen);
    uint32_t s = IRON_GEN_SLOT_OF(gen);
    if (s == 0) return false;
    iron_atomic_u64 *c = atomic_load_explicit(&iron_gen_chunks[s >> IRON_GEN_CHUNK_BITS],
                                              memory_order_acquire);
    if (!c) return false;
    iron_atomic_u64 *e = &c[s & ((1u << IRON_GEN_CHUNK_BITS) - 1)];
    uint64_t expect = gen;
    if (!atomic_compare_exchange_strong_explicit(e, &expect, gen | IRON_STACK_TAG,
                                                 memory_order_release, memory_order_relaxed))
        return false;
    if ((gen & IRON_GEN_SEQ_MASK) < IRON_GEN_SEQ_MASK) gen_give_slot(s);
    return true;
}

void iron_gen_thread_exit(void) {
    if (t_gen_nfree == 0) return;
    gen_lock();
    if (s_gen_nfree + (size_t)t_gen_nfree > s_gen_cap) {
        size_t cap = s_gen_cap ? s_gen_cap * 2 : 1024;
        while (cap < s_gen_nfree + (size_t)t_gen_nfree) cap *= 2;
        uint32_t *n = (uint32_t *)realloc(s_gen_free, cap * sizeof(uint32_t));
        if (!n) { gen_unlock(); return; }   /* the slots are only lost */
        s_gen_free = n;
        s_gen_cap = cap;
    }
    while (t_gen_nfree > 0) s_gen_free[s_gen_nfree++] = t_gen_free[--t_gen_nfree];
    gen_unlock();
}

/* Generations used to come from one counter; heap blocks now take a table
 * slot. Kept for callers outside the runtime. */
uint64_t iron_heap_next_gen(void) {
    return iron_gen_new(false);
}

/* ── List element borrows (iron_runtime.h, #543) ─────────────────────────
 * The pinned buffers and their pin counts, in a small array under a spin
 * lock: pins are few at any moment (one per call in progress that borrows
 * an element). iron_list_pin_count is their total, read without the lock
 * by the fast path of IRON_LIST_PIN_CHECK. */
_Atomic uint64_t iron_list_pin_count;
static atomic_flag s_pin_lock = ATOMIC_FLAG_INIT;
static struct { const void *buf; uint64_t count; int kind; } *s_pins;   /* kind 0 list, 1 receiver */
static size_t s_npins, s_pincap;

static void pin_lock(void)   { while (atomic_flag_test_and_set_explicit(&s_pin_lock, memory_order_acquire)) { } }
static void pin_unlock(void) { atomic_flag_clear_explicit(&s_pin_lock, memory_order_release); }

static void addr_pin(const void *buf, int kind) {
    pin_lock();
    size_t i = 0;
    while (i < s_npins && !(s_pins[i].buf == buf && s_pins[i].kind == kind)) i++;
    if (i == s_npins) {
        if (s_npins == s_pincap) {
            size_t cap = s_pincap ? s_pincap * 2 : 16;
            void *n = realloc(s_pins, cap * sizeof(*s_pins));
            if (!n) { pin_unlock(); iron_oom_abort("pins"); }
            s_pins = n;
            s_pincap = cap;
        }
        s_pins[s_npins].buf = buf;
        s_pins[s_npins].count = 0;
        s_pins[s_npins].kind = kind;
        s_npins++;
    }
    s_pins[i].count++;
    atomic_fetch_add_explicit(&iron_list_pin_count, 1, memory_order_relaxed);
    pin_unlock();
}

static void addr_unpin(const void *buf, int kind) {
    pin_lock();
    for (size_t i = 0; i < s_npins; i++) {
        if (s_pins[i].buf != buf || s_pins[i].kind != kind) continue;
        if (--s_pins[i].count == 0) s_pins[i] = s_pins[--s_npins];
        atomic_fetch_sub_explicit(&iron_list_pin_count, 1, memory_order_relaxed);
        break;
    }
    pin_unlock();
}

static bool addr_is_pinned(const void *buf, int kind) {
    bool pinned = false;
    pin_lock();
    for (size_t i = 0; i < s_npins && !pinned; i++)
        pinned = s_pins[i].buf == buf && s_pins[i].kind == kind;
    pin_unlock();
    return pinned;
}

uint64_t iron_list_pin(const void *buf) {
    if (buf) addr_pin(buf, 0);
    return iron_gen_new(false);
}

void iron_list_unpin(const void *buf, uint64_t gen) {
    (void)iron_gen_end(gen);
    if (buf) addr_unpin(buf, 0);
}

bool iron_list_is_pinned(const void *buf) { return addr_is_pinned(buf, 0); }
/* A for loop over a list pins its buffer while it runs (#734). */
void iron_list_iter_pin(const void *buf) { if (buf) addr_pin(buf, 0); }
void iron_list_iter_unpin(const void *buf) { if (buf) addr_unpin(buf, 0); }
void iron_recv_pin(const void *addr) { if (addr) addr_pin(addr, 1); }
void iron_recv_unpin(const void *addr) { if (addr) addr_unpin(addr, 1); }
bool iron_recv_is_pinned(const void *addr) { return addr_is_pinned(addr, 1); }

/* ── Stack scopes (iron_runtime.h, #471 #537 #538 #541) ──────────────────
 *
 * Each thread that enters a scope takes a number (1..4095) and registers
 * its thread-local scope block under it. At thread exit the number goes
 * back, together with the thread's last id, so the next thread to take the
 * number continues the id sequence: a pointer into a dead thread's scope
 * never matches a scope of the thread that reuses the number. A thread
 * that uses up the ids of its number takes another one; the old number
 * stays registered to the thread's block while the thread lives (scopes
 * opened under it may still be open) and is never given out again. Other
 * threads read a registered block only under the registry lock, which the
 * exit takes before the block disappears with its thread. */
_Thread_local IronStackTLS iron_stk = { .gen = IRON_STACK_GEN_INIT };
static atomic_flag s_stack_lock = ATOMIC_FLAG_INIT;
static struct {
    IronStackTLS *tls;        /* NULL when the number is free or retired */
    uint64_t      next_id;    /* the id sequence, kept across owners */
} s_stack_reg[IRON_STACK_THREADS];
static uint32_t s_stack_free[IRON_STACK_THREADS];
static uint32_t s_stack_nfree;
static uint32_t s_stack_next = 1;

static void stack_lock(void)   { while (atomic_flag_test_and_set_explicit(&s_stack_lock, memory_order_acquire)) { } }
static void stack_unlock(void) { atomic_flag_clear_explicit(&s_stack_lock, memory_order_release); }

/* A number with ids left, registered to this thread. Under the lock. */
static uint32_t stack_take_number(void) {
    uint32_t n = 0;
    while (s_stack_nfree > 0 && !n) {
        n = s_stack_free[--s_stack_nfree];
        if (s_stack_reg[n].next_id >= IRON_STACK_ID_MASK) n = 0;   /* used up: retired */
    }
    if (!n && s_stack_next < IRON_STACK_THREADS) n = s_stack_next++;
    if (n) {
        s_stack_reg[n].tls = &iron_stk;
        iron_stk.next_id = s_stack_reg[n].next_id;
        iron_stk.tno = n;
    }
    return n;
}

void iron_stack_attach(void) {
    stack_lock();
    uint32_t n = stack_take_number();
    stack_unlock();
    if (!n) iron_oom_abort("stack scopes: more than 4095 threads take addresses of locals");
}

void iron_stack_renumber(void) {
    stack_lock();
    /* The old number keeps pointing at this block until the thread exits. */
    s_stack_reg[iron_stk.tno].next_id = IRON_STACK_ID_MASK;
    uint32_t n = stack_take_number();
    iron_stk.renumbered = true;
    stack_unlock();
    if (!n) iron_oom_abort("stack scopes: scope ids used up");
}

static iron_atomic_u64 *stack_deep_slot(IronStackTLS *t, uint32_t d, bool make) {
    if (d > IRON_STACK_DEPTH_MAX) {
        if (make) iron_oom_abort("stack scopes: nesting deeper than 1048575");
        return NULL;
    }
    iron_stack_chunk_ptr *more = atomic_load_explicit(&t->more, memory_order_acquire);
    if (!more) {
        if (!make) return NULL;
        more = (iron_stack_chunk_ptr *)calloc(IRON_STACK_CHUNKS, sizeof(*more));
        if (!more) iron_oom_abort("stack scopes");
        atomic_store_explicit(&t->more, more, memory_order_release);
    }
    uint32_t k = d / IRON_STACK_CHUNK;
    iron_atomic_u64 *c = atomic_load_explicit(&more[k], memory_order_acquire);
    if (!c) {
        if (!make) return NULL;
        c = (iron_atomic_u64 *)calloc(IRON_STACK_CHUNK, sizeof(iron_atomic_u64));
        if (!c) iron_oom_abort("stack scopes");
        atomic_store_explicit(&more[k], c, memory_order_release);
    }
    return &c[d % IRON_STACK_CHUNK];
}

/* The owner's own deep slots: the owner allocates chunks, other threads
 * only read them under the lock, and the chunks go away at thread exit
 * under the lock. */
void iron_stack_set_deep(uint32_t d, uint64_t key) {
    iron_atomic_u64 *slot = stack_deep_slot(&iron_stk, d, true);
    atomic_store_explicit(slot, key, memory_order_relaxed);
}

void iron_stack_thread_exit(void) {
    iron_gen_thread_exit();
    IronStackTLS *t = &iron_stk;
    if (!t->tno) return;
    stack_lock();
    s_stack_reg[t->tno].tls = NULL;
    s_stack_reg[t->tno].next_id = t->next_id;
    s_stack_free[s_stack_nfree++] = t->tno;
    if (t->renumbered) {
        /* The numbers this thread used up still point at its block. */
        for (uint32_t n = 1; n < IRON_STACK_THREADS; n++)
            if (s_stack_reg[n].tls == t) s_stack_reg[n].tls = NULL;
    }
    iron_stack_chunk_ptr *more = atomic_load_explicit(&t->more, memory_order_relaxed);
    if (more) {
        for (uint32_t k = 0; k < IRON_STACK_CHUNKS; k++)
            free(atomic_load_explicit(&more[k], memory_order_relaxed));
        free(more);
        atomic_store_explicit(&t->more, NULL, memory_order_relaxed);
    }
    stack_unlock();
    t->tno = 0;
    t->renumbered = false;
    atomic_store_explicit(&t->depth, 0, memory_order_relaxed);
    t->gen = IRON_STACK_GEN_INIT;
}

/* Recorded key at depth d of t, 0 when that depth is not open. Under the
 * registry lock. */
static uint64_t stack_key_at(IronStackTLS *t, uint32_t d) {
    if (d < IRON_STACK_CHUNK) return atomic_load_explicit(&t->ids[d], memory_order_relaxed);
    iron_atomic_u64 *slot = stack_deep_slot(t, d, false);
    return slot ? atomic_load_explicit(slot, memory_order_relaxed) : 0;
}

/* A stack-form gen the fast path did not find open in this thread's first
 * 1024 depths: a scope that ended, one of another thread, or one 1024 or
 * more scopes deep. Look it up in its owner's block under the registry
 * lock. A thread that ended took its scopes with it: its pointers are
 * dangling. */
bool iron_stack_live_slow(uint64_t gen) {
    uint32_t n = IRON_STACK_THREAD(gen);
    if (n == 0 || n >= IRON_STACK_THREADS) return false;
    uint64_t key = gen & IRON_STACK_KEY_MASK;
    uint32_t d = IRON_STACK_DEPTH(gen);
    bool live = false;
    stack_lock();
    IronStackTLS *t = s_stack_reg[n].tls;
    if (t) {
        uint32_t cur = atomic_load_explicit(&t->depth, memory_order_relaxed);
        if (d < 0xFFFF) {
            live = d <= cur && stack_key_at(t, d) == key;
        } else if (cur >= 0xFFFF && !t->renumbered) {
            /* Saturated depth: keys increase with depth along the open
             * scopes of one thread number, so binary search 0xFFFF..cur. */
            uint32_t lo = 0xFFFF, hi = cur;
            while (lo <= hi) {
                uint32_t mid = lo + (hi - lo) / 2;
                uint64_t v = stack_key_at(t, mid);
                if (v == key) { live = true; break; }
                if (v == 0 || v > key) { if (mid == lo) break; hi = mid - 1; } else lo = mid + 1;
            }
        } else if (cur >= 0xFFFF) {
            /* The thread changed number while these scopes were open: the
             * keys are no longer ordered. */
            for (uint32_t k = 0xFFFF; k <= cur && !live; k++) live = stack_key_at(t, k) == key;
        }
    }
    stack_unlock();
    return live;
}

/* The out-of-line rest of iron_check_ptr (iron_runtime.h). */
#undef iron_panic_stale_gen
#undef iron_panic_stale_stack_pointer
void iron_check_ptr_slow(uint64_t gen, const char *deref_file, int deref_line) {
    if (gen & IRON_STACK_TAG) {
        if (iron_stack_gen_live(gen)) return;
        iron_panic_stale_stack_pointer(deref_file, deref_line, gen);
    }
    if (iron_gen_live(gen)) return;
    iron_panic_stale_gen(deref_file, deref_line, gen);
}

/* iron_panic_stale_pointer body lives in src/runtime/iron_panic.c (Plan
 * 19-02). It is forward-declared in src/runtime/iron_runtime.h with
 * __attribute__((noreturn)); the canonical declaration is in
 * src/diagnostics/diagnostics.h next to iron_oom_abort. The link-time
 * resolve closes the call site in iron_heap_free below and the static-
 * inline iron_check_pointer_gen in iron_runtime.h. */

/* Compile-time lock on lock-free 64-bit atomics — fail fast on
 * misconfigured 32-bit ARM builds that would otherwise emit __atomic_*
 * library calls and require -latomic. */
#ifndef _WIN32
  _Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
                 "iron_runtime requires lock-free 64-bit atomics. "
                 "On 32-bit ARM, link with -latomic; see Pitfall 8 in "
                 ".planning/phases/19-generational-pointer-infrastructure/19-RESEARCH.md");
#endif

/* ── Phase 31 GA1 (Plan 31-01) — debug-allocator state (DBG-03 registry) ────
 *
 * Intrusive doubly-linked registry threaded through the 64B debug header.
 * O(1) link on alloc / unlink on free, serialized by a single mutex. The
 * atexit dump (iron_leak_dump) walks the list and reports still-live
 * allocations to STDERR with their alloc-site provenance.
 *
 * Scope note (Pitfall 3, intended false-negative): only iron_heap_alloc'd
 * blocks are registered. rc (iron_rc.c) and arena (iron_arena_rt.c)
 * allocate their own blocks WITHOUT routing through iron_heap_alloc, so they
 * are intentionally absent from this registry — documented, not a bug. The
 * dump header line states this. */
#ifdef IRON_DEBUG_ALLOCATOR
static IronAllocHdr *s_reg_head = NULL;
static iron_mutex_t  s_reg_lock;
static bool          s_reg_inited = false;

/* Idempotent registry-lock initializer. Called from iron_runtime_init under
 * IRON_DEBUG_ALLOCATOR (src/runtime/iron_string.c). Safe across repeated
 * init in a unit-test harness — the once-flag guards re-init. */
void iron_debug_alloc_init(void) {
    if (!s_reg_inited) {
        IRON_MUTEX_INIT(s_reg_lock);
        s_reg_inited = true;
    }
}

/* DBG-03: link hdr at the registry head (under lock). */
static void iron_debug_registry_link(IronAllocHdr *hdr) {
    if (!s_reg_inited) iron_debug_alloc_init();
    IRON_MUTEX_LOCK(s_reg_lock);
    hdr->reg_next = s_reg_head;
    hdr->reg_prev = NULL;
    if (s_reg_head) s_reg_head->reg_prev = hdr;
    s_reg_head = hdr;
    IRON_MUTEX_UNLOCK(s_reg_lock);
}

/* DBG-03: unlink hdr from the registry (under lock). */
static void iron_debug_registry_unlink(IronAllocHdr *hdr) {
    if (!s_reg_inited) return;
    IRON_MUTEX_LOCK(s_reg_lock);
    if (hdr->reg_prev) {
        hdr->reg_prev->reg_next = hdr->reg_next;
    } else if (s_reg_head == hdr) {
        s_reg_head = hdr->reg_next;
    }
    if (hdr->reg_next) hdr->reg_next->reg_prev = hdr->reg_prev;
    hdr->reg_next = NULL;
    hdr->reg_prev = NULL;
    IRON_MUTEX_UNLOCK(s_reg_lock);
}

/* ── Phase 31 DBG-01/04 — freed-block quarantine ───────────────────────────
 *
 * A debug free does not hand the block straight back to libc. Both guarantees
 * the debug allocator makes about a freed block need the block to still be ours
 * to read: DBG-01 says a use-after-free read hits 0xDD, and DBG-04 says a
 * double-free can still name the FIRST free-site out of the header. Neither
 * survives free() — the host allocator owns those bytes and writes its own
 * freelist bookkeeping into them. On macOS the payload reads back 0x00 and the
 * recorded free-site is gone, so both guarantees were false; on glibc they held
 * by luck of the size class, which is why only macOS noticed.
 *
 * Freed blocks go on a bounded FIFO instead — poisoned, unlinked from the
 * registry (so they are not leaks) and intact. Once the cap is reached the
 * oldest block is genuinely released, so retention is bounded and never grows.
 * A block that ages out loses both guarantees again, exactly as before: the
 * quarantine makes them true for recent frees, which is where use-after-free and
 * double-free bugs actually land. Debug builds only — the release path below is
 * untouched and still frees immediately. */
#define IRON_DBG_QUARANTINE_MAX 256

static IronAllocHdr *s_quarantine[IRON_DBG_QUARANTINE_MAX];
static int           s_quar_next  = 0;  /* next ring slot to fill */

/* Retain hdr, releasing the block it evicts. Shares s_reg_lock: both are
 * short, uncontended, and this keeps the debug allocator to one lock. */
static void iron_debug_quarantine_push(IronAllocHdr *hdr) {
    if (!s_reg_inited) iron_debug_alloc_init();
    IRON_MUTEX_LOCK(s_reg_lock);
    IronAllocHdr *evicted = s_quarantine[s_quar_next];  /* NULL until it wraps */
    s_quarantine[s_quar_next] = hdr;
    s_quar_next = (s_quar_next + 1) % IRON_DBG_QUARANTINE_MAX;
    IRON_MUTEX_UNLOCK(s_reg_lock);
    free(evicted);  /* free(NULL) is a no-op */
}

/* Whether hdr is still held by the quarantine (so its header is ours). */
static bool iron_debug_quarantine_holds(const IronAllocHdr *hdr) {
    if (!s_reg_inited) return false;
    bool held = false;
    IRON_MUTEX_LOCK(s_reg_lock);
    for (int i = 0; i < IRON_DBG_QUARANTINE_MAX; i++)
        if (s_quarantine[i] == hdr) { held = true; break; }
    IRON_MUTEX_UNLOCK(s_reg_lock);
    return held;
}

/* Release everything still quarantined. Called from iron_runtime_shutdown so a
 * leak checker does not see the retained blocks as still-reachable garbage. */
void iron_debug_quarantine_drain(void) {
    if (!s_reg_inited) return;
    IRON_MUTEX_LOCK(s_reg_lock);
    for (int i = 0; i < IRON_DBG_QUARANTINE_MAX; i++) {
        free(s_quarantine[i]);
        s_quarantine[i] = NULL;
    }
    s_quar_next = 0;
    IRON_MUTEX_UNLOCK(s_reg_lock);
}

/* DBG-03: atexit leak dump. Walks the registry under lock and reports each
 * still-live allocation to STDERR (NOT stdout — Pitfall 7: stdout must be
 * byte-identical debug vs release; only stderr diagnostics differ). Names
 * the alloc-site file:line + size + alloc_id. If the registry is empty,
 * prints NOTHING (clean exit). Tolerates an uninitialized lock. */
void iron_leak_dump(void) {
    if (!s_reg_inited) return;
    IRON_MUTEX_LOCK(s_reg_lock);
    if (s_reg_head == NULL) {
        IRON_MUTEX_UNLOCK(s_reg_lock);
        return;  /* clean exit — no leaks */
    }
    /* Count first so the header line can report the total. */
    uint64_t count = 0;
    for (IronAllocHdr *h = s_reg_head; h; h = h->reg_next) count++;
    /* The program's own output comes first: stdout is still buffered
     * when atexit handlers run. */
    fflush(stdout);
    fprintf(stderr,
            "iron: %llu heap allocation(s) leaked at exit "
            "(heap allocations only; rc/arena not tracked)\n",
            (unsigned long long)count);
    for (IronAllocHdr *h = s_reg_head; h; h = h->reg_next) {
        const char *af = h->alloc_site_file ? h->alloc_site_file : "<unknown>";
        fprintf(stderr, "  leaked: %s:%u  (id=%u size=%llu)\n",
                af, (unsigned)h->alloc_site_line,
                (unsigned)h->alloc_id,
                (unsigned long long)h->size);
    }
    IRON_MUTEX_UNLOCK(s_reg_lock);
    fflush(stderr);
}
#endif /* IRON_DEBUG_ALLOCATOR */

/* A double free used to be caught by a ring of recently freed addresses
 * (#260) and otherwise by reading the freed block's header. The generation
 * table answers it without touching the block: iron_gen_end fails for a
 * generation that already ended. */

Iron_FatPtr iron_heap_alloc(const char *site_file, int site_line, size_t size) {
    /* Allocate header + payload contiguously. */
    void *block = malloc(sizeof(IronAllocHdr) + size);
    if (!block) {
        iron_oom_abort("iron_heap_alloc");
    }
    IronAllocHdr *hdr = (IronAllocHdr *)block;

    IRON_ATOMIC_U64_INIT(hdr->gen, iron_gen_new(false));
    hdr->size = (uint64_t)size;

#ifdef IRON_DEBUG_ALLOCATOR
    hdr->alloc_site_file = site_file;  /* string-literal pointer; no strdup */
    hdr->alloc_site_line = (uint32_t)site_line;
    hdr->alloc_id = (uint32_t)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(iron_alloc_id_counter, 1);
    /* DBG-04: free-site starts NULL (no free yet). */
    hdr->free_site_file = NULL;
    hdr->free_site_line = 0;
    hdr->_pad = 0;
    /* DBG-03: link into the leak registry (O(1) head insert under lock). */
    iron_debug_registry_link(hdr);
#endif

    void *user = (void *)((uint8_t *)block + sizeof(IronAllocHdr));

#ifndef IRON_DEBUG_ALLOCATOR
    /* Phase 31 DBG-07 (Plan 31-03): release-build opt-in leak check. The 16B
     * release header has no registry slots, so a side-table keyed by the user
     * pointer tracks live allocations when IRON_LEAK_CHECK=1. This call
     * early-returns at zero cost when the env flag is unset (the common case);
     * debug builds use the in-header registry instead (no double-tracking). */
    iron_leakcheck_register(user, site_file, site_line, (uint64_t)size);
#endif

    return (Iron_FatPtr){user, IRON_ATOMIC_U64_LOAD_ACQUIRE(hdr->gen)};
}

#ifdef IRON_DEBUG_ALLOCATOR

/* Phase 31 DBG-01/03/04: debug-gated free with explicit free-site.
 *
 * Codegen (src/lir/emit_c.c) emits iron_heap_free_dbg(fp, __FILE__, __LINE__)
 * under IRON_DEBUG_ALLOCATOR so a double-free can report the SECOND/current
 * free-site too. Layered on the Phase 19 gen-mismatch path:
 *   - gen mismatch  → header still holds the FIRST free-site → both-sites
 *                     double-free panic (DBG-04)
 *   - gen match     → record this free-site, unlink from the registry,
 *                     poison the payload 0xDD (DBG-01), bump gen, free. */
void iron_heap_free_dbg(Iron_FatPtr fp, const char *free_file, int free_line) {
    if (!fp.addr) return;
    IronAllocHdr *hdr = ((IronAllocHdr *)fp.addr) - 1;

    if (!iron_gen_end(fp.gen)) {
        /* DBG-04: a second free. The header (and the first free-site it
         * holds) is only read while the block is still quarantined, i.e.
         * still ours; otherwise the first site is unknown. */
        bool held = iron_debug_quarantine_holds(hdr);
        iron_panic_double_free(held ? hdr->free_site_file : NULL,
                               held ? (int)hdr->free_site_line : 0,
                               free_file, free_line, held ? hdr : NULL);
        /* noreturn */
    }

    /* DBG-04: record THIS free-site so a later double free can name it. */
    hdr->free_site_file = free_file;
    hdr->free_site_line = (uint32_t)free_line;

    /* DBG-03: unlink from the registry before poisoning/freeing. */
    iron_debug_registry_unlink(hdr);

    /* DBG-01: poison the user payload so any UAF read hits 0xDD garbage. */
    memset(fp.addr, 0xDD, (size_t)hdr->size);
    IRON_ATOMIC_U64_STORE_RELEASE(hdr->gen, 0);
    /* Retain rather than free: the poison above and the free-site recorded in
     * the header are only readable while the block is still ours. */
    iron_debug_quarantine_push(hdr);
}

/* Debug build: iron_heap_free forwards to the dbg path with no explicit
 * free-site (NULL/0). Codegen normally calls iron_heap_free_dbg directly so
 * this NULL-site fallback is only hit by hand-written runtime callers. */
void iron_heap_free(Iron_FatPtr fp) {
    iron_heap_free_dbg(fp, NULL, 0);
}

#else /* !IRON_DEBUG_ALLOCATOR — release path (unchanged from Phase 19) */

void iron_heap_free(Iron_FatPtr fp) {
    if (!fp.addr) return;
    IronAllocHdr *hdr = ((IronAllocHdr *)fp.addr) - 1;

    /* Double-free / stale-fp validation through the generation table: the
     * block itself is not read unless its generation is still live (#413). */
    if (!iron_gen_end(fp.gen)) {
        iron_panic_stale_pointer("<iron_heap_free>", 0, NULL);
    }

    /* Phase 31 DBG-07 (Plan 31-03): release-build opt-in leak check — unlink
     * this allocation from the side-table (keyed by the user pointer fp.addr,
     * still valid until free(hdr) below). Early-returns at zero cost when
     * IRON_LEAK_CHECK is unset. No poison in release. */
    iron_leakcheck_unregister(fp.addr);
    free(hdr);  /* free entire block (header + payload) */
}

/* Release-build iron_heap_free_dbg: thin wrapper. Codegen always emits
 * iron_heap_free_dbg(fp, __FILE__, __LINE__) so the generated C call site is
 * identical across build modes; in release the free-site is ignored and the
 * plain generation-checked free runs (no registry, no poison, generic
 * stale-pointer panic on double-free). */
void iron_heap_free_dbg(Iron_FatPtr fp, const char *free_file, int free_line) {
    (void)free_file;
    (void)free_line;
    iron_heap_free(fp);
}

#endif /* IRON_DEBUG_ALLOCATOR */
