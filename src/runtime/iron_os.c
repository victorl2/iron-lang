/* iron_os.c: the platform behind the freestanding runtime header.
 *
 * iron_runtime.h declares memory, output, thread and lock primitives as
 * plain functions so that generated C and the header's inline helpers
 * never include a libc or OS header (#235). This file is the one place
 * that does: it forwards the memory calls to libc and implements the
 * threading handles over pthreads or Win32 inside the opaque storage the
 * header declares, with static assertions that the storage fits. */
#include "runtime/iron_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
  #include <io.h>
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <pthread.h>
  #include <sched.h>
  #include <unistd.h>
#endif

/* ── Memory ─────────────────────────────────────────────────────────────── */

void  *iron_mem_alloc(size_t size)                        { return malloc(size); }
void  *iron_mem_calloc(size_t count, size_t size)         { return calloc(count, size); }
void  *iron_mem_realloc(void *ptr, size_t size)           { return realloc(ptr, size); }
void   iron_mem_free(void *ptr)                           { free(ptr); }
void  *iron_mem_copy(void *dst, const void *src, size_t n) { return memcpy(dst, src, n); }
void  *iron_mem_move(void *dst, const void *src, size_t n) { return memmove(dst, src, n); }
void  *iron_mem_set(void *dst, int byte, size_t n)        { return memset(dst, byte, n); }
int    iron_mem_cmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
size_t iron_cstr_len(const char *s)                       { return strlen(s); }
int    iron_cstr_cmp(const char *a, const char *b)        { return strcmp(a, b); }
void   iron_sort(void *base, size_t count, size_t size,
                 int (*cmp)(const void *, const void *))  { qsort(base, count, size, cmp); }

/* ── Standard streams and process exit ──────────────────────────────────── */

void iron_out_write(const char *bytes, size_t n) { fwrite(bytes, 1, n, stdout); }
void iron_err_write(const char *bytes, size_t n) { fwrite(bytes, 1, n, stderr); }
void iron_out_flush(void)                        { fflush(stdout); }
void iron_exit(int code)                         { exit(code); }
void iron_abort(void)                            { fflush(stdout); fflush(stderr); abort(); }
void iron_panic_null_box(void) {
    fflush(stdout);
    fputs("iron: panic: unwrap() on null Box\n", stderr);
    fflush(stderr);
    abort();
}

/* ── FileHandle (write mode) ─────────────────────────────────────────────── */

int iron_filehandle_open(const char *path) {
    FILE *f = fopen(path ? path : "", "w");
    if (!f) return -1;
#ifdef _WIN32
    return _fileno(f);
#else
    return fileno(f);
#endif
}
void iron_filehandle_close(int fd) {
    printf("closed fd\n");   /* the FileHandle fixtures observe the close */
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

/* ── Threads and locks ──────────────────────────────────────────────────── */

void iron_once(iron_once_t *once, void (*fn)(void)) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&once->state, &expected, 1)) {
        fn();
        atomic_store_explicit(&once->state, 2, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&once->state, memory_order_acquire) != 2) {
#ifdef _WIN32
        Sleep(0);
#else
        sched_yield();
#endif
    }
}

#ifdef _WIN32

_Static_assert(sizeof(CRITICAL_SECTION) <= sizeof(((iron_mutex_t *)0)->opaque), "iron_mutex_t storage");
_Static_assert(sizeof(CONDITION_VARIABLE) <= sizeof(((iron_cond_t *)0)->opaque), "iron_cond_t storage");
_Static_assert(sizeof(SRWLOCK) <= sizeof(((iron_rwlock_t *)0)->opaque), "iron_rwlock_t storage");
_Static_assert(sizeof(HANDLE) <= sizeof(uintptr_t), "iron_thread_t storage");

#define MUTEX(m)  ((CRITICAL_SECTION *)(m)->opaque)
#define COND(c)   ((CONDITION_VARIABLE *)(c)->opaque)
#define RWLOCK(l) ((SRWLOCK *)(l)->opaque)

typedef struct { void *(*fn)(void *); void *arg; } iron_thread_start;
static DWORD WINAPI iron_thread_proc(void *p) {
    iron_thread_start *t = (iron_thread_start *)p;
    void *(*fn)(void *) = t->fn;
    void *arg = t->arg;
    free(t);
    fn(arg);
    return 0;
}
int iron_thread_create(iron_thread_t *t, void *(*fn)(void *), void *arg) {
    iron_thread_start *start = (iron_thread_start *)malloc(sizeof(*start));
    if (!start) return -1;
    start->fn = fn;
    start->arg = arg;
    HANDLE h = CreateThread(NULL, IRON_THREAD_STACK_SIZE, iron_thread_proc, start,
                            STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!h) { free(start); return -1; }
    t->handle = (uintptr_t)h;
    return 0;
}
int iron_thread_join(iron_thread_t t) {
    HANDLE h = (HANDLE)t.handle;
    DWORD rc = WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return rc == WAIT_OBJECT_0 ? 0 : -1;
}
int iron_thread_detach(iron_thread_t t) { return CloseHandle((HANDLE)t.handle) ? 0 : -1; }
iron_thread_t iron_thread_self(void) { iron_thread_t t; t.handle = (uintptr_t)GetCurrentThreadId(); return t; }
bool iron_thread_equal(iron_thread_t a, iron_thread_t b) { return a.handle == b.handle; }

void iron_mutex_init(iron_mutex_t *m)    { InitializeCriticalSection(MUTEX(m)); }
void iron_mutex_lock(iron_mutex_t *m)    { EnterCriticalSection(MUTEX(m)); }
void iron_mutex_unlock(iron_mutex_t *m)  { LeaveCriticalSection(MUTEX(m)); }
void iron_mutex_destroy(iron_mutex_t *m) { DeleteCriticalSection(MUTEX(m)); }
void iron_cond_init(iron_cond_t *c)      { InitializeConditionVariable(COND(c)); }
void iron_cond_wait(iron_cond_t *c, iron_mutex_t *m) { SleepConditionVariableCS(COND(c), MUTEX(m), INFINITE); }
void iron_cond_signal(iron_cond_t *c)    { WakeConditionVariable(COND(c)); }
void iron_cond_broadcast(iron_cond_t *c) { WakeAllConditionVariable(COND(c)); }
void iron_cond_destroy(iron_cond_t *c)   { (void)c; }
void iron_rwlock_init(iron_rwlock_t *l)     { InitializeSRWLock(RWLOCK(l)); }
void iron_rwlock_rdlock(iron_rwlock_t *l)   { AcquireSRWLockShared(RWLOCK(l)); }
void iron_rwlock_wrlock(iron_rwlock_t *l)   { AcquireSRWLockExclusive(RWLOCK(l)); }
void iron_rwlock_rdunlock(iron_rwlock_t *l) { ReleaseSRWLockShared(RWLOCK(l)); }
void iron_rwlock_wrunlock(iron_rwlock_t *l) { ReleaseSRWLockExclusive(RWLOCK(l)); }
void iron_rwlock_destroy(iron_rwlock_t *l)  { (void)l; }

int iron_cond_timedwait_ms(iron_cond_t *cv, iron_mutex_t *lock, int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    BOOL ok = SleepConditionVariableCS(COND(cv), MUTEX(lock), (DWORD)timeout_ms);
    if (ok) return IRON_TIMEDWAIT_OK;
    if (GetLastError() == ERROR_TIMEOUT) return IRON_TIMEDWAIT_EXPIRED;
    return IRON_TIMEDWAIT_ERROR;
}

#else /* POSIX */

_Static_assert(sizeof(pthread_mutex_t) <= sizeof(((iron_mutex_t *)0)->opaque), "iron_mutex_t storage");
_Static_assert(sizeof(pthread_cond_t) <= sizeof(((iron_cond_t *)0)->opaque), "iron_cond_t storage");
_Static_assert(sizeof(pthread_rwlock_t) <= sizeof(((iron_rwlock_t *)0)->opaque), "iron_rwlock_t storage");
_Static_assert(sizeof(pthread_t) <= sizeof(uintptr_t), "iron_thread_t storage");

#define MUTEX(m)  ((pthread_mutex_t *)(m)->opaque)
#define COND(c)   ((pthread_cond_t *)(c)->opaque)
#define RWLOCK(l) ((pthread_rwlock_t *)(l)->opaque)

int iron_thread_create(iron_thread_t *t, void *(*fn)(void *), void *arg) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return -1;
    pthread_attr_setstacksize(&attr, IRON_THREAD_STACK_SIZE);
    pthread_t th;
    int rc = pthread_create(&th, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) return rc;
    memcpy(&t->handle, &th, sizeof(th));
    return 0;
}
static pthread_t iron_thread_native(iron_thread_t t) {
    pthread_t th;
    memcpy(&th, &t.handle, sizeof(th));
    return th;
}
int iron_thread_join(iron_thread_t t)   { return pthread_join(iron_thread_native(t), NULL); }
int iron_thread_detach(iron_thread_t t) { return pthread_detach(iron_thread_native(t)); }
iron_thread_t iron_thread_self(void) {
    iron_thread_t t;
    t.handle = 0;
    pthread_t th = pthread_self();
    memcpy(&t.handle, &th, sizeof(th));
    return t;
}
bool iron_thread_equal(iron_thread_t a, iron_thread_t b) {
    return pthread_equal(iron_thread_native(a), iron_thread_native(b)) != 0;
}

void iron_mutex_init(iron_mutex_t *m)    { pthread_mutex_init(MUTEX(m), NULL); }
void iron_mutex_lock(iron_mutex_t *m)    { pthread_mutex_lock(MUTEX(m)); }
void iron_mutex_unlock(iron_mutex_t *m)  { pthread_mutex_unlock(MUTEX(m)); }
void iron_mutex_destroy(iron_mutex_t *m) { pthread_mutex_destroy(MUTEX(m)); }
void iron_cond_init(iron_cond_t *c)      { pthread_cond_init(COND(c), NULL); }
void iron_cond_wait(iron_cond_t *c, iron_mutex_t *m) { pthread_cond_wait(COND(c), MUTEX(m)); }
void iron_cond_signal(iron_cond_t *c)    { pthread_cond_signal(COND(c)); }
void iron_cond_broadcast(iron_cond_t *c) { pthread_cond_broadcast(COND(c)); }
void iron_cond_destroy(iron_cond_t *c)   { pthread_cond_destroy(COND(c)); }
void iron_rwlock_init(iron_rwlock_t *l)     { pthread_rwlock_init(RWLOCK(l), NULL); }
void iron_rwlock_rdlock(iron_rwlock_t *l)   { pthread_rwlock_rdlock(RWLOCK(l)); }
void iron_rwlock_wrlock(iron_rwlock_t *l)   { pthread_rwlock_wrlock(RWLOCK(l)); }
void iron_rwlock_rdunlock(iron_rwlock_t *l) { pthread_rwlock_unlock(RWLOCK(l)); }
void iron_rwlock_wrunlock(iron_rwlock_t *l) { pthread_rwlock_unlock(RWLOCK(l)); }
void iron_rwlock_destroy(iron_rwlock_t *l)  { pthread_rwlock_destroy(RWLOCK(l)); }

/* Bounded wait on CLOCK_REALTIME (may jump during an NTP slew; accepted). */
int iron_cond_timedwait_ms(iron_cond_t *cv, iron_mutex_t *lock, int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec  += 1;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_cond_timedwait(COND(cv), MUTEX(lock), &ts);
    if (rc == 0)          return IRON_TIMEDWAIT_OK;
    if (rc == ETIMEDOUT)  return IRON_TIMEDWAIT_EXPIRED;
    return IRON_TIMEDWAIT_ERROR;
}

#endif
