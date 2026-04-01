#ifndef IRON_RUNTIME_H
#define IRON_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Platform atomic abstraction ────────────────────────────────────────── */
#ifdef _WIN32
  #include <windows.h>
  typedef volatile LONG iron_atomic_int;
  #define IRON_ATOMIC_INIT(v, val)          ((v) = (val))
  #define IRON_ATOMIC_LOAD(v)               InterlockedCompareExchange(&(v), 0, 0)
  #define IRON_ATOMIC_FETCH_ADD(v, n)       InterlockedExchangeAdd(&(v), (n))
  #define IRON_ATOMIC_FETCH_SUB(v, n)       InterlockedExchangeAdd(&(v), -(n))
  #define IRON_ATOMIC_CAS_WEAK(v, exp, des) iron__win_cas(&(v), (exp), (des))
  static inline bool iron__win_cas(volatile LONG *v, int *expected, int desired) {
      LONG old = InterlockedCompareExchange(v, desired, *expected);
      if (old == *expected) return true;
      *expected = (int)old;
      return false;
  }
#else
  #include <stdatomic.h>
  typedef atomic_int iron_atomic_int;
  #define IRON_ATOMIC_INIT(v, val)          atomic_init(&(v), (val))
  #define IRON_ATOMIC_LOAD(v)               atomic_load(&(v))
  #define IRON_ATOMIC_FETCH_ADD(v, n)       atomic_fetch_add(&(v), (n))
  #define IRON_ATOMIC_FETCH_SUB(v, n)       atomic_fetch_sub(&(v), (n))
  #define IRON_ATOMIC_CAS_WEAK(v, exp, des) atomic_compare_exchange_weak(&(v), (exp), (des))
#endif

/* ── Platform threading abstraction ──────────────────────────────────────── */
#ifdef _WIN32

  typedef HANDLE               iron_thread_t;
  typedef CRITICAL_SECTION     iron_mutex_t;
  typedef CONDITION_VARIABLE   iron_cond_t;

  /* Thread wrapper: Win32 thread proc signature differs from pthreads */
  typedef struct { void *(*fn)(void*); void *arg; } iron__win_trampoline_t;
  static DWORD WINAPI iron__win_thread_proc(void *p) {
      iron__win_trampoline_t *t = (iron__win_trampoline_t *)p;
      t->fn(t->arg);
      free(p);
      return 0;
  }
  static inline int iron__win_thread_create(iron_thread_t *t,
                                            void *(*fn)(void*), void *arg) {
      iron__win_trampoline_t *tramp = (iron__win_trampoline_t *)malloc(sizeof(*tramp));
      if (!tramp) return -1;
      tramp->fn = fn; tramp->arg = arg;
      *t = CreateThread(NULL, 0, iron__win_thread_proc, tramp, 0, NULL);
      return *t ? 0 : -1;
  }

  #define IRON_THREAD_CREATE(t,fn,arg)   iron__win_thread_create(&(t),(fn),(arg))
  #define IRON_THREAD_JOIN(t)            (WaitForSingleObject((t), INFINITE), CloseHandle((t)))
  #define IRON_MUTEX_INIT(m)             InitializeCriticalSection(&(m))
  #define IRON_MUTEX_LOCK(m)             EnterCriticalSection(&(m))
  #define IRON_MUTEX_UNLOCK(m)           LeaveCriticalSection(&(m))
  #define IRON_MUTEX_DESTROY(m)          DeleteCriticalSection(&(m))
  #define IRON_COND_INIT(c)              InitializeConditionVariable(&(c))
  #define IRON_COND_WAIT(c,m)            SleepConditionVariableCS(&(c), &(m), INFINITE)
  #define IRON_COND_SIGNAL(c)            WakeConditionVariable(&(c))
  #define IRON_COND_BROADCAST(c)         WakeAllConditionVariable(&(c))
  #define IRON_COND_DESTROY(c)           ((void)(c))  /* Win32 CV needs no destroy */
#else
  #include <pthread.h>
  typedef pthread_t          iron_thread_t;
  typedef pthread_mutex_t    iron_mutex_t;
  typedef pthread_cond_t     iron_cond_t;

  #define IRON_THREAD_CREATE(t,fn,arg)   pthread_create(&(t),NULL,(fn),(arg))
  #define IRON_THREAD_JOIN(t)            pthread_join((t), NULL)
  #define IRON_MUTEX_INIT(m)             pthread_mutex_init(&(m), NULL)
  #define IRON_MUTEX_LOCK(m)             pthread_mutex_lock(&(m))
  #define IRON_MUTEX_UNLOCK(m)           pthread_mutex_unlock(&(m))
  #define IRON_MUTEX_DESTROY(m)          pthread_mutex_destroy(&(m))
  #define IRON_COND_INIT(c)              pthread_cond_init(&(c), NULL)
  #define IRON_COND_WAIT(c,m)            pthread_cond_wait(&(c), &(m))
  #define IRON_COND_SIGNAL(c)            pthread_cond_signal(&(c))
  #define IRON_COND_BROADCAST(c)         pthread_cond_broadcast(&(c))
  #define IRON_COND_DESTROY(c)           pthread_cond_destroy(&(c))
#endif

/* ── Iron_String ────────────────────────────────────────────────────────────
 * 24-byte string type with Small String Optimisation (SSO).
 * Strings <= IRON_STRING_SSO_MAX bytes are stored inline without heap
 * allocation. Longer strings are heap-allocated. The intern table
 * deduplicates identical string content for literal strings.
 */
#define IRON_STRING_SSO_MAX 23

typedef struct {
    union {
        /* Heap variant (is_heap flag set) */
        struct {
            char    *data;
            uint32_t byte_length;
            uint32_t codepoint_count;
            uint8_t  _padding[7];
            uint8_t  flags; /* bit 0 = is_heap, bit 1 = is_interned */
        } heap;

        /* SSO variant (is_heap flag clear).
         * data[0..len-1] holds the string bytes; data[len] is always '\0'.
         * data has SSO_MAX+1 slots so a 23-byte string fits with terminator.
         * The union is padded to 25 bytes by the compiler to accommodate this.
         */
        struct {
            char    data[IRON_STRING_SSO_MAX + 1]; /* +1 for null terminator */
            uint8_t len; /* byte length (0..IRON_STRING_SSO_MAX) */
        } sso;
    };
} Iron_String;

/* Iron_String API */
Iron_String  iron_string_from_cstr(const char *cstr, size_t byte_len);
Iron_String  iron_string_from_literal(const char *lit, size_t byte_len);
const char  *iron_string_cstr(const Iron_String *s);
size_t       iron_string_byte_len(const Iron_String *s);
size_t       iron_string_codepoint_count(const Iron_String *s);
bool         iron_string_equals(const Iron_String *a, const Iron_String *b);
Iron_String  iron_string_concat(const Iron_String *a, const Iron_String *b);
Iron_String  iron_string_intern(Iron_String s);

/* ── Iron_Rc / Iron_Weak ─────────────────────────────────────────────────────
 * Atomic reference-counted heap value.  Iron_Weak holds a non-owning
 * reference that returns NULL after the last strong ref is dropped.
 */
typedef struct {
    iron_atomic_int  strong_count;
    iron_atomic_int  weak_count;
    void      (*destructor)(void *value);
} Iron_RcControl;

typedef struct {
    Iron_RcControl *ctrl;
    void           *value;
} Iron_Rc;

typedef struct {
    Iron_RcControl *ctrl;
} Iron_Weak;

Iron_Rc  iron_rc_create(void *value, size_t size, void (*destructor)(void *));
void     iron_rc_retain(Iron_Rc *rc);
void     iron_rc_release(Iron_Rc *rc);
Iron_Weak iron_rc_downgrade(const Iron_Rc *rc);
Iron_Rc  iron_weak_upgrade(const Iron_Weak *weak);

/* ── Iron_Error ──────────────────────────────────────────────────────────────
 * Lightweight error type (no heap allocation).
 */
typedef struct {
    int         code;
    const char *message;
} Iron_Error;

static inline Iron_Error iron_error_none(void)              { return (Iron_Error){0, NULL}; }
static inline Iron_Error iron_error_new(int c, const char *m) { return (Iron_Error){c, m}; }
static inline bool       iron_error_is_ok(Iron_Error e)     { return e.code == 0; }

/* ── Built-in function declarations ──────────────────────────────────────────
 * These are called by code generated by the Iron compiler.
 */
void    Iron_print(Iron_String s);
void    Iron_println(Iron_String s);
int64_t Iron_len(Iron_String s);
int64_t Iron_min(int64_t a, int64_t b);
int64_t Iron_max(int64_t a, int64_t b);
int64_t Iron_clamp(int64_t val, int64_t lo, int64_t hi);
int64_t Iron_abs(int64_t val);
void    Iron_assert(bool cond, Iron_String msg);
static inline int64_t Iron_range(int64_t n) { return n; }

/* ── Iron_Pool (fixed-size thread pool) ──────────────────────────────────────
 * Iron_Pool manages a set of worker threads and a FIFO work queue.
 * Iron_pool_barrier() blocks until all submitted work completes.
 * The global pool is initialized in iron_runtime_init().
 */
typedef struct Iron_Pool Iron_Pool;

/* Global pool — initialized in iron_runtime_init() */
extern Iron_Pool *Iron_global_pool;

/* Pool API */
Iron_Pool *Iron_pool_create(const char *name, int thread_count);
void       Iron_pool_destroy(Iron_Pool *pool);
void       Iron_pool_submit(Iron_Pool *pool, void (*fn)(void *), void *arg);
void       Iron_pool_barrier(Iron_Pool *pool);
int        Iron_pool_thread_count(const Iron_Pool *pool);

/* ── Iron_Handle (future for spawn/await) ────────────────────────────────────
 * Created by spawn; awaited with Iron_handle_wait().
 * Panic in the spawned task is stored and re-raised on wait.
 */
typedef struct {
    iron_thread_t   thread;
    bool            done;
    void           *result;
    iron_mutex_t    lock;
    iron_cond_t     cond;
    char           *panic_msg;
} Iron_Handle;

/* Handle API */
Iron_Handle *Iron_handle_create(void (*fn)(void *), void *arg);
void         Iron_handle_wait(Iron_Handle *handle);
void         Iron_handle_destroy(Iron_Handle *handle);
void        *iron_future_await(Iron_Handle *handle);
Iron_Handle *iron_handle_create_self_ref(void (*fn)(void *));

/* ── Iron_Channel (bounded ring buffer) ──────────────────────────────────────
 * send blocks when the buffer is full; recv blocks when it is empty.
 * try_recv returns immediately with true/false.
 * capacity 0 or 1 is treated as unbuffered (capacity = 1).
 */
typedef struct Iron_Channel Iron_Channel;

/* Channel API */
Iron_Channel *Iron_channel_create(int capacity);
void          Iron_channel_send(Iron_Channel *ch, void *item);
void         *Iron_channel_recv(Iron_Channel *ch);
bool          Iron_channel_try_recv(Iron_Channel *ch, void **out);
void          Iron_channel_close(Iron_Channel *ch);
void          Iron_channel_destroy(Iron_Channel *ch);

/* ── Iron_Mutex (value-wrapping mutex) ───────────────────────────────────────
 * Wraps a value so that all access must go through lock/unlock.
 * Iron_mutex_lock() returns a pointer to the wrapped value.
 */
typedef struct {
    iron_mutex_t    lock;
    void           *value;
    size_t          value_size;
} Iron_Mutex;

/* Mutex API */
Iron_Mutex *Iron_mutex_create(void *initial_value, size_t size);
void       *Iron_mutex_lock(Iron_Mutex *m);   /* returns pointer to value */
void        Iron_mutex_unlock(Iron_Mutex *m);
void        Iron_mutex_destroy(Iron_Mutex *m);

/* ── Lock / CondVar raw primitives ───────────────────────────────────────────
 * Thin wrappers around pthread_mutex_t and pthread_cond_t for use in
 * Iron programs that need lower-level synchronisation.
 */
typedef iron_mutex_t    Iron_Lock;
typedef iron_cond_t     Iron_CondVar;

void Iron_lock_init(Iron_Lock *l);
void Iron_lock_acquire(Iron_Lock *l);
void Iron_lock_release(Iron_Lock *l);
void Iron_condvar_init(Iron_CondVar *cv);
void Iron_condvar_wait(Iron_CondVar *cv, Iron_Lock *l);
void Iron_condvar_signal(Iron_CondVar *cv);
void Iron_condvar_broadcast(Iron_CondVar *cv);

/* ── Runtime lifecycle ───────────────────────────────────────────────────────
 * iron_runtime_init() must be called before any Iron_String or Iron_Rc use.
 * It also creates Iron_global_pool with (cpu_count - 1) worker threads.
 * iron_runtime_shutdown() releases all runtime resources.
 */
void iron_runtime_init(void);
void iron_runtime_shutdown(void);

/* ── Collection macros ───────────────────────────────────────────────────────
 * Macro-generated List[T], Map[K,V], and Set[T] collection types.
 *
 * The Iron compiler's monomorphization pass (gen_types.c ensure_monomorphized_type)
 * emits stub struct typedefs with the naming convention Iron_<base>_<csuffix>.
 * These macros generate the matching function implementations.
 *
 * Naming example (from gen_types.c mangle_generic):
 *   List[Int]        -> Iron_List_int64_t   (struct + functions)
 *   Map[String, Int] -> Iron_Map_Iron_String_int64_t
 *   Set[Int]         -> Iron_Set_int64_t
 *
 * Usage:
 *   IRON_LIST_DECL(int64_t, int64_t)   -- declares function prototypes
 *   IRON_LIST_IMPL(int64_t, int64_t)   -- defines function bodies (in .c file)
 *
 * The codegen-emitted struct typedef must already be visible (the codegen
 * output includes it before calling any runtime function).  For the runtime's
 * own test/collection file, use the pre-instantiated common types defined
 * with IRON_CODEGEN_PROVIDES_STRUCTS unset (see iron_collections.c).
 */

/* ── List[T] macros ──────────────────────────────────────────────────────────
 * Expected struct layout (emitted by ensure_monomorphized_type):
 *   typedef struct Iron_List_##suffix {
 *       T       *items;
 *       int64_t  count;
 *       int64_t  capacity;
 *   } Iron_List_##suffix;
 */
#define IRON_LIST_DECL(T, suffix) \
    Iron_List_##suffix Iron_List_##suffix##_create(void); \
    Iron_List_##suffix Iron_List_##suffix##_create_with_capacity(int64_t cap); \
    Iron_List_##suffix Iron_List_##suffix##_clone(const Iron_List_##suffix *src); \
    void               Iron_List_##suffix##_push(Iron_List_##suffix *self, T item); \
    T                  Iron_List_##suffix##_get(const Iron_List_##suffix *self, int64_t index); \
    void               Iron_List_##suffix##_set(Iron_List_##suffix *self, int64_t index, T item); \
    T                  Iron_List_##suffix##_pop(Iron_List_##suffix *self); \
    int64_t            Iron_List_##suffix##_len(const Iron_List_##suffix *self); \
    void               Iron_List_##suffix##_free(Iron_List_##suffix *self);

#define IRON_LIST_IMPL(T, suffix) \
    Iron_List_##suffix Iron_List_##suffix##_create(void) { \
        Iron_List_##suffix l; \
        l.items = NULL; l.count = 0; l.capacity = 0; \
        return l; \
    } \
    Iron_List_##suffix Iron_List_##suffix##_create_with_capacity(int64_t cap) { \
        Iron_List_##suffix l; \
        l.count = 0; \
        l.capacity = cap; \
        l.items = cap > 0 ? (T *)malloc((size_t)cap * sizeof(T)) : NULL; \
        return l; \
    } \
    Iron_List_##suffix Iron_List_##suffix##_clone(const Iron_List_##suffix *src) { \
        Iron_List_##suffix dst; \
        dst.count = src->count; \
        dst.capacity = src->count; \
        if (src->count > 0) { \
            dst.items = (T *)malloc((size_t)src->count * sizeof(T)); \
            memcpy(dst.items, src->items, (size_t)src->count * sizeof(T)); \
        } else { \
            dst.items = NULL; \
        } \
        return dst; \
    } \
    void Iron_List_##suffix##_push(Iron_List_##suffix *self, T item) { \
        if (self->count >= self->capacity) { \
            self->capacity = self->capacity ? self->capacity * 2 : 8; \
            self->items = (T *)realloc(self->items, (size_t)self->capacity * sizeof(T)); \
        } \
        self->items[self->count++] = item; \
    } \
    T Iron_List_##suffix##_get(const Iron_List_##suffix *self, int64_t index) { \
        return self->items[index]; \
    } \
    void Iron_List_##suffix##_set(Iron_List_##suffix *self, int64_t index, T item) { \
        self->items[index] = item; \
    } \
    T Iron_List_##suffix##_pop(Iron_List_##suffix *self) { \
        return self->items[--self->count]; \
    } \
    int64_t Iron_List_##suffix##_len(const Iron_List_##suffix *self) { \
        return self->count; \
    } \
    void Iron_List_##suffix##_free(Iron_List_##suffix *self) { \
        free(self->items); \
        self->items = NULL; self->count = 0; self->capacity = 0; \
    }

/* ── Map[K,V] macros ─────────────────────────────────────────────────────────
 * Simple array-based map with linear-scan lookup (O(n), sufficient for v1).
 * eq_fn has signature: bool (*)(const K *a, const K *b)
 *
 * Expected struct layout:
 *   typedef struct Iron_Map_##ksuffix##_##vsuffix {
 *       K       *keys;
 *       V       *values;
 *       int64_t  count;
 *       int64_t  capacity;
 *   } Iron_Map_##ksuffix##_##vsuffix;
 */
#define IRON_MAP_DECL(K, V, ksuffix, vsuffix) \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_create(void); \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_create_with_capacity(int64_t cap); \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_clone(const Iron_Map_##ksuffix##_##vsuffix *src); \
    void  Iron_Map_##ksuffix##_##vsuffix##_put(Iron_Map_##ksuffix##_##vsuffix *self, K key, V value); \
    V     Iron_Map_##ksuffix##_##vsuffix##_get(const Iron_Map_##ksuffix##_##vsuffix *self, K key); \
    bool  Iron_Map_##ksuffix##_##vsuffix##_has(const Iron_Map_##ksuffix##_##vsuffix *self, K key); \
    void  Iron_Map_##ksuffix##_##vsuffix##_remove(Iron_Map_##ksuffix##_##vsuffix *self, K key); \
    int64_t Iron_Map_##ksuffix##_##vsuffix##_len(const Iron_Map_##ksuffix##_##vsuffix *self); \
    void  Iron_Map_##ksuffix##_##vsuffix##_free(Iron_Map_##ksuffix##_##vsuffix *self);

#define IRON_MAP_IMPL(K, V, ksuffix, vsuffix, eq_fn) \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_create(void) { \
        Iron_Map_##ksuffix##_##vsuffix m; \
        m.keys = NULL; m.values = NULL; m.count = 0; m.capacity = 0; \
        return m; \
    } \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_create_with_capacity(int64_t cap) { \
        Iron_Map_##ksuffix##_##vsuffix m; \
        m.count = 0; \
        m.capacity = cap; \
        m.keys   = cap > 0 ? (K *)malloc((size_t)cap * sizeof(K)) : NULL; \
        m.values = cap > 0 ? (V *)malloc((size_t)cap * sizeof(V)) : NULL; \
        return m; \
    } \
    Iron_Map_##ksuffix##_##vsuffix Iron_Map_##ksuffix##_##vsuffix##_clone(const Iron_Map_##ksuffix##_##vsuffix *src) { \
        Iron_Map_##ksuffix##_##vsuffix dst; \
        dst.count = src->count; \
        dst.capacity = src->count; \
        if (src->count > 0) { \
            dst.keys   = (K *)malloc((size_t)src->count * sizeof(K)); \
            dst.values = (V *)malloc((size_t)src->count * sizeof(V)); \
            memcpy(dst.keys,   src->keys,   (size_t)src->count * sizeof(K)); \
            memcpy(dst.values, src->values, (size_t)src->count * sizeof(V)); \
        } else { \
            dst.keys = NULL; \
            dst.values = NULL; \
        } \
        return dst; \
    } \
    void Iron_Map_##ksuffix##_##vsuffix##_put(Iron_Map_##ksuffix##_##vsuffix *self, K key, V value) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->keys[i], &key)) { self->values[i] = value; return; } \
        } \
        if (self->count >= self->capacity) { \
            self->capacity = self->capacity ? self->capacity * 2 : 8; \
            self->keys   = (K *)realloc(self->keys,   (size_t)self->capacity * sizeof(K)); \
            self->values = (V *)realloc(self->values, (size_t)self->capacity * sizeof(V)); \
        } \
        self->keys[self->count]   = key; \
        self->values[self->count] = value; \
        self->count++; \
    } \
    V Iron_Map_##ksuffix##_##vsuffix##_get(const Iron_Map_##ksuffix##_##vsuffix *self, K key) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->keys[i], &key)) { return self->values[i]; } \
        } \
        /* Caller must use has() first — undefined if key absent */ \
        V _zero; \
        memset(&_zero, 0, sizeof(V)); \
        return _zero; \
    } \
    bool Iron_Map_##ksuffix##_##vsuffix##_has(const Iron_Map_##ksuffix##_##vsuffix *self, K key) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->keys[i], &key)) { return true; } \
        } \
        return false; \
    } \
    void Iron_Map_##ksuffix##_##vsuffix##_remove(Iron_Map_##ksuffix##_##vsuffix *self, K key) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->keys[i], &key)) { \
                self->keys[i]   = self->keys[self->count - 1]; \
                self->values[i] = self->values[self->count - 1]; \
                self->count--; \
                return; \
            } \
        } \
    } \
    int64_t Iron_Map_##ksuffix##_##vsuffix##_len(const Iron_Map_##ksuffix##_##vsuffix *self) { \
        return self->count; \
    } \
    void Iron_Map_##ksuffix##_##vsuffix##_free(Iron_Map_##ksuffix##_##vsuffix *self) { \
        free(self->keys);   self->keys   = NULL; \
        free(self->values); self->values = NULL; \
        self->count = 0; self->capacity = 0; \
    }

/* ── Set[T] macros ───────────────────────────────────────────────────────────
 * Simple array-based set with linear-scan deduplication (O(n), sufficient v1).
 * eq_fn has signature: bool (*)(const T *a, const T *b)
 *
 * Expected struct layout:
 *   typedef struct Iron_Set_##suffix {
 *       T       *items;
 *       int64_t  count;
 *       int64_t  capacity;
 *   } Iron_Set_##suffix;
 */
#define IRON_SET_DECL(T, suffix) \
    Iron_Set_##suffix Iron_Set_##suffix##_create(void); \
    Iron_Set_##suffix Iron_Set_##suffix##_create_with_capacity(int64_t cap); \
    Iron_Set_##suffix Iron_Set_##suffix##_clone(const Iron_Set_##suffix *src); \
    void    Iron_Set_##suffix##_add(Iron_Set_##suffix *self, T item); \
    bool    Iron_Set_##suffix##_contains(const Iron_Set_##suffix *self, T item); \
    void    Iron_Set_##suffix##_remove(Iron_Set_##suffix *self, T item); \
    int64_t Iron_Set_##suffix##_len(const Iron_Set_##suffix *self); \
    void    Iron_Set_##suffix##_free(Iron_Set_##suffix *self);

#define IRON_SET_IMPL(T, suffix, eq_fn) \
    Iron_Set_##suffix Iron_Set_##suffix##_create(void) { \
        Iron_Set_##suffix s; \
        s.items = NULL; s.count = 0; s.capacity = 0; \
        return s; \
    } \
    Iron_Set_##suffix Iron_Set_##suffix##_create_with_capacity(int64_t cap) { \
        Iron_Set_##suffix s; \
        s.count = 0; \
        s.capacity = cap; \
        s.items = cap > 0 ? (T *)malloc((size_t)cap * sizeof(T)) : NULL; \
        return s; \
    } \
    Iron_Set_##suffix Iron_Set_##suffix##_clone(const Iron_Set_##suffix *src) { \
        Iron_Set_##suffix dst; \
        dst.count = src->count; \
        dst.capacity = src->count; \
        if (src->count > 0) { \
            dst.items = (T *)malloc((size_t)src->count * sizeof(T)); \
            memcpy(dst.items, src->items, (size_t)src->count * sizeof(T)); \
        } else { \
            dst.items = NULL; \
        } \
        return dst; \
    } \
    void Iron_Set_##suffix##_add(Iron_Set_##suffix *self, T item) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->items[i], &item)) { return; } \
        } \
        if (self->count >= self->capacity) { \
            self->capacity = self->capacity ? self->capacity * 2 : 8; \
            self->items = (T *)realloc(self->items, (size_t)self->capacity * sizeof(T)); \
        } \
        self->items[self->count++] = item; \
    } \
    bool Iron_Set_##suffix##_contains(const Iron_Set_##suffix *self, T item) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->items[i], &item)) { return true; } \
        } \
        return false; \
    } \
    void Iron_Set_##suffix##_remove(Iron_Set_##suffix *self, T item) { \
        for (int64_t i = 0; i < self->count; i++) { \
            if (eq_fn(&self->items[i], &item)) { \
                self->items[i] = self->items[self->count - 1]; \
                self->count--; \
                return; \
            } \
        } \
    } \
    int64_t Iron_Set_##suffix##_len(const Iron_Set_##suffix *self) { \
        return self->count; \
    } \
    void Iron_Set_##suffix##_free(Iron_Set_##suffix *self) { \
        free(self->items); \
        self->items = NULL; self->count = 0; self->capacity = 0; \
    }

/* ── Pre-instantiated common collection struct typedefs ──────────────────────
 * The codegen emits its own struct typedefs in the generated C output.
 * Here we define the most common types so that iron_collections.c and
 * test files compile without needing a full codegen pass.
 * C11 permits duplicate compatible typedefs, so codegen output can repeat them.
 */
#ifndef IRON_CODEGEN_PROVIDES_STRUCTS

typedef struct Iron_List_int64_t    { int64_t     *items; int64_t count; int64_t capacity; } Iron_List_int64_t;
typedef struct Iron_List_int32_t    { int32_t     *items; int64_t count; int64_t capacity; } Iron_List_int32_t;
typedef struct Iron_List_double     { double      *items; int64_t count; int64_t capacity; } Iron_List_double;
typedef struct Iron_List_bool       { bool        *items; int64_t count; int64_t capacity; } Iron_List_bool;
typedef struct Iron_List_Iron_String { Iron_String *items; int64_t count; int64_t capacity; } Iron_List_Iron_String;

typedef struct Iron_Map_Iron_String_int64_t    { Iron_String *keys; int64_t     *values; int64_t count; int64_t capacity; } Iron_Map_Iron_String_int64_t;
typedef struct Iron_Map_Iron_String_Iron_String { Iron_String *keys; Iron_String *values; int64_t count; int64_t capacity; } Iron_Map_Iron_String_Iron_String;

typedef struct Iron_Set_int64_t    { int64_t     *items; int64_t count; int64_t capacity; } Iron_Set_int64_t;
typedef struct Iron_Set_Iron_String { Iron_String *items; int64_t count; int64_t capacity; } Iron_Set_Iron_String;

#endif /* IRON_CODEGEN_PROVIDES_STRUCTS */

/* Declarations for the pre-instantiated types in iron_collections.c */
IRON_LIST_DECL(int64_t,     int64_t)
IRON_LIST_DECL(int32_t,     int32_t)
IRON_LIST_DECL(double,      double)
IRON_LIST_DECL(bool,        bool)
IRON_LIST_DECL(Iron_String, Iron_String)

IRON_MAP_DECL(Iron_String, int64_t,     Iron_String, int64_t)
IRON_MAP_DECL(Iron_String, Iron_String, Iron_String, Iron_String)

IRON_SET_DECL(int64_t,     int64_t)
IRON_SET_DECL(Iron_String, Iron_String)

#endif /* IRON_RUNTIME_H */
