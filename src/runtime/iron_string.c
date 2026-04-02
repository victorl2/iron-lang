#include "iron_runtime.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* stb_ds hash map — STB_DS_IMPLEMENTATION is in src/util/stb_ds_impl.c */
#include "vendor/stb_ds.h"

/* ── Intern table ────────────────────────────────────────────────────────── */

/* Key: heap-allocated C string (owned by the table)
 * Value: the interned Iron_String (which points into the same key buffer) */
static struct { char *key; Iron_String value; } *s_intern_table = NULL;

static iron_mutex_t s_intern_lock;
#ifdef _WIN32
  static INIT_ONCE s_intern_once = INIT_ONCE_STATIC_INIT;
  static BOOL CALLBACK iron__init_intern_lock(PINIT_ONCE once, PVOID param, PVOID *ctx) {
      (void)once; (void)param; (void)ctx;
      IRON_MUTEX_INIT(s_intern_lock);
      return TRUE;
  }
  static inline void iron__ensure_intern_lock(void) {
      InitOnceExecuteOnce(&s_intern_once, iron__init_intern_lock, NULL, NULL);
  }
#else
  #include <pthread.h>
  static pthread_once_t s_intern_once = PTHREAD_ONCE_INIT;
  static void iron__init_intern_lock(void) {
      IRON_MUTEX_INIT(s_intern_lock);
  }
  static inline void iron__ensure_intern_lock(void) {
      pthread_once(&s_intern_once, iron__init_intern_lock);
  }
#endif

/* ── UTF-8 helpers ───────────────────────────────────────────────────────── */

static size_t count_codepoints(const char *data, size_t byte_len) {
    size_t count = 0;
    size_t i = 0;
    while (i < byte_len) {
        unsigned char c = (unsigned char)data[i];
        if      ((c & 0x80) == 0x00) { i += 1; }
        else if ((c & 0xE0) == 0xC0) { i += 2; }
        else if ((c & 0xF0) == 0xE0) { i += 3; }
        else                          { i += 4; }
        count++;
    }
    return count;
}

/* ── Iron_String construction ────────────────────────────────────────────── */

Iron_String iron_string_from_cstr(const char *cstr, size_t byte_len) {
    Iron_String s;
    memset(&s, 0, sizeof(s));

    if (byte_len <= IRON_STRING_SSO_MAX) {
        /* Inline / SSO path.
         * sso.data has IRON_STRING_SSO_MAX+1 slots, so data[byte_len] is valid
         * even when byte_len == IRON_STRING_SSO_MAX. */
        if (cstr && byte_len > 0) {
            memcpy(s.sso.data, cstr, byte_len);
        }
        s.sso.data[byte_len] = '\0';
        s.sso.len = (uint8_t)byte_len;
    } else {
        /* Heap path */
        char *buf = (char *)malloc(byte_len + 1);
        if (!buf) {
            /* OOM fallback: store as much as fits in SSO */
            size_t cap = IRON_STRING_SSO_MAX;
            if (cstr) memcpy(s.sso.data, cstr, cap);
            s.sso.data[cap] = '\0';
            s.sso.len = (uint8_t)cap;
            return s;
        }
        memcpy(buf, cstr, byte_len);
        buf[byte_len] = '\0';
        s.heap.data            = buf;
        s.heap.byte_length     = (uint32_t)byte_len;
        s.heap.codepoint_count = (uint32_t)count_codepoints(cstr, byte_len);
        s.heap.flags           = 0x01; /* is_heap */
    }
    return s;
}

Iron_String iron_string_from_literal(const char *lit, size_t byte_len) {
    Iron_String s = iron_string_from_cstr(lit, byte_len);
    return iron_string_intern(s);
}

/* ── Iron_String accessors ───────────────────────────────────────────────── */

const char *iron_string_cstr(const Iron_String *s) {
    if (s->heap.flags & 0x01) {
        return s->heap.data;
    }
    return s->sso.data;
}

size_t iron_string_byte_len(const Iron_String *s) {
    if (s->heap.flags & 0x01) {
        return (size_t)s->heap.byte_length;
    }
    return (size_t)s->sso.len;
}

size_t iron_string_codepoint_count(const Iron_String *s) {
    if (s->heap.flags & 0x01) {
        return (size_t)s->heap.codepoint_count;
    }
    /* SSO: count on demand (strings <= 23 bytes, cheap) */
    return count_codepoints(s->sso.data, (size_t)s->sso.len);
}

bool iron_string_equals(const Iron_String *a, const Iron_String *b) {
    size_t la = iron_string_byte_len(a);
    size_t lb = iron_string_byte_len(b);
    if (la != lb) return false;
    return memcmp(iron_string_cstr(a), iron_string_cstr(b), la) == 0;
}

Iron_String iron_string_concat(const Iron_String *a, const Iron_String *b) {
    size_t la    = iron_string_byte_len(a);
    size_t lb    = iron_string_byte_len(b);
    size_t total = la + lb;

    if (total <= IRON_STRING_SSO_MAX) {
        Iron_String s;
        memset(&s, 0, sizeof(s));
        memcpy(s.sso.data,      iron_string_cstr(a), la);
        memcpy(s.sso.data + la, iron_string_cstr(b), lb);
        s.sso.data[total] = '\0';
        s.sso.len         = (uint8_t)total;
        return s;
    }

    char *buf = (char *)malloc(total + 1);
    if (!buf) {
        return iron_string_from_cstr("", 0);
    }
    memcpy(buf,      iron_string_cstr(a), la);
    memcpy(buf + la, iron_string_cstr(b), lb);
    buf[total] = '\0';

    Iron_String s;
    memset(&s, 0, sizeof(s));
    s.heap.data            = buf;
    s.heap.byte_length     = (uint32_t)total;
    s.heap.codepoint_count = (uint32_t)count_codepoints(buf, total);
    s.heap.flags           = 0x01; /* is_heap */
    return s;
}

/* ── Interning ───────────────────────────────────────────────────────────── */

Iron_String iron_string_intern(Iron_String s) {
    const char *cstr = iron_string_cstr(&s);
    size_t      blen = iron_string_byte_len(&s);

    iron__ensure_intern_lock();
    IRON_MUTEX_LOCK(s_intern_lock);

    ptrdiff_t idx = shgeti(s_intern_table, cstr);
    if (idx >= 0) {
        Iron_String existing = s_intern_table[idx].value;
        IRON_MUTEX_UNLOCK(s_intern_lock);
        /* Free the heap allocation of the input string if not yet interned */
        if ((s.heap.flags & 0x01) && !(s.heap.flags & 0x02)) {
            free(s.heap.data);
        }
        return existing;
    }

    /* New entry — build an interned copy */
    Iron_String interned;
    memset(&interned, 0, sizeof(interned));

    if (blen <= IRON_STRING_SSO_MAX) {
        interned = s; /* already inline */
    } else {
        /* Heap string: mark as interned (intern table key owns a separate copy) */
        interned = s;
        interned.heap.flags = 0x03; /* is_heap | is_interned */
    }

    shput(s_intern_table, cstr, interned);

    IRON_MUTEX_UNLOCK(s_intern_lock);
    return interned;
}

/* Forward declarations for the thread subsystem (implemented in iron_threads.c) */
void iron_threads_init(void);
void iron_threads_shutdown(void);

/* ── Runtime lifecycle ───────────────────────────────────────────────────── */

/* Stored during iron_runtime_init for later access by iron_os_args() */
static int    s_iron_argc = 0;
static char **s_iron_argv = NULL;

void iron_runtime_init(int argc, char **argv) {
    s_iron_argc = argc;
    s_iron_argv = argv;
    iron__ensure_intern_lock();
    IRON_MUTEX_LOCK(s_intern_lock);
    if (s_intern_table == NULL) {
        sh_new_strdup(s_intern_table);
    }
    IRON_MUTEX_UNLOCK(s_intern_lock);
    iron_threads_init();
}

void iron_runtime_shutdown(void) {
    /* Shut down thread pool before freeing strings */
    iron_threads_shutdown();

    iron__ensure_intern_lock();
    IRON_MUTEX_LOCK(s_intern_lock);
    /* Free heap-allocated string data stored in the intern table values */
    for (ptrdiff_t i = 0; i < shlen(s_intern_table); i++) {
        Iron_String *v = &s_intern_table[i].value;
        if ((v->heap.flags & 0x01) && v->heap.data) {
            free(v->heap.data);
        }
    }
    shfree(s_intern_table);
    s_intern_table = NULL;
    IRON_MUTEX_UNLOCK(s_intern_lock);
}
