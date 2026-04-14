#include "iron_runtime.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

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
        /* FIX-02: replace Phase 65 silent-truncation fallback with iron_oom_abort
         * for consistent OOM reporting (AUDIT-06 §21). */
        /* FIX-03 / AUDIT-04 §10: SAFETY — cross-reference. Phase 65 audit row
         * §4.10 flagged the silent SSO-truncation fallback on OOM as
         * "caller cannot detect truncation". That fallback was REMOVED in
         * Phase 67-06 (commit 85e925c) — the path now calls iron_oom_abort
         * with a named location literal, so a grep of stderr during any
         * OOM run pinpoints this exact site. Row 10 is closed by that
         * earlier FIX-02 edit; no additional work required. */
        char *buf = (char *)malloc(byte_len + 1);
        if (!buf) iron_oom_abort("iron_string.c:iron_string_from_cstr");
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

    /* FIX-02: replace Phase 65 silent empty-string fallback with iron_oom_abort. */
    char *buf = (char *)malloc(total + 1);
    if (!buf) iron_oom_abort("iron_string.c:iron_string_concat");
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

/* Thread-safety contract (WEB-RUNTIME-01):
 *
 * The intern table is protected by a single mutex `s_intern_lock` that
 * covers the entire shgeti/shput critical section. Lock initialization
 * uses `pthread_once` (POSIX) or `InitOnceExecuteOnce` (Win32), both of
 * which provide sequentially-consistent happens-before for first-reader
 * visibility of the mutex.
 *
 * Under Emscripten + SharedArrayBuffer + -pthread:
 *   - Worker threads calling iron_string_intern() block on
 *     pthread_mutex_lock, which maps to Atomics.wait. This is permitted
 *     on non-main (Web Worker) threads.
 *   - The main browser thread calls iron_string_intern() indirectly only
 *     via iron_runtime_init() at program start, before any worker spawn.
 *     That lock acquire is uncontested and returns immediately, which is
 *     safe on the main thread (no actual Atomics.wait suspension).
 *   - PROXY_TO_PTHREAD is a forbidden emcc flag (WEB-BUILD-07), so
 *     iron_runtime_init genuinely runs on the browser main thread; the
 *     uncontested-at-init invariant is enforced by compile-time flag
 *     policy.
 *
 * A double-checked read path using IRON_ATOMIC_LOAD on a snapshot pointer
 * is NOT required: the single-mutex pattern is race-free. This invariant
 * is empirically verified by tests/unit/test_string_intern_race.c under
 * ThreadSanitizer. If that test ever reports a data race, THIS COMMENT
 * IS WRONG and a DCL upgrade is required — see CONTEXT.md for the
 * upgrade playbook.
 */
/* FIX-03 / AUDIT-04 §11: iron_string_intern OWNERSHIP CONTRACT.
 *
 * iron_string_intern takes ownership of the input Iron_String's heap
 * allocation (`s.heap.data` when s.heap.flags & 0x01 is set AND the
 * is_interned bit 0x02 is NOT yet set). If the input's content is already
 * present in the intern table, this function frees the input's heap.data
 * and returns the existing interned copy — the caller MUST NOT retain any
 * pointer derived from `s.heap.data` (such as a prior `iron_string_cstr(&s)`
 * result) after this call, because that pointer is a use-after-free.
 *
 * Callers MUST discard the input Iron_String value after interning and use
 * the returned Iron_String instead. The Iron compiler's string-literal
 * interning path (iron_string_from_literal in this file) already follows
 * this contract: it assigns the return value to the same local and
 * immediately uses that returned value. Any future caller that wants to
 * keep both the input and the interned copy around must copy the input's
 * bytes into a fresh Iron_String FIRST and then intern the copy.
 *
 * This comment makes the pre-existing runtime contract grep-visible. No
 * code change — the contract is enforced by convention on every Iron
 * string-literal path, not by runtime assertion (an assertion would
 * require modifying s, but s is a by-value Iron_String argument, so any
 * mutation dies with the function call). */
Iron_String iron_string_intern(Iron_String s) {
    const char *cstr = iron_string_cstr(&s);
    size_t      blen = iron_string_byte_len(&s);

    iron__ensure_intern_lock();
    IRON_MUTEX_LOCK(s_intern_lock);

    ptrdiff_t idx = shgeti(s_intern_table, cstr);
    if (idx >= 0) {
        Iron_String existing = s_intern_table[idx].value;
        IRON_MUTEX_UNLOCK(s_intern_lock);
        /* FIX-03 / AUDIT-04 §11: free the heap allocation of the input
         * string if not yet interned. After this point `cstr` (computed
         * above from &s.heap.data) is dangling; DO NOT touch it again in
         * this function, and callers MUST discard their local copy of s. */
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

    /* Phase 59 P01c: network runtime hooks — WSAStartup (Windows) and
     * SIGPIPE=SIG_IGN (POSIX). Both hooks are idempotent:
     *   - Iron_net_wsa_startup_once is refcounted under an internal mutex
     *   - iron_net_install_sigpipe_ignore is a plain signal() SIG_IGN
     * so repeated iron_runtime_init calls (unit tests that init per test)
     * are safe. The return value of Iron_net_wsa_startup_once is ignored
     * here because a WSAStartup failure is rare and the runtime has no
     * panic channel — downstream socket APIs will surface IRON_ERR_NET_*
     * on their first use. */
    (void)Iron_net_wsa_startup_once();
    iron_net_install_sigpipe_ignore();
}

void iron_runtime_shutdown(void) {
    /* Phase 59 P01c: tear down network runtime hooks BEFORE thread pool
     * teardown so any elastic-I/O pool worker still holding a socket
     * doesn't race with WSACleanup. (On POSIX this is a no-op so the
     * ordering only matters on Windows.) */
    Iron_net_wsa_cleanup_once();

    /* Shut down thread pool before freeing strings */
    iron_threads_shutdown();

    iron__ensure_intern_lock();
    IRON_MUTEX_LOCK(s_intern_lock);
    /* FIX-03 / AUDIT-04 §12: SAFETY — verify shput key-storage independence.
     *
     * Phase 65 audit row §4.12 worried that `iron_runtime_shutdown` might
     * double-free: freeing v->heap.data below and then shfree'ing the
     * intern table could free the same bytes twice if the stb_ds shmap
     * stored the VALUE's heap.data pointer as its KEY (aliasing).
     *
     * Verification (audited at 67-07):
     *   1. s_intern_table is initialized with `sh_new_strdup(s_intern_table)`
     *      in iron_runtime_init above (line ~235). `sh_new_strdup` tells
     *      stb_ds to call `strdup()` on every key passed to `shput`,
     *      producing an independent heap copy owned by the shmap.
     *   2. The `shput(s_intern_table, cstr, interned)` call in
     *      iron_string_intern passes `cstr = iron_string_cstr(&s)` as the
     *      key. For the heap path, this returns `s.heap.data`. stb_ds
     *      strdup's this pointer's content into a fresh heap block.
     *   3. The VALUE stored is `interned`, which is `s` itself (see
     *      intern function body `interned = s;`). So `interned.heap.data`
     *      IS s.heap.data — the same pointer returned by iron_string_cstr.
     *   4. At shutdown: we free `v->heap.data` (the ORIGINAL pointer from
     *      step 3), THEN `shfree(s_intern_table)` which frees the stb_ds
     *      strdup'd KEY copy from step 1. These are TWO DIFFERENT heap
     *      blocks — no double-free.
     *
     * The two-pointer separation depends critically on `sh_new_strdup`
     * being called in iron_runtime_init. If anyone ever replaces it with
     * `sh_new_arena` or plain `shput` (without strdup), the shutdown path
     * below becomes a double-free. Enforce at review: search for
     * sh_new_strdup in this file — it MUST be the shmap initializer. */
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

/* ── String built-in methods (Phase 38) ─────────────────────────────────── */

Iron_String Iron_string_upper(Iron_String self) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    /* FIX-02: replace silent empty-string fallback with iron_oom_abort. */
    char *buf = (char *)malloc(len + 1);
    if (!buf) iron_oom_abort("iron_string.c:Iron_string_upper");
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)toupper((unsigned char)s[i]);
    buf[len] = '\0';
    Iron_String result = iron_string_from_cstr(buf, len);
    free(buf);
    return result;
}

Iron_String Iron_string_lower(Iron_String self) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    /* FIX-02: replace silent empty-string fallback with iron_oom_abort. */
    char *buf = (char *)malloc(len + 1);
    if (!buf) iron_oom_abort("iron_string.c:Iron_string_lower");
    for (size_t i = 0; i < len; i++)
        buf[i] = (char)tolower((unsigned char)s[i]);
    buf[len] = '\0';
    Iron_String result = iron_string_from_cstr(buf, len);
    free(buf);
    return result;
}

Iron_String Iron_string_trim(Iron_String self) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    size_t start = 0, end = len;
    while (start < end && (unsigned char)s[start] <= ' ') start++;
    while (end > start && (unsigned char)s[end-1] <= ' ') end--;
    return iron_string_from_cstr(s + start, end - start);
}

bool Iron_string_contains(Iron_String self, Iron_String sub) {
    const char *s = iron_string_cstr(&self);
    const char *d = iron_string_cstr(&sub);
    return strstr(s, d) != NULL;
}

bool Iron_string_starts_with(Iron_String self, Iron_String prefix) {
    const char *s    = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *p    = iron_string_cstr(&prefix);
    size_t      plen = iron_string_byte_len(&prefix);
    if (plen > slen) return false;
    return memcmp(s, p, plen) == 0;
}

bool Iron_string_ends_with(Iron_String self, Iron_String suffix) {
    const char *s      = iron_string_cstr(&self);
    size_t      slen   = iron_string_byte_len(&self);
    const char *sfx    = iron_string_cstr(&suffix);
    size_t      sfxlen = iron_string_byte_len(&suffix);
    if (sfxlen > slen) return false;
    return memcmp(s + slen - sfxlen, sfx, sfxlen) == 0;
}

int64_t Iron_string_index_of(Iron_String self, Iron_String sub) {
    const char *s   = iron_string_cstr(&self);
    const char *d   = iron_string_cstr(&sub);
    const char *hit = strstr(s, d);
    if (!hit) return -1;
    return (int64_t)(hit - s);
}

Iron_String Iron_string_char_at(Iron_String self, int64_t i) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    if (i < 0 || (size_t)i >= len) return iron_string_from_cstr("", 0);
    return iron_string_from_cstr(s + (size_t)i, 1);
}

int64_t Iron_string_len(Iron_String self) {
    return (int64_t)iron_string_byte_len(&self);
}

int64_t Iron_string_count(Iron_String self, Iron_String sub) {
    const char *s    = iron_string_cstr(&self);
    const char *d    = iron_string_cstr(&sub);
    size_t      dlen = iron_string_byte_len(&sub);
    if (dlen == 0) return 0;
    int64_t cnt = 0;
    const char *p = s;
    while ((p = strstr(p, d)) != NULL) { cnt++; p += dlen; }
    return cnt;
}

/* ── String built-in methods — wave 2 (Phase 38 Plan 02) ────────────────── */

Iron_List_Iron_String Iron_string_split(Iron_String self, Iron_String sep) {
    Iron_List_Iron_String result = Iron_List_Iron_String_create();
    const char *s    = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *d    = iron_string_cstr(&sep);
    size_t      dlen = iron_string_byte_len(&sep);

    if (dlen == 0) {
        /* empty separator: each character is its own element */
        for (size_t i = 0; i < slen; i++) {
            Iron_String ch = iron_string_from_cstr(s + i, 1);
            Iron_List_Iron_String_push(&result, ch);
        }
        return result;
    }

    const char *cur = s;
    const char *end = s + slen;
    while (cur <= end) {
        const char *hit = (cur < end) ? strstr(cur, d) : NULL;
        if (!hit) hit = end;
        Iron_String part = iron_string_from_cstr(cur, (size_t)(hit - cur));
        Iron_List_Iron_String_push(&result, part);
        if (hit == end) break;
        cur = hit + dlen;
    }
    return result;
}

Iron_String Iron_string_join(Iron_String self, Iron_List_Iron_String parts) {
    const char *sep    = iron_string_cstr(&self);
    size_t      seplen = iron_string_byte_len(&self);
    int64_t     n      = Iron_List_Iron_String_len(&parts);
    if (n == 0) return iron_string_from_cstr("", 0);

    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        Iron_String item = Iron_List_Iron_String_get(&parts, i);
        total += iron_string_byte_len(&item);
    }
    total += (size_t)(n > 0 ? n - 1 : 0) * seplen;

    /* FIX-02: replace silent empty-string fallback with iron_oom_abort. */
    char *buf = (char *)malloc(total + 1);
    if (!buf) iron_oom_abort("iron_string.c:Iron_string_join");
    char *p = buf;
    for (int64_t i = 0; i < n; i++) {
        if (i > 0) { memcpy(p, sep, seplen); p += seplen; }
        Iron_String item = Iron_List_Iron_String_get(&parts, i);
        size_t ilen = iron_string_byte_len(&item);
        memcpy(p, iron_string_cstr(&item), ilen);
        p += ilen;
    }
    *p = '\0';
    Iron_String result = iron_string_from_cstr(buf, total);
    free(buf);
    return result;
}

Iron_String Iron_string_replace(Iron_String self, Iron_String old_s, Iron_String new_s) {
    const char *s      = iron_string_cstr(&self);
    size_t      slen   = iron_string_byte_len(&self);
    const char *oldc   = iron_string_cstr(&old_s);
    size_t      oldlen = iron_string_byte_len(&old_s);
    const char *newc   = iron_string_cstr(&new_s);
    size_t      newlen = iron_string_byte_len(&new_s);

    if (oldlen == 0) return self; /* no-op: empty old pattern */

    /* First pass: count occurrences to size the output buffer */
    size_t count = 0;
    const char *p = s;
    while ((p = strstr(p, oldc)) != NULL) { count++; p += oldlen; }

    size_t total = slen + count * (newlen - oldlen);
    /* FIX-02: replace silent self-return fallback with iron_oom_abort. */
    char *buf = (char *)malloc(total + 1);
    if (!buf) iron_oom_abort("iron_string.c:Iron_string_replace");

    char *out = buf;
    const char *cur = s;
    while (1) {
        const char *hit = strstr(cur, oldc);
        if (!hit) {
            /* copy remainder */
            size_t tail = (size_t)((s + slen) - cur);
            memcpy(out, cur, tail);
            out += tail;
            break;
        }
        /* copy segment before hit */
        size_t seg = (size_t)(hit - cur);
        memcpy(out, cur, seg); out += seg;
        /* copy replacement */
        memcpy(out, newc, newlen); out += newlen;
        cur = hit + oldlen;
    }
    *out = '\0';
    Iron_String result = iron_string_from_cstr(buf, total);
    free(buf);
    return result;
}

Iron_String Iron_string_substring(Iron_String self, int64_t start, int64_t end_idx) {
    const char *s   = iron_string_cstr(&self);
    int64_t     len = (int64_t)iron_string_byte_len(&self);
    if (start < 0) start = 0;
    if (end_idx > len) end_idx = len;
    if (start > end_idx) start = end_idx;
    return iron_string_from_cstr(s + start, (size_t)(end_idx - start));
}

int64_t Iron_string_to_int(Iron_String self) {
    const char *s = iron_string_cstr(&self);
    char *end;
    int64_t v = (int64_t)strtoll(s, &end, 10);
    if (end == s) return 0;  /* nothing consumed */
    return v;
}

double Iron_string_to_float(Iron_String self) {
    const char *s = iron_string_cstr(&self);
    char *end;
    double v = strtod(s, &end);
    if (end == s) return 0.0;
    return v;
}

Iron_String Iron_string_repeat(Iron_String self, int64_t n) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    if (n <= 0 || len == 0) return iron_string_from_cstr("", 0);
    size_t total = len * (size_t)n;
    /* FIX-02: replace silent empty-string fallback with iron_oom_abort. */
    char *buf = (char *)malloc(total + 1);
    if (!buf) iron_oom_abort("iron_string.c:Iron_string_repeat");
    for (int64_t i = 0; i < n; i++)
        memcpy(buf + (size_t)i * len, s, len);
    buf[total] = '\0';
    Iron_String result = iron_string_from_cstr(buf, total);
    free(buf);
    return result;
}

Iron_String Iron_string_pad_left(Iron_String self, int64_t width, Iron_String ch) {
    const char *s    = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *pad  = iron_string_cstr(&ch);
    size_t      plen = iron_string_byte_len(&ch);
    char        pc   = (plen > 0) ? pad[0] : ' ';

    if ((int64_t)slen >= width) return self;
    size_t pad_count = (size_t)width - slen;
    size_t total     = (size_t)width;
    /* FIX-02: replace silent self-return fallback with iron_oom_abort. */
    char *buf = (char *)malloc(total + 1);
    if (!buf) iron_oom_abort("iron_string.c:Iron_string_pad_left");
    for (size_t i = 0; i < pad_count; i++) buf[i] = pc;
    memcpy(buf + pad_count, s, slen);
    buf[total] = '\0';
    Iron_String result = iron_string_from_cstr(buf, total);
    free(buf);
    return result;
}

Iron_String Iron_string_pad_right(Iron_String self, int64_t width, Iron_String ch) {
    const char *s    = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *pad  = iron_string_cstr(&ch);
    size_t      plen = iron_string_byte_len(&ch);
    char        pc   = (plen > 0) ? pad[0] : ' ';

    if ((int64_t)slen >= width) return self;
    size_t pad_count = (size_t)width - slen;
    size_t total     = (size_t)width;
    /* FIX-02: replace silent self-return fallback with iron_oom_abort. */
    char *buf = (char *)malloc(total + 1);
    if (!buf) iron_oom_abort("iron_string.c:Iron_string_pad_right");
    memcpy(buf, s, slen);
    for (size_t i = 0; i < pad_count; i++) buf[slen + i] = pc;
    buf[total] = '\0';
    Iron_String result = iron_string_from_cstr(buf, total);
    free(buf);
    return result;
}

/* ── Phase 59 P01c: rindex_of / byte_at / from_byte ─────────────────────── */

/* Rightmost occurrence of `sub` in `self` (byte-level). Returns -1 if not
 * found or if `sub` is empty. Mirrors the semantics of Iron_string_index_of
 * but scans from the end of the string. */
int64_t Iron_string_rindex_of(Iron_String self, Iron_String sub) {
    const char *s    = iron_string_cstr(&self);
    const char *d    = iron_string_cstr(&sub);
    size_t      slen = iron_string_byte_len(&self);
    size_t      dlen = iron_string_byte_len(&sub);
    if (dlen == 0 || dlen > slen) return -1;
    /* Scan right-to-left, returning the first (rightmost) hit. */
    for (size_t i = slen - dlen + 1; i-- > 0; ) {
        if (memcmp(s + i, d, dlen) == 0) return (int64_t)i;
    }
    return -1;
}

/* Byte value at index `i` (0..len-1). Returns -1 for out-of-range indices
 * so callers can branch on the negative result without a separate length
 * check. */
int64_t Iron_string_byte_at(Iron_String self, int64_t i) {
    size_t len = iron_string_byte_len(&self);
    if (i < 0 || (size_t)i >= len) return -1;
    const char *s = iron_string_cstr(&self);
    return (int64_t)(unsigned char)s[i];
}

/* Build a 1-byte string from the low 8 bits of `b`. Caller is responsible
 * for any UTF-8 validity concerns — this is a byte constructor, not a
 * codepoint constructor. */
Iron_String Iron_string_from_byte(int64_t b) {
    char buf[1];
    buf[0] = (char)(b & 0xff);
    return iron_string_from_cstr(buf, 1);
}
