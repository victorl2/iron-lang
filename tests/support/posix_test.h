/* posix_test.h: the POSIX surface C tests use, on every platform.
 *
 * Tests include this instead of <unistd.h>, <dirent.h> and <pthread.h>.
 * On POSIX it is just those headers. On Windows it brings in util/os.h and
 * util/pthread_compat.h (the same shims the compiler and the language
 * server use) plus the few calls only tests need:
 *
 *   memmem, mkdtemp, setenv, unsetenv,
 *   IRON_TEST_TMP, the scratch directory prefix ("/tmp" on POSIX),
 *   open_memstream (contents are published on fflush and fclose, which is
 *   all the tests rely on) and fmemopen (read mode).
 *
 * Tests that need fork, waitpid, signals or pipes stay POSIX only; they are
 * not built on Windows (see tests/unit/CMakeLists.txt). */
#ifndef IRON_TESTS_POSIX_TEST_H
#define IRON_TESTS_POSIX_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/os.h"
#include "util/pthread_compat.h"

/* Where tests create scratch files. Windows has no /tmp; CMake points this
 * at a directory in the build tree there. */
#ifndef IRON_TEST_TMP
#define IRON_TEST_TMP "/tmp"
#endif

#ifdef _WIN32

static inline void *memmem(const void *hay, size_t hlen,
                           const void *needle, size_t nlen) {
    if (nlen == 0) return (void *)hay;
    const unsigned char *h = (const unsigned char *)hay;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (h[i] == *(const unsigned char *)needle &&
            memcmp(h + i, needle, nlen) == 0) {
            return (void *)(h + i);
        }
    }
    return NULL;
}

static inline char *mkdtemp(char *tmpl) {
    if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0) return NULL;
    return _mkdir(tmpl) == 0 ? tmpl : NULL;
}

static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}

static inline int unsetenv(const char *name) {
    return _putenv_s(name, "") == 0 ? 0 : -1;
}

/* open_memstream over a temporary file. The stream is registered so that
 * fflush and fclose (redefined below for the including test) copy what was
 * written into the caller's buffer, as POSIX does. */
typedef struct {
    FILE   *f;
    char  **buf;
    size_t *len;
} iron_test_memstream;

#define IRON_TEST_MEMSTREAMS 32
static iron_test_memstream iron_test_memstreams[IRON_TEST_MEMSTREAMS];

static inline FILE *open_memstream(char **buf, size_t *len) {
    FILE *f = tmpfile();
    if (!f) return NULL;
    for (int i = 0; i < IRON_TEST_MEMSTREAMS; i++) {
        if (!iron_test_memstreams[i].f) {
            iron_test_memstreams[i] = (iron_test_memstream){ f, buf, len };
            *buf = NULL;
            *len = 0;
            return f;
        }
    }
    fclose(f);
    return NULL;
}

static inline void iron_test_memstream_publish(iron_test_memstream *m) {
    long pos = ftell(m->f);
    if (pos < 0) return;
    char *data = (char *)malloc((size_t)pos + 1);
    if (!data) return;
    rewind(m->f);
    size_t got = fread(data, 1, (size_t)pos, m->f);
    data[got] = '\0';
    fseek(m->f, pos, SEEK_SET);
    free(*m->buf);
    *m->buf = data;
    *m->len = got;
}

static inline iron_test_memstream *iron_test_memstream_find(FILE *f) {
    for (int i = 0; i < IRON_TEST_MEMSTREAMS; i++) {
        if (f && iron_test_memstreams[i].f == f) return &iron_test_memstreams[i];
    }
    return NULL;
}

static inline int iron_test_fflush(FILE *f) {
    int rc = fflush(f);
    iron_test_memstream *m = iron_test_memstream_find(f);
    if (m) iron_test_memstream_publish(m);
    return rc;
}

static inline int iron_test_fclose(FILE *f) {
    iron_test_memstream *m = iron_test_memstream_find(f);
    if (m) {
        fflush(f);
        iron_test_memstream_publish(m);
        m->f = NULL;
    }
    return fclose(f);
}

#define fflush iron_test_fflush
#define fclose iron_test_fclose

/* fmemopen, read mode only: the bytes are copied into a temporary file. */
static inline FILE *fmemopen(void *buf, size_t size, const char *mode) {
    (void)mode;
    FILE *f = tmpfile();
    if (!f) return NULL;
    if (size && fwrite(buf, 1, size, f) != size) { fclose(f); return NULL; }
    rewind(f);
    return f;
}

#endif /* _WIN32 */

#endif /* IRON_TESTS_POSIX_TEST_H */
