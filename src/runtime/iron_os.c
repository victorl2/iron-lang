/* iron_os.c: the platform behind the freestanding runtime header.
 *
 * iron_runtime.h declares memory, output, thread and lock primitives as
 * plain functions so that generated C and the header's inline helpers
 * never include a libc or OS header (#235). This file is the one place
 * that does: it forwards the memory calls to libc and implements the
 * threading handles over pthreads or Win32 inside the opaque storage the
 * header declares, with static assertions that the storage fits. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
  #define _GNU_SOURCE 1   /* sched_getaffinity / CPU_COUNT for OS.cpu_count */
#endif
#include "runtime/iron_runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <pthread.h>
  #include <sched.h>
  #include <unistd.h>
#endif
#ifdef __APPLE__
  #include <sys/types.h>
  #include <sys/sysctl.h>
#endif
#ifdef __linux__
  #include <sched.h>
#endif
#include <stdarg.h>

/* ── Memory ─────────────────────────────────────────────────────────────── */

/* A request for zero bytes gets one: malloc(0) may return NULL and
 * realloc(p, 0) may free p and return NULL, which every caller reads as
 * out of memory (a list of an object with no fields aborted on push). */
void  *iron_mem_alloc(size_t size)                        { return malloc(size ? size : 1); }
void  *iron_mem_calloc(size_t count, size_t size)         { return calloc(count ? count : 1, size ? size : 1); }
/* A list buffer an element pointer is borrowed from (iron_list_pin) must
 * not move or go away while the borrowing call runs (#543). */
void  *iron_mem_realloc(void *ptr, size_t size) {
    IRON_LIST_PIN_CHECK(ptr);
    return realloc(ptr, size ? size : 1);
}
void   iron_mem_free(void *ptr) {
    IRON_LIST_PIN_CHECK(ptr);
    free(ptr);
}
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

/* ── Debug traps (#388) ──────────────────────────────────────────────────
 * The checks of a --debug build call iron_debug_panic_stop just before a
 * panic; when it returns true they run a breakpoint instruction in the Iron
 * function (IRON_DEBUG_BREAK in iron_runtime.h). Asked at the panic, not
 * once at startup, so a debugger attached to a running program counts. */
char iron_debug_panic_message[512];

static bool iron_debugger_attached(void) {
#if defined(_WIN32)
    return IsDebuggerPresent() != 0;
#elif defined(__APPLE__)
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };
    struct kinfo_proc info;
    size_t size = sizeof info;
    memset(&info, 0, sizeof info);
    if (sysctl(mib, 4, &info, &size, NULL, 0) != 0) return false;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#elif defined(__linux__)
    /* TracerPid in /proc/self/status: nonzero while ptrace-attached. */
    char buf[2048];
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    const char *t = strstr(buf, "TracerPid:");
    if (!t) return false;
    t += 10;
    while (*t == ' ' || *t == '\t') t++;
    return *t >= '1' && *t <= '9';
#else
    return false;
#endif
}

bool iron_debug_panic_stop(const char *site_file, int site_line, const char *fmt, ...) {
    if (site_file && site_line <= 0) return false;  /* a runtime body: no Iron line */
    if (!iron_debugger_attached()) return false;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(iron_debug_panic_message, sizeof iron_debug_panic_message, fmt, ap);
    va_end(ap);
    fflush(stdout);
    if (site_file)
        fprintf(stderr, "panic: %s at %s:%d\n", iron_debug_panic_message, site_file, site_line);
    else
        fprintf(stderr, "panic: %s\n", iron_debug_panic_message);
    fflush(stderr);
    return true;
}

/* ── FileHandle ─────────────────────────────────────────────────────────────
 * Opens the file for reading and writing, creating it when it does not
 * exist; an existing file keeps its contents (it used to be opened with
 * fopen "w", which truncated it, and the FILE was never freed). A path
 * that is empty or holds a zero byte opens nothing (-1). */
int iron_filehandle_open(const char *path, size_t len) {
    if (!path || len == 0 || memchr(path, '\0', len) != NULL) return -1;
#ifdef _WIN32
    return _open(path, _O_RDWR | _O_CREAT | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    return open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
#endif
}
void iron_filehandle_close(int fd) {
    if (fd < 0) return;
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
        IRON_HB_BEFORE(&once->state);
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
    IRON_HB_AFTER(&once->state);
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
    iron_stack_thread_exit();   /* its scope table and cached slots go back */
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

/* Every runtime thread ends through this trampoline, which gives back its
 * stack scope table number and its cached generation slots. */
typedef struct { void *(*fn)(void *); void *arg; } iron_thread_start;
static void *iron_thread_trampoline(void *p) {
    iron_thread_start *t = (iron_thread_start *)p;
    void *(*fn)(void *) = t->fn;
    void *arg = t->arg;
    free(t);
    void *r = fn(arg);
    iron_stack_thread_exit();
    return r;
}
int iron_thread_create(iron_thread_t *t, void *(*fn)(void *), void *arg) {
    iron_thread_start *start = (iron_thread_start *)malloc(sizeof(*start));
    if (!start) return -1;
    start->fn = fn;
    start->arg = arg;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) { free(start); return -1; }
    pthread_attr_setstacksize(&attr, IRON_THREAD_STACK_SIZE);
    pthread_t th;
    int rc = pthread_create(&th, &attr, iron_thread_trampoline, start);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(start); return rc; }
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

/* ── Environment and command line ───────────────────────────────────────── */

#ifdef _WIN32
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

/* UTF-8 <-> UTF-16 for the wide Windows APIs; the result is malloc'd. */
static wchar_t *os_utf8_to_wide(const char *s, size_t len) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(((size_t)n + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, (int)len, w, n);
    w[n] = 0;
    return w;
}

static char *os_wide_to_utf8(const wchar_t *w, int *out_len) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (!s) return NULL;
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    else s[0] = 0;
    if (out_len) *out_len = n > 0 ? n - 1 : 0;
    return s;
}

/* The command line as UTF-8: the C runtime's argv is in the ANSI code
 * page, so non-ASCII arguments would not round-trip. */
int iron_os_utf8_args(char ***out) {
    int n = 0;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
    *out = NULL;
    if (!w) return 0;
    char **v = (char **)calloc(n > 0 ? (size_t)n : 1, sizeof(char *));
    if (v) for (int i = 0; i < n; i++) v[i] = os_wide_to_utf8(w[i], NULL);
    LocalFree(w);
    *out = v;
    return v ? n : 0;
}

void iron_os_utf8_args_free(char **v, int n) {
    if (!v) return;
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

/* The variable's value as UTF-8 (malloc'd), or NULL when it is not set. */
static char *os_getenv(const Iron_String *name, int *out_len) {
    wchar_t *wn = os_utf8_to_wide(iron_string_cstr(name), iron_string_byte_len(name));
    if (!wn) return NULL;
    DWORD need = GetEnvironmentVariableW(wn, NULL, 0);
    if (need == 0) {
        bool unset = GetLastError() == ERROR_ENVVAR_NOT_FOUND;
        free(wn);
        if (unset) return NULL;
        char *empty = (char *)malloc(1);
        if (empty) empty[0] = 0;
        if (out_len) *out_len = 0;
        return empty;
    }
    wchar_t *wv = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
    if (!wv) { free(wn); return NULL; }
    GetEnvironmentVariableW(wn, wv, need);
    free(wn);
    char *v = os_wide_to_utf8(wv, out_len);
    free(wv);
    return v;
}
#else
static char *os_getenv(const Iron_String *name, int *out_len) {
    const char *v = getenv(iron_string_cstr(name));
    if (!v) return NULL;
    size_t n = strlen(v);
    char *copy = (char *)malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, v, n + 1);
    if (out_len) *out_len = (int)n;
    return copy;
}
#endif

/* os.iron: OS.has_env(name) and OS.env_or(name, fallback); OS.env(name)
 * is written in Iron on top of them. */
/* A name that is empty or holds '=' or a zero byte names no variable
 * (getenv would look up a prefix of it, or a different variable). */
static bool os_env_name_ok(const Iron_String *name) {
    size_t n = iron_string_byte_len(name);
    const char *p = iron_string_cstr(name);
    return n > 0 && memchr(p, '\0', n) == NULL && memchr(p, '=', n) == NULL;
}

bool Iron_os_has_env(Iron_String name) {
    if (!os_env_name_ok(&name)) return false;
    char *v = os_getenv(&name, NULL);
    bool set = v != NULL;
    free(v);
    return set;
}

Iron_String Iron_os_env_or(Iron_String name, Iron_String fallback) {
    int len = 0;
    char *v = os_env_name_ok(&name) ? os_getenv(&name, &len) : NULL;
    if (!v) return iron_string_from_cstr(iron_string_cstr(&fallback),
                                         iron_string_byte_len(&fallback));
    Iron_String s = iron_string_from_cstr(v, (size_t)len);
    free(v);
    return s;
}

/* os.iron: OS.trap_shutdown_signals() and OS.shutdown_signal().
 *
 * After the trap is installed, SIGINT and SIGTERM (Ctrl+C, Ctrl+Break and
 * console close on Windows) no longer end the process: the handler records
 * the first signal and the program polls OS.shutdown_signal() to drain and
 * exit on its own. A second signal of either kind ends the process at once,
 * so a stuck shutdown can still be interrupted. */
static volatile sig_atomic_t os_shutdown_signo = 0;

#ifdef _WIN32
static BOOL WINAPI os_console_handler(DWORD kind) {
    switch (kind) {
    case CTRL_C_EVENT: case CTRL_BREAK_EVENT: case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (os_shutdown_signo != 0) ExitProcess(130);
        os_shutdown_signo = kind == CTRL_C_EVENT ? 2 : 15;
        return TRUE;
    default:
        return FALSE;
    }
}

void Iron_os_trap_shutdown_signals(void) {
    SetConsoleCtrlHandler(os_console_handler, TRUE);
}
#else
static void os_shutdown_handler(int signo) {
    if (os_shutdown_signo != 0) {
        signal(signo, SIG_DFL);
        raise(signo);
        return;
    }
    os_shutdown_signo = signo;
}

void Iron_os_trap_shutdown_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = os_shutdown_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}
#endif

int64_t Iron_os_shutdown_signal(void) { return (int64_t)os_shutdown_signo; }

int64_t Iron_os_pid(void) {
#ifdef _WIN32
    return (int64_t)GetCurrentProcessId();
#else
    return (int64_t)getpid();
#endif
}

/* os.iron: OS.exit(code). Flushes standard output and error and ends the
 * process with the status (truncated to 0..255 on POSIX). Drops, defers and
 * other tasks do not run: it is for fatal errors and command-line tools. */
void Iron_os_exit(int64_t code) {
    fflush(stdout);
    fflush(stderr);
    exit((int)code);
}

/* os.iron: OS.cpu_count(). The CPUs this process may run on: the affinity
 * mask on Linux (so a container or taskset limit is respected), the online
 * processors elsewhere. At least 1. */
int64_t Iron_os_cpu_count(void) {
#ifdef _WIN32
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int64_t)n : 1;
#else
  #ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
  #endif
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int64_t)n : 1;
#endif
}
