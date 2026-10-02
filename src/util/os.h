/* os.h: the small POSIX surface the compiler host code uses, on every
 * platform. POSIX systems get their own headers; Windows gets the UCRT /
 * Win32 equivalents under the POSIX names, so call sites stay portable.
 *
 * Covered: unistd (access, isatty, getcwd, chdir, STD*_FILENO), sys/stat
 * (S_ISDIR / S_ISREG), dirent (opendir / readdir / closedir), libgen
 * (dirname / basename), realpath, and the C11-style clock and sleep used
 * by the stdlib C sources. Process spawning stays in src/cli/build.c,
 * which has both implementations. */
#ifndef IRON_UTIL_OS_H
#define IRON_UTIL_OS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <windows.h>
  #include <direct.h>
  #include <io.h>
  #include <process.h>
  /* windows.h defines `interface` as a macro; src/analyzer/types.h has a
   * union member of that name. */
  #undef interface
  #undef min
  #undef max

  #ifndef STDIN_FILENO
    #define STDIN_FILENO  0
    #define STDOUT_FILENO 1
    #define STDERR_FILENO 2
  #endif
  #ifndef F_OK
    #define F_OK 0
    #define X_OK 1   /* _access has no execute bit: existence is the test */
    #define W_OK 2
    #define R_OK 4
  #endif
  #define iron_os_access(p, m) _access((p), (m) == X_OK ? 0 : (m))
  #define access   iron_os_access
  #define isatty   _isatty
  #define getcwd   _getcwd
  #define chdir    _chdir
  #define unlink   _unlink
  #define rmdir    _rmdir
  #define getpid   _getpid
  #define fileno   _fileno
  #define strdup   _strdup
  #define popen    _popen
  #define pclose   _pclose
  typedef int mode_t;
  typedef intptr_t ssize_t;
  /* mkdir(path, mode): the mode is ignored on Windows. */
  static inline int iron_os_mkdir(const char *path, mode_t mode) { (void)mode; return _mkdir(path); }
  #define mkdir iron_os_mkdir

  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
  #ifndef S_ISREG
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif
  #define lstat stat

  /* dirname / basename: in place on a mutable copy, as POSIX allows. */
  static inline char *iron_os_dirname(char *path) {
      char *p = path + strlen(path);
      while (p > path && (p[-1] == '/' || p[-1] == '\\')) p--;   /* trailing separators */
      while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
      if (p == path) { path[0] = '.'; path[1] = '\0'; return path; }
      while (p > path + 1 && (p[-1] == '/' || p[-1] == '\\')) p--;
      *p = '\0';
      return path;
  }
  static inline char *iron_os_basename(char *path) {
      char *p = path + strlen(path);
      while (p > path && (p[-1] == '/' || p[-1] == '\\')) *--p = '\0';
      while (p > path && p[-1] != '/' && p[-1] != '\\') p--;
      return p;
  }
  #define dirname  iron_os_dirname
  #define basename iron_os_basename

  #ifndef PATH_MAX
    #define PATH_MAX MAX_PATH
  #endif
  static inline char *realpath(const char *path, char *resolved) {
      return _fullpath(resolved, path, resolved ? PATH_MAX : 0);
  }

  /* dirent: FindFirstFile / FindNextFile behind the POSIX API. */
  struct dirent { char d_name[MAX_PATH]; };
  typedef struct {
      HANDLE h;
      WIN32_FIND_DATAA data;
      bool first;
      struct dirent ent;
  } DIR;
  static inline DIR *opendir(const char *path) {
      char pattern[MAX_PATH + 4];
      snprintf(pattern, sizeof(pattern), "%s\\*", path);
      DIR *d = (DIR *)calloc(1, sizeof(DIR));
      if (!d) return NULL;
      d->h = FindFirstFileA(pattern, &d->data);
      if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
      d->first = true;
      return d;
  }
  static inline struct dirent *readdir(DIR *d) {
      if (!d) return NULL;
      if (d->first) d->first = false;
      else if (!FindNextFileA(d->h, &d->data)) return NULL;
      strncpy(d->ent.d_name, d->data.cFileName, MAX_PATH - 1);
      d->ent.d_name[MAX_PATH - 1] = '\0';
      return &d->ent;
  }
  static inline int closedir(DIR *d) {
      if (!d) return -1;
      FindClose(d->h);
      free(d);
      return 0;
  }

  /* clock_gettime / nanosleep: the UCRT has timespec but not these calls. */
  #ifndef CLOCK_REALTIME
    #define CLOCK_REALTIME  0
    #define CLOCK_MONOTONIC 1
    typedef int clockid_t;
    static inline int clock_gettime(clockid_t id, struct timespec *ts) {
        if (id == CLOCK_MONOTONIC) {
            LARGE_INTEGER f, c;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&c);
            ts->tv_sec = (time_t)(c.QuadPart / f.QuadPart);
            ts->tv_nsec = (long)((c.QuadPart % f.QuadPart) * 1000000000LL / f.QuadPart);
            return 0;
        }
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;   /* 100 ns since 1601 */
        t -= 116444736000000000ULL;                                             /* to 1970 */
        ts->tv_sec = (time_t)(t / 10000000ULL);
        ts->tv_nsec = (long)((t % 10000000ULL) * 100);
        return 0;
    }
  #endif
  static inline int nanosleep(const struct timespec *req, struct timespec *rem) {
      (void)rem;
      Sleep((DWORD)(req->tv_sec * 1000 + req->tv_nsec / 1000000));
      return 0;
  }
  static inline unsigned iron_os_sleep(unsigned seconds) { Sleep(seconds * 1000); return 0; }
  #define sleep iron_os_sleep
#else
  #include <dirent.h>
  #include <libgen.h>
  #include <limits.h>
  #include <unistd.h>
#endif

#endif /* IRON_UTIL_OS_H */
