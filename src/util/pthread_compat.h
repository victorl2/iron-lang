/* pthread_compat.h: the few pthread calls the language server makes, on
 * every platform. POSIX systems get <pthread.h>; Windows gets the same
 * names over CRITICAL_SECTION, INIT_ONCE and CreateThread. Only what the
 * server uses is provided: mutexes (init, lock, unlock), once, and
 * threads created joinable or detached (create, join). Programs compiled
 * by ironc do not see this header; the runtime has its own primitives. */
#ifndef IRON_UTIL_PTHREAD_COMPAT_H
#define IRON_UTIL_PTHREAD_COMPAT_H

#ifndef _WIN32
  #include <pthread.h>
#else
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <windows.h>
  #include <stdlib.h>
  #undef interface
  #undef min
  #undef max

  typedef CRITICAL_SECTION pthread_mutex_t;
  typedef int pthread_mutexattr_t;
  #define PTHREAD_MUTEX_INITIALIZER { (void *)-1, -1, 0, 0, 0, 0 }

  static inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) {
      (void)a; InitializeCriticalSection(m); return 0;
  }
  static inline int pthread_mutex_destroy(pthread_mutex_t *m) { DeleteCriticalSection(m); return 0; }
  static inline int pthread_mutex_lock(pthread_mutex_t *m) { EnterCriticalSection(m); return 0; }
  static inline int pthread_mutex_unlock(pthread_mutex_t *m) { LeaveCriticalSection(m); return 0; }

  typedef INIT_ONCE pthread_once_t;
  #define PTHREAD_ONCE_INIT INIT_ONCE_STATIC_INIT
  typedef struct { void (*fn)(void); } iron_pc_once_ctx;
  static inline BOOL CALLBACK iron_pc_once_tramp(PINIT_ONCE o, PVOID param, PVOID *ctx) {
      (void)o; (void)ctx; ((iron_pc_once_ctx *)param)->fn(); return TRUE;
  }
  static inline int pthread_once(pthread_once_t *once, void (*fn)(void)) {
      iron_pc_once_ctx c = { fn };
      return InitOnceExecuteOnce(once, iron_pc_once_tramp, &c, NULL) ? 0 : -1;
  }

  typedef struct { HANDLE handle; } pthread_t;
  typedef struct { int detached; } pthread_attr_t;
  #define PTHREAD_CREATE_DETACHED 1
  #define PTHREAD_CREATE_JOINABLE 0
  static inline int pthread_attr_init(pthread_attr_t *a) { a->detached = 0; return 0; }
  static inline int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
  static inline int pthread_attr_setdetachstate(pthread_attr_t *a, int s) { a->detached = s; return 0; }

  typedef struct { void *(*fn)(void *); void *arg; } iron_pc_thread_ctx;
  static inline DWORD WINAPI iron_pc_thread_tramp(LPVOID p) {
      iron_pc_thread_ctx c = *(iron_pc_thread_ctx *)p;
      free(p);
      c.fn(c.arg);
      return 0;
  }
  static inline int pthread_create(pthread_t *t, const pthread_attr_t *a,
                                   void *(*fn)(void *), void *arg) {
      iron_pc_thread_ctx *c = (iron_pc_thread_ctx *)malloc(sizeof *c);
      if (!c) return -1;
      c->fn = fn; c->arg = arg;
      HANDLE h = CreateThread(NULL, 0, iron_pc_thread_tramp, c, 0, NULL);
      if (!h) { free(c); return -1; }
      if (a && a->detached) { CloseHandle(h); t->handle = NULL; }
      else t->handle = h;
      return 0;
  }
  static inline int pthread_join(pthread_t t, void **ret) {
      if (ret) *ret = NULL;
      if (!t.handle) return 0;
      WaitForSingleObject(t.handle, INFINITE);
      CloseHandle(t.handle);
      return 0;
  }
  static inline int pthread_detach(pthread_t t) { if (t.handle) CloseHandle(t.handle); return 0; }
#endif

#endif /* IRON_UTIL_PTHREAD_COMPAT_H */
