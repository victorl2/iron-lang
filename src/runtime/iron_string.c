#include "iron_runtime.h"
#include <stdatomic.h>   /* string / list refcounts are C11 atomics on every platform */
#include "runtime/iron_panic.h"  /* Phase 19-02: iron_panic_init_from_env */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

/* stb_ds hash map — STB_DS_IMPLEMENTATION is in src/util/stb_ds_impl.c */
#include "vendor/stb_ds.h"

/* A heap string's characters are shared by every copy of the value: the
 * buffer sits behind a header holding an atomic reference count. A copy
 * retains, a drop releases, and the last release frees the block. Interned
 * literals belong to the runtime and are never counted. */
typedef struct { _Atomic uint64_t rc; uint64_t _pad; } IronStrHdr;

static char *iron__str_alloc(size_t byte_len, const char *where) {
    IronStrHdr *h = (IronStrHdr *)malloc(sizeof(IronStrHdr) + byte_len + 1);
    if (!h) iron_oom_abort(where);
    atomic_init(&h->rc, 1);
    h->_pad = 0;
    return (char *)(h + 1);
}

static inline IronStrHdr *iron__str_hdr(const Iron_String *s) {
    return ((IronStrHdr *)s->heap.data) - 1;
}

static inline bool iron__str_counted(const Iron_String *s) {
    return (s->heap.flags & 0x01) && !(s->heap.flags & 0x02);
}

static void iron__str_free(Iron_String *s) {
    free(iron__str_hdr(s));
}

/* ── Intern table ────────────────────────────────────────────────────────── */

/* Key: heap-allocated C string (owned by the table)
 * Value: the interned Iron_String (which points into the same key buffer) */
static struct { char *key; Iron_String value; } *s_intern_table = NULL;

static iron_mutex_t s_intern_lock;
static iron_once_t s_intern_once = IRON_ONCE_INIT;
static void iron__init_intern_lock(void) {
    IRON_MUTEX_INIT(s_intern_lock);
}
static inline void iron__ensure_intern_lock(void) {
    iron_once(&s_intern_once, iron__init_intern_lock);
}

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
        char *buf = iron__str_alloc(byte_len, "iron_string.c:iron_string_from_cstr");
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

/* Lexicographic order by bytes, which for UTF-8 is code point order.
 * Returns <0, 0 or >0 like memcmp. */
int iron_string_compare(const Iron_String *a, const Iron_String *b) {
    size_t la = iron_string_byte_len(a);
    size_t lb = iron_string_byte_len(b);
    int c = memcmp(iron_string_cstr(a), iron_string_cstr(b), la < lb ? la : lb);
    if (c != 0) return c;
    return (la > lb) - (la < lb);
}

Iron_String iron_string_concat(const Iron_String *a, const Iron_String *b) {
    /* Phase 96 STR-01: defense-in-depth NULL guard — the compiler-side
     * lowering at hir_lower.c always emits two valid Iron_String pointers,
     * but partial chains and future generated-call paths may insert NULL
     * for "empty operand" without an extra null check. Treating NULL as
     * empty matches the CONTEXT.md "NULL operand = empty string" decision
     * and keeps the helper crash-free in isolation (test_string_concat.c).
     * The static const is zero-initialised: heap.flags = 0 (SSO variant)
     * with sso.len = 0, which iron_string_byte_len + iron_string_cstr
     * already handle correctly. */
    static const Iron_String iron_string_empty_const;
    if (!a) a = &iron_string_empty_const;
    if (!b) b = &iron_string_empty_const;

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
    char *buf = iron__str_alloc(total, "iron_string.c:iron_string_concat");
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
        if (iron__str_counted(&s)) iron_string_release(&s);
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

void iron_string_retain(const Iron_String *s) {
    if (!s || !iron__str_counted(s)) return;
    (void)IRON_ATOMIC_U64_FETCH_ADD_RELAXED(iron__str_hdr(s)->rc, 1);
}

void iron_string_release(Iron_String *s) {
    if (!s) return;
    if (iron__str_counted(s)) {
        uint64_t prev = IRON_ATOMIC_U64_FETCH_SUB_RELEASE(iron__str_hdr(s)->rc, 1);
        if (prev == 1) {
            IRON_ATOMIC_FENCE_ACQUIRE();
            iron__str_free(s);
        }
    }
    memset(s, 0, sizeof(*s));
}

/* Forward declarations for the thread subsystem (implemented in iron_threads.c) */
void iron_threads_init(void);
void iron_threads_shutdown(void);

/* ── Runtime lifecycle ───────────────────────────────────────────────────── */

/* Stored during iron_runtime_init for later access by iron_os_args() */
static int    s_iron_argc = 0;
static char **s_iron_argv = NULL;

#ifdef _WIN32
int  iron_os_utf8_args(char ***out);            /* iron_os.c */
void iron_os_utf8_args_free(char **v, int n);
#endif

Iron_List_Iron_String iron_runtime_args(void) {
    Iron_List_Iron_String args = Iron_List_Iron_String_create();
#ifdef _WIN32
    char **v = NULL;
    int n = iron_os_utf8_args(&v);
    for (int i = 1; i < n; i++)
        if (v[i]) Iron_List_Iron_String_push(&args, iron_string_from_cstr(v[i], strlen(v[i])));
    iron_os_utf8_args_free(v, n);
#else
    for (int i = 1; i < s_iron_argc; i++)
        if (s_iron_argv && s_iron_argv[i])
            Iron_List_Iron_String_push(&args, iron_string_from_cstr(s_iron_argv[i],
                                                                   strlen(s_iron_argv[i])));
#endif
    return args;
}

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

    /* Phase 19: heap-tracker allocation-id counter (debug builds use it
     * in IronAllocHdr; release builds set it for forward-compat).
     * Atomic init is idempotent across repeated iron_runtime_init calls
     * (unit-test harness pattern) per the existing s_intern_table /
     * iron_threads_init conventions in this file. */
    IRON_ATOMIC_U64_INIT(iron_alloc_id_counter, 0);

#ifdef IRON_DEBUG_ALLOCATOR
    /* Phase 31 GA1 (Plan 31-01): debug allocator init + atexit leak dump.
     * iron_debug_alloc_init() is idempotent (guards its own once-flag). The
     * atexit registration is guarded by a local once-flag so a repeated
     * iron_runtime_init in a unit-test harness does NOT double-register the
     * dump (atexit has no deregister; double-registering would print twice).
     * The DBG-07 release opt-in (#else) lands in Plan 31-03; leave a marker.
     *   Plan 31-03: iron_leakcheck_init_from_env()   (IRON_LEAK_CHECK opt-in) */
    iron_debug_alloc_init();
    {
        static bool s_leak_dump_registered = false;
        if (!s_leak_dump_registered) {
            atexit(iron_leak_dump);
            s_leak_dump_registered = true;
        }
    }
#else
    /* Phase 31 DBG-07 (Plan 31-03): release-build opt-in leak check. Reads
     * IRON_LEAK_CHECK once and, only when set to "1", arms the side-table
     * (src/runtime/iron_leakcheck.c) + registers atexit(iron_leakcheck_dump).
     * When unset (the default) this does nothing — no lock, no atexit, and the
     * alloc/free hooks early-return at zero cost so the release binary is
     * byte-for-byte behaviourally unchanged. No poison in release. */
    iron_leakcheck_init_from_env();
#endif

    /* Phase 19-02: cache IRON_PANIC_FORMAT env variable BEFORE any
     * allocation can panic (Pitfall 6: getenv-once-at-init). Idempotent
     * across repeated iron_runtime_init calls. Must come AFTER alloc-id
     * init since panic formatting may reference the alloc-id field on
     * stale-deref. */
    iron_panic_init_from_env();

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

#ifdef IRON_DEBUG_ALLOCATOR
    /* Release the blocks the debug allocator retains for its poison /
     * free-site guarantees, so they are not left behind as reachable garbage. */
    iron_debug_quarantine_drain();
#endif

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
            iron__str_free(v);
        }
    }
    shfree(s_intern_table);
    s_intern_table = NULL;
    IRON_MUTEX_UNLOCK(s_intern_lock);
}

/* ── String built-in methods (Phase 38) ─────────────────────────────────── */

/* ── UTF-8 helpers: String positions count code points ─────────────────── */

/* Length of the UTF-8 sequence led by byte c (an invalid or continuation
 * byte counts as a 1-byte unit, so malformed input never stalls a walk). */
static size_t utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if (c >= 0xC0 && c < 0xE0) return 2;
    if (c >= 0xE0 && c < 0xF0) return 3;
    if (c >= 0xF0 && c < 0xF8) return 4;
    return 1;
}

/* Byte offset of code point `cp` (clamped to [0, len]). */
static size_t utf8_cp_to_byte(const char *s, size_t len, int64_t cp) {
    size_t b = 0;
    while (cp > 0 && b < len) {
        size_t n = utf8_seq_len((unsigned char)s[b]);
        b = (b + n > len) ? len : b + n;
        cp--;
    }
    return b;
}

/* Number of code points in the first `bytes` bytes. */
static int64_t utf8_byte_to_cp(const char *s, size_t bytes) {
    int64_t cp = 0;
    size_t b = 0;
    while (b < bytes) {
        b += utf8_seq_len((unsigned char)s[b]);
        cp++;
    }
    return cp;
}

/* Decode the code point at s[*b] and advance *b; malformed bytes decode
 * as themselves. */
static uint32_t utf8_decode(const char *s, size_t len, size_t *b) {
    unsigned char c = (unsigned char)s[*b];
    size_t n = utf8_seq_len(c);
    if (n == 1 || *b + n > len) { (*b)++; return c; }
    uint32_t cp = (n == 2) ? (c & 0x1Fu) : (n == 3) ? (c & 0x0Fu) : (c & 0x07u);
    for (size_t k = 1; k < n; k++) {
        unsigned char cc = (unsigned char)s[*b + k];
        if ((cc & 0xC0) != 0x80) { (*b)++; return c; }
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    *b += n;
    return cp;
}

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Simple (one-to-one) case mapping for ASCII, Latin-1, Latin Extended-A,
 * Greek and Cyrillic. Characters outside those blocks are unchanged. */
static uint32_t cp_to_upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c < 0x80) return c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 0x20;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c % 2 == 0) ? c - 1 : c;
        if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
        return (c % 2 == 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 0x20;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;
    return c;
}

static uint32_t cp_to_lower(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c % 2 == 1) ? c + 1 : c;
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
        return (c % 2 == 0) ? c + 1 : c;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 0x20;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    return c;
}

static Iron_String map_case(Iron_String self, uint32_t (*map)(uint32_t),
                            const char *who) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    /* Mapped characters in these blocks keep their UTF-8 length, except
     * y-diaeresis (1 -> 2 bytes); 2x plus 4 bounds every case. */
    char *buf = (char *)malloc(len * 2 + 4);
    if (!buf) iron_oom_abort(who);
    size_t b = 0, o = 0;
    while (b < len) {
        size_t before = b;
        uint32_t cp = utf8_decode(s, len, &b);
        uint32_t m = map(cp);
        if (m == cp) {
            memcpy(buf + o, s + before, b - before);
            o += b - before;
        } else {
            o += utf8_encode(m, buf + o);
        }
    }
    buf[o] = '\0';
    Iron_String result = iron_string_from_cstr(buf, o);
    free(buf);
    return result;
}

Iron_String Iron_string_upper(Iron_String self) {
    return map_case(self, cp_to_upper, "iron_string.c:Iron_string_upper");
}

Iron_String Iron_string_lower(Iron_String self) {
    return map_case(self, cp_to_lower, "iron_string.c:Iron_string_lower");
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
    return utf8_byte_to_cp(s, (size_t)(hit - s));
}

/* The character (code point) at position i, or "" when out of range. */
Iron_String Iron_string_char_at(Iron_String self, int64_t i) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    if (i < 0) return iron_string_from_cstr("", 0);
    size_t b = utf8_cp_to_byte(s, len, i);
    if (b >= len) return iron_string_from_cstr("", 0);
    size_t n = utf8_seq_len((unsigned char)s[b]);
    if (b + n > len) n = len - b;
    return iron_string_from_cstr(s + b, n);
}

/* Number of characters (code points). */
int64_t Iron_string_len(Iron_String self) {
    return (int64_t)iron_string_codepoint_count(&self);
}

/* Size in bytes of the UTF-8 encoding. */
int64_t Iron_string_byte_len(Iron_String self) {
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

/* Each character (code point) as its own String. */
Iron_List_Iron_String Iron_string_chars(Iron_String self) {
    Iron_List_Iron_String result = Iron_List_Iron_String_create();
    const char *s    = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    size_t b = 0;
    while (b < slen) {
        size_t n = utf8_seq_len((unsigned char)s[b]);
        if (b + n > slen) n = slen - b;
        Iron_List_Iron_String_push(&result, iron_string_from_cstr(s + b, n));
        b += n;
    }
    return result;
}

Iron_List_Iron_String Iron_string_split(Iron_String self, Iron_String sep) {
    Iron_List_Iron_String result = Iron_List_Iron_String_create();
    const char *s    = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *d    = iron_string_cstr(&sep);
    size_t      dlen = iron_string_byte_len(&sep);

    if (dlen == 0) {
        /* empty separator: each character is its own element */
        Iron_List_Iron_String_free(&result);
        return Iron_string_chars(self);
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

    if (oldlen == 0) { iron_string_retain(&self); return self; } /* no-op: empty old pattern */

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

/* Characters [start, end_idx) by code point position, clamped. */
Iron_String Iron_string_substring(Iron_String self, int64_t start, int64_t end_idx) {
    const char *s   = iron_string_cstr(&self);
    size_t      len = iron_string_byte_len(&self);
    if (start < 0) start = 0;
    if (end_idx < start) end_idx = start;
    size_t bs = utf8_cp_to_byte(s, len, start);
    size_t be = utf8_cp_to_byte(s, len, end_idx);
    return iron_string_from_cstr(s + bs, be - bs);
}

/* Only surrounding whitespace may follow a number: "42 " and " 42\n" are
 * 42, while "3x", "4 5" and "" are not numbers (the manual: 0 when not a
 * number). An Int that does not fit in 64 bits is not a number either. */
static bool iron_rest_is_space(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f') p++;
    return *p == '\0';
}

int64_t Iron_string_to_int(Iron_String self) {
    const char *s = iron_string_cstr(&self);
    char *end;
    errno = 0;
    int64_t v = (int64_t)strtoll(s, &end, 10);
    if (end == s || errno == ERANGE || !iron_rest_is_space(end)) return 0;
    return v;
}

double Iron_string_to_float(Iron_String self) {
    const char *s = iron_string_cstr(&self);
    char *end;
    double v = strtod(s, &end);
    if (end == s || !iron_rest_is_space(end)) return 0.0;
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
    /* width counts characters; the pad is the first character of ch. */
    const char *s    = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *pad  = iron_string_cstr(&ch);
    size_t      plen = iron_string_byte_len(&ch);
    size_t      pn   = plen > 0 ? utf8_seq_len((unsigned char)pad[0]) : 1;
    if (pn > plen && plen > 0) pn = plen;
    const char *pc   = plen > 0 ? pad : " ";

    int64_t chars = (int64_t)iron_string_codepoint_count(&self);
    /* The result is owned by the caller: hand out a reference of its own. */
    if (chars >= width) { iron_string_retain(&self); return self; }
    size_t pad_count = (size_t)(width - chars);
    size_t total     = slen + pad_count * pn;
    char *buf = (char *)malloc(total + 1);
    if (!buf) iron_oom_abort("iron_string.c:Iron_string_pad_left");
    size_t o = 0;
    for (size_t i = 0; i < pad_count; i++) { memcpy(buf + o, pc, pn); o += pn; }
    memcpy(buf + o, s, slen); o += slen;
    buf[o] = '\0';
    Iron_String result = iron_string_from_cstr(buf, o);
    free(buf);
    return result;
}

Iron_String Iron_string_pad_right(Iron_String self, int64_t width, Iron_String ch) {
    /* width counts characters; the pad is the first character of ch. */
    const char *s    = iron_string_cstr(&self);
    size_t      slen = iron_string_byte_len(&self);
    const char *pad  = iron_string_cstr(&ch);
    size_t      plen = iron_string_byte_len(&ch);
    size_t      pn   = plen > 0 ? utf8_seq_len((unsigned char)pad[0]) : 1;
    if (pn > plen && plen > 0) pn = plen;
    const char *pc   = plen > 0 ? pad : " ";

    int64_t chars = (int64_t)iron_string_codepoint_count(&self);
    /* The result is owned by the caller: hand out a reference of its own. */
    if (chars >= width) { iron_string_retain(&self); return self; }
    size_t pad_count = (size_t)(width - chars);
    size_t total     = slen + pad_count * pn;
    char *buf = (char *)malloc(total + 1);
    if (!buf) iron_oom_abort("iron_string.c:Iron_string_pad_right");
    size_t o = 0;
    memcpy(buf + o, s, slen); o += slen;
    for (size_t i = 0; i < pad_count; i++) { memcpy(buf + o, pc, pn); o += pn; }
    buf[o] = '\0';
    Iron_String result = iron_string_from_cstr(buf, o);
    free(buf);
    return result;
}

/* ── Phase 59 P01c: rindex_of / byte_at / from_byte ─────────────────────── */

/* Character position of the rightmost occurrence of `sub` in `self`, or -1.
 * An empty `sub` occurs at every position, the last being the end of the
 * string, as index_of("") finds it at 0. */
int64_t Iron_string_rindex_of(Iron_String self, Iron_String sub) {
    const char *s    = iron_string_cstr(&self);
    const char *d    = iron_string_cstr(&sub);
    size_t      slen = iron_string_byte_len(&self);
    size_t      dlen = iron_string_byte_len(&sub);
    if (dlen == 0) return utf8_byte_to_cp(s, slen);
    if (dlen > slen) return -1;
    /* Scan right-to-left, returning the first (rightmost) hit. */
    for (size_t i = slen - dlen + 1; i-- > 0; ) {
        if (memcmp(s + i, d, dlen) == 0) return utf8_byte_to_cp(s, i);
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

/* `s.release()`: strings are freed with their owners (#182); kept for
 * source compatibility and does nothing. */
void Iron_string_release(Iron_String self) {
    (void)self;
}
