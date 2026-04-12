# Audit: Runtime + Stdlib + Infrastructure

Scope: `src/runtime/`, `src/stdlib/`, `src/cli/`, `src/diagnostics/`, `src/pkg/`, `src/util/`

**Total lines audited (runtime + stdlib):** 5,102 lines across 20 files

---

## 1. Blind Casts (AUDIT-01)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | iron_runtime.h:59 | L | `(iron__win_trampoline_t *)p` in Win32 thread proc -- safe, controlled by IRON_THREAD_CREATE caller | Document precondition | test_blind_cast_thread_trampoline |
| 2 | iron_runtime.h:536 | L | `memcpy(&map_fn, &f.fn, sizeof(map_fn))` in IRON_LIST_COLL_IMPL -- type-punning through memcpy to avoid -Wcast-function-type; technically safe but assumes closure fn ABI matches MapFn/FilterFn/ReduceFn | Assert sizeof(fn) == sizeof(map_fn) at call site | test_blind_cast_closure_fn_pun |
| 3 | iron_rc.c:20 | L | `(char *)ctrl + sizeof(Iron_RcControl)` to access value stored after control block -- relies on single-allocation layout; correct but not documented with a static_assert | Add _Static_assert or offsetof check | test_blind_cast_rc_value_layout |
| 4 | iron_rc.c:74 | L | Same pointer arithmetic in iron_weak_upgrade -- `(char *)ctrl + sizeof(Iron_RcControl)` | Same as above | test_blind_cast_rc_weak_upgrade |
| 5 | iron_threads.c:149 | L | `(Iron_Pool *)arg` in pool_worker -- always called with pool pointer, safe | Document precondition | test_blind_cast_pool_worker |
| 6 | iron_threads.c:420 | L | `(Iron_Pool *)arg` in pool_worker_elastic -- same pattern as fixed worker | Document precondition | test_blind_cast_elastic_worker |
| 7 | iron_threads.c:667 | L | `(HandleWrapper *)arg` in handle_thread_fn -- controlled by Iron_handle_create | Document precondition | test_blind_cast_handle_wrapper |
| 8 | iron_net.c:594 | M | `(uint8_t *)(uintptr_t)base` in Iron_tcpsocket_read -- casts away const from iron_string_cstr() return to pass to recv buffer; **writes into immutable Iron_String data** | Use a local buffer and copy back, or document the API contract that buf must be mutable | test_blind_cast_recv_into_string |
| 9 | iron_net.c:744 | L | `(const uint8_t *)iron_string_cstr(&a.bytes)` -- safe, read-only access to IPv6 octets stored in Iron_String | None needed | test_blind_cast_ipv6_bytes |
| 10 | iron_string.c:15 | L | stb_ds hash map `s_intern_table` uses `char *key` and `Iron_String value` struct -- stb_ds internally casts entries; safe by construction | None needed | test_blind_cast_intern_stbds |
| 11 | iron_collections.c:35-56 | L | IRON_LIST_IMPL / IRON_MAP_IMPL / IRON_SET_IMPL macro expansions contain `(T *)malloc(...)` and `(T *)realloc(...)` casts -- standard C allocation pattern; safe | None needed | test_blind_cast_collection_alloc |

**Runtime + Stdlib subtotal:** 0H, 1M, 10L = 11 findings

---

## 2. Enum Switch Exhaustiveness (AUDIT-02)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | iron_net.c:78-100 | M | `iron_net_translate_wsa` switch on WSA error codes -- has `default` catch-all mapping to IRON_ERR_NET_UNKNOWN; safe but may mask novel errors silently | Add logging in default case for debug builds | test_enum_switch_wsa_translate |
| 2 | iron_net.c:102-131 | M | `iron_net_translate_errno` switch on errno values -- same pattern with `default: IRON_ERR_NET_UNKNOWN` | Add logging in default case for debug builds | test_enum_switch_errno_translate |
| 3 | iron_log.c:8 | L | `s_log_level` is Iron_LogLevel enum but comparison uses `<` operator (level < s_log_level) -- works because enum values are sequential integers; no switch at all | Add comment documenting sequential assumption | test_enum_switch_log_level |
| 4 | iron_threads.c:48-51 | L | `iron_cond_timedwait_ms` Win32 path returns based on GetLastError() == ERROR_TIMEOUT -- only two paths (OK/EXPIRED), no switch on the return code | None needed | test_enum_switch_timedwait_win |
| 5 | iron_threads.c:66-70 | L | `iron_cond_timedwait_ms` POSIX path switches on pthread_cond_timedwait return -- checks 0 and ETIMEDOUT, defaults to ERROR; complete for practical purposes | None needed | test_enum_switch_timedwait_posix |
| 6 | iron_runtime.h:244-248 | L | Iron_Address_Tag enum has exactly 2 values (V4=0, V6=1) -- any switch in iron_net.c DNS code handles both; no missing case | None needed | test_enum_switch_address_tag |

**Runtime + Stdlib subtotal:** 0H, 2M, 4L = 6 findings

---

## 3. Null Safety (AUDIT-03)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | iron_string.c:81 | M | `memcpy(buf, cstr, byte_len)` in heap path of iron_string_from_cstr -- `cstr` is not NULL-checked before memcpy (only checked for SSO path at line 66) | Add `if (cstr)` guard before memcpy | test_null_safety_string_from_null |
| 2 | iron_string.c:161-162 | M | `iron_string_cstr(&s)` in iron_string_intern -- returns pointer to local `s` which is passed by value; if s is zeroed out, cstr returns sso.data (empty), safe | None needed (false positive) | test_null_safety_intern_empty |
| 3 | iron_string.c:85 | M | `count_codepoints(cstr, byte_len)` in heap path -- if `cstr` is NULL and byte_len > IRON_STRING_SSO_MAX, NULL is passed to count_codepoints which dereferences `data[i]` | Guard with `cstr ? count_codepoints(cstr, byte_len) : 0` | test_null_safety_codepoint_null |
| 4 | iron_builtins.c:9 | L | `iron_string_cstr(&s)` in Iron_print -- if s has zeroed heap.flags, returns sso.data which is valid; safe by construction | None needed | test_null_safety_print |
| 5 | iron_threads.c:132-133 | M | `pool_queue_grow` malloc failure returns silently; caller at line 249 proceeds to write `pool->queue[pool->queue_tail]` which may now be NULL if original queue was freed | Return error or abort on OOM in pool_queue_grow; alternatively check `pool->queue` before write | test_null_safety_pool_queue_grow |
| 6 | iron_threads.c:700 | M | IRON_THREAD_CREATE in Iron_handle_create -- return value is unchecked; if pthread_create fails, `h->thread` is uninitialized and Iron_handle_wait will join garbage | Check IRON_THREAD_CREATE return value | test_null_safety_handle_create_thread |
| 7 | iron_threads.c:756 | M | Same unchecked IRON_THREAD_CREATE in iron_handle_create_self_ref | Same fix | test_null_safety_handle_selfref_thread |
| 8 | iron_threads.c:237 | M | IRON_THREAD_CREATE in pool_create loop -- return value unchecked; if one thread fails to create, pool has fewer workers than expected | Check return value and handle partial creation | test_null_safety_pool_create_thread |
| 9 | iron_threads.c:414 | M | IRON_THREAD_CREATE in pool_spawn_elastic_worker_locked -- unchecked return; pool->thread_count already incremented at line 411 | Check return and rollback thread_count on failure | test_null_safety_elastic_spawn |
| 10 | iron_rc.c:36 | L | `IRON_ATOMIC_FETCH_SUB(rc->ctrl->strong_count, 1)` -- if prev is already 0, strong_count goes negative (underflow on atomic_int); caller must guarantee correct retain/release pairing | Add assert(prev > 0) in debug builds | test_null_safety_rc_double_release |
| 11 | iron_io.c:206-207 | L | `strrchr(p, '/')` in Iron_io_basename -- no Windows backslash handling; on Windows paths with `\` only, returns the entire path | Add `\\` check for Windows | test_null_safety_basename_backslash |
| 12 | iron_io.c:181-183 | L | Iron_io_read_line uses fixed 4096-byte stack buffer -- lines longer than 4095 chars are silently truncated | Document or use dynamic allocation | test_null_safety_readline_long |
| 13 | iron_net.c:229-231 | L | iron_string_cstr(&host) with NULL check and fallback to "" -- properly handled | None needed | test_null_safety_net_listen_host |
| 14 | iron_net.c:310-311 | L | Same host NULL check in Iron_net_tcp_dial -- properly handled | None needed | test_null_safety_net_dial_host |
| 15 | iron_net_init.c:49-53 | M | `iron_net_ensure_wsa_lock` has a TOCTOU race: if two threads call simultaneously before `s_wsa_lock_init` is set, both may call IRON_MUTEX_INIT | Use INIT_ONCE / pthread_once (as comment notes, single init is assumed) | test_null_safety_wsa_init_race |
| 16 | iron_math.c:54 | L | `__thread` TLS for s_math_rng -- if destructor accesses TLS after thread exit, UB; safe in practice since RNG is never accessed after thread exit | None needed | test_null_safety_math_rng_tls |
| 17 | iron_log.c:36 | L | `isatty(STDERR_FILENO)` -- safe; always returns 0 or 1 | None needed | test_null_safety_log_isatty |
| 18 | iron_runtime.h:497 | H | IRON_LIST_IMPL `_push` calls `realloc` but **does not check for NULL return** -- if realloc fails, `self->items` becomes NULL and the next line `self->items[self->count++] = item` is a NULL dereference | Check realloc return and abort or return error | test_null_safety_list_push_oom |
| 19 | iron_runtime.h:640-641 | H | IRON_MAP_IMPL `_put` calls `realloc` for both keys and values without NULL check -- same NULL deref risk on OOM | Check realloc returns | test_null_safety_map_put_oom |
| 20 | iron_runtime.h:487-488 | M | IRON_LIST_IMPL `_clone` calls `malloc` without NULL check -- if malloc fails, `dst.items` is NULL and memcpy dereferences NULL | Check malloc return | test_null_safety_list_clone_oom |
| 21 | iron_runtime.h:624-625 | M | IRON_MAP_IMPL `_clone` calls `malloc` for keys and values without NULL check | Check malloc returns | test_null_safety_map_clone_oom |
| 22 | iron_runtime.h:502 | L | IRON_LIST_IMPL `_get` does not bounds-check `index` -- `self->items[index]` is UB if index >= count or < 0 | Add bounds check | test_null_safety_list_get_oob |
| 23 | iron_runtime.h:505 | L | IRON_LIST_IMPL `_set` does not bounds-check -- same issue | Add bounds check | test_null_safety_list_set_oob |
| 24 | iron_runtime.h:508 | L | IRON_LIST_IMPL `_pop` does not check `self->count > 0` -- underflow dereferences `self->items[-1]` | Add count > 0 check | test_null_safety_list_pop_empty |

**Runtime + Stdlib subtotal:** 2H, 8M, 14L = 24 findings

---

## 4. Arena Lifetimes (AUDIT-04)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | iron_string.c:72-79 | M | OOM fallback in iron_string_from_cstr silently truncates to SSO_MAX bytes -- caller has no way to detect truncation; data loss without error | Return an error indicator or set a flag | test_arena_string_oom_truncation |
| 2 | iron_string.c:172-176 | M | iron_string_intern frees input string's heap.data if already interned -- correct ownership transfer, but if caller retains a pointer to s.heap.data after interning, it's a use-after-free | Document that intern consumes the input string's ownership | test_arena_intern_uaf |
| 3 | iron_string.c:185-188 | L | Interned heap strings share key ownership with stb_ds -- stb_ds `shput` calls strdup internally for the key; the interned Iron_String's heap.data points to the original allocation (not the key copy) | Document ownership model | test_arena_intern_key_ownership |
| 4 | iron_string.c:244-252 | M | iron_runtime_shutdown frees interned string heap.data then calls shfree -- stb_ds also frees the key copies; if any key was aliased to a value's data pointer, double-free is possible (unlikely with current shput semantics) | Verify shput always strdup's keys independently | test_arena_shutdown_double_free |
| 5 | iron_rc.c:33-48 | M | iron_rc_release: when last strong ref drops and weak_count > 0, ctrl block is kept alive but value's destructor is called -- the weak ref still points into the ctrl allocation which now has a destroyed value; upgrading after this correctly returns NULL but the ctrl block leaks if all weak refs are dropped without explicit cleanup | Add weak-count decrement to free ctrl when weak refs are released (need iron_weak_release API) | test_arena_rc_ctrl_leak |
| 6 | iron_rc.c:57-58 | L | iron_rc_downgrade increments weak_count but there is no iron_weak_release function to decrement it -- weak refs cannot be explicitly dropped, so ctrl blocks for objects that had weak refs are permanently leaked | Add Iron_weak_release that decrements weak_count and frees ctrl when both counts reach 0 | test_arena_weak_release_missing |

**Runtime + Stdlib subtotal:** 0H, 4M, 2L = 6 findings

---

## 5. Integer Safety (AUDIT-05)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | iron_string.c:130 | M | `total = la + lb` in iron_string_concat -- no overflow check; two strings > 2GB each could wrap size_t on 32-bit | Add overflow check: `if (la > SIZE_MAX - lb)` return empty | test_int_safety_concat_overflow |
| 2 | iron_string.c:478 | M | `total = len * (size_t)n` in Iron_string_repeat -- if `n` is very large, multiplication wraps without check | Add overflow check before multiplication | test_int_safety_repeat_overflow |
| 3 | iron_string.c:421 | M | `total = slen + count * (newlen - oldlen)` in Iron_string_replace -- if newlen < oldlen, the subtraction is unsigned and wraps to a huge number; if count is large, product can overflow | Use signed arithmetic or bounds-check the product | test_int_safety_replace_overflow |
| 4 | iron_string.c:69 | L | `(uint8_t)byte_len` in SSO path -- byte_len is guaranteed <= IRON_STRING_SSO_MAX (23), so cast is safe | None needed | test_int_safety_sso_len_cast |
| 5 | iron_string.c:84 | L | `(uint32_t)byte_len` for heap byte_length -- truncates if byte_len > 4GB; Iron_String can't represent strings > 4GB | Document 4GB string limit | test_int_safety_heap_len_cap |
| 6 | iron_runtime.h:497 | M | IRON_LIST_IMPL `_push` doubles capacity via `self->capacity * 2` -- if capacity exceeds INT64_MAX/2, multiplication overflows int64_t | Add saturation check before doubling | test_int_safety_list_capacity_overflow |
| 7 | iron_runtime.h:640 | M | IRON_MAP_IMPL `_put` same capacity doubling pattern | Same fix | test_int_safety_map_capacity_overflow |
| 8 | iron_net.c:494-497 | L | `recv` returns `int` on Win32 / `ssize_t` on POSIX -- cast to `int64_t` is safe for positive values | None needed | test_int_safety_recv_cast |
| 9 | iron_net.c:546-548 | L | `send` same pattern -- safe | None needed | test_int_safety_send_cast |
| 10 | iron_net.c:593-594 | M | `(int64_t)iron_string_byte_len(&buf)` used as recv capacity -- iron_string_byte_len returns size_t which can exceed INT_MAX on 64-bit; passing to recv as int (Win32) truncates | Cast to int with bounds check on Win32 path | test_int_safety_recv_cap_truncate |
| 11 | iron_math.c:41 | L | `Iron_rng_next(rng) % range` -- modulo bias when range is not a power of 2; standard PRNG limitation | Use rejection sampling for uniform distribution | test_int_safety_rng_modulo_bias |
| 12 | iron_time.c:25 | L | `(int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec` in now_ns -- overflows int64_t after ~292 years of uptime; safe in practice | Document theoretical overflow limit | test_int_safety_nowns_overflow |
| 13 | iron_threads.c:131-132 | L | `pool_queue_grow` doubles capacity via `new_cap = pool->queue_capacity * 2` -- int overflow if queue_capacity > INT_MAX/2 | Add overflow check | test_int_safety_queue_grow_overflow |
| 14 | iron_io.c:230 | L | `alen + 1 + blen + 1` in join_path -- safe in practice for file paths but technically can overflow size_t for pathological inputs | Add overflow check | test_int_safety_joinpath_overflow |

**Runtime + Stdlib subtotal:** 0H, 6M, 8L = 14 findings

---

## 6. Allocation Error Handling (AUDIT-06)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | iron_string.c:72-79 | M | iron_string_from_cstr heap malloc fails: falls back to SSO truncation -- silent data loss | Return error or abort | test_alloc_string_from_cstr_oom |
| 2 | iron_string.c:143-144 | M | iron_string_concat malloc fails: returns empty string -- silent data loss | Abort or propagate error | test_alloc_concat_oom |
| 3 | iron_string.c:261 | M | Iron_string_upper malloc fails: returns empty string | Return self or abort | test_alloc_upper_oom |
| 4 | iron_string.c:271 | M | Iron_string_lower malloc fails: returns empty string | Same | test_alloc_lower_oom |
| 5 | iron_string.c:391 | M | Iron_string_join malloc fails: returns empty string | Abort or propagate error | test_alloc_join_oom |
| 6 | iron_string.c:422-423 | M | Iron_string_replace malloc fails: returns self (original) | Document return-self-on-OOM | test_alloc_replace_oom |
| 7 | iron_string.c:479-480 | M | Iron_string_repeat malloc fails: returns empty string | Abort or propagate error | test_alloc_repeat_oom |
| 8 | iron_string.c:499-500 | M | Iron_string_pad_left malloc fails: returns self | Document | test_alloc_pad_left_oom |
| 9 | iron_string.c:520-521 | M | Iron_string_pad_right malloc fails: returns self | Document | test_alloc_pad_right_oom |
| 10 | iron_threads.c:132-133 | M | pool_queue_grow malloc fails: **silently drops expansion**; next enqueue writes to possibly-freed queue | Return error flag; abort if queue is critical | test_alloc_pool_queue_grow_oom |
| 11 | iron_threads.c:199-200 | L | Iron_pool_create malloc of pool: returns NULL -- **callers (iron_threads_init) do not check** | Check return in iron_threads_init | test_alloc_pool_create_oom |
| 12 | iron_threads.c:520-521 | L | Iron_elastic_pool_create malloc of pool: returns NULL -- same caller issue | Same | test_alloc_elastic_create_oom |
| 13 | iron_threads.c:585 | L | Iron_poolwait_create calloc: returns NULL -- callers check | None needed | test_alloc_poolwait_create |
| 14 | iron_threads.c:682 | L | Iron_handle_create malloc: returns NULL -- correct | None needed | test_alloc_handle_create |
| 15 | iron_threads.c:691-693 | L | HandleWrapper malloc in Iron_handle_create: returns NULL after freeing h -- correct | None needed | test_alloc_handle_wrapper |
| 16 | iron_threads.c:776-777 | L | Iron_channel_create malloc: returns NULL -- correct | None needed | test_alloc_channel_create |
| 17 | iron_threads.c:779-782 | L | Iron_channel_create ring malloc: frees ch and returns NULL -- correct | None needed | test_alloc_channel_ring |
| 18 | iron_threads.c:867-868 | L | Iron_mutex_create malloc: returns NULL -- correct | None needed | test_alloc_mutex_create |
| 19 | iron_threads.c:870-873 | L | Iron_mutex_create value malloc: frees m and returns NULL -- correct | None needed | test_alloc_mutex_value |
| 20 | iron_threads.c:332-333 | M | Iron_pool_destroy elastic snapshot malloc: if NULL, joins are skipped entirely -- leaked threads never joined | Fall back to iterating slots directly under lock | test_alloc_pool_destroy_snapshot_oom |
| 21 | iron_rc.c:10-12 | L | iron_rc_create malloc: returns {NULL, NULL} -- correct | None needed | test_alloc_rc_create |
| 22 | iron_runtime.h:497 | H | IRON_LIST_IMPL `_push` realloc: **NULL return not checked** -- writes to NULL pointer | Check return | test_alloc_list_push_oom |
| 23 | iron_runtime.h:479 | M | IRON_LIST_IMPL `_create_with_capacity` malloc: if fails, l.items=NULL but l.capacity=cap -- capacity is non-zero but items is NULL; next push reallocs from NULL which works on some platforms but is UB | Set capacity=0 on malloc failure | test_alloc_list_create_cap_oom |
| 24 | iron_runtime.h:487 | M | IRON_LIST_IMPL `_clone` malloc: unchecked; memcpy into NULL | Check return | test_alloc_list_clone_oom |
| 25 | iron_runtime.h:615-616 | M | IRON_MAP_IMPL `_create_with_capacity` malloc for keys/values: if one fails and other succeeds, asymmetric state | Check both and clean up on failure | test_alloc_map_create_cap_oom |
| 26 | iron_runtime.h:624-625 | M | IRON_MAP_IMPL `_clone` malloc: unchecked for both keys and values | Check returns | test_alloc_map_clone_oom |
| 27 | iron_runtime.h:640-641 | H | IRON_MAP_IMPL `_put` realloc for keys and values: **neither checked** -- NULL deref on OOM | Check returns | test_alloc_map_put_oom |
| 28 | iron_io.c:39-44 | L | Iron_io_read_file_result malloc: checked, returns error tuple -- correct | None needed | test_alloc_io_read_oom |
| 29 | iron_io.c:135-140 | L | Iron_io_list_files_result initial malloc: checked -- correct | None needed | test_alloc_io_list_oom |
| 30 | iron_io.c:156-161 | L | Iron_io_list_files_result realloc: checked, cleans up on fail -- correct | None needed | test_alloc_io_list_realloc_oom |
| 31 | iron_io.c:231 | M | Iron_io_join_path malloc: checked, returns empty on OOM -- silent data loss but acceptable for path ops | None needed (acceptable) | test_alloc_joinpath_oom |
| 32 | iron_threads.c:922-923 | M | iron_threads_init does not check Iron_pool_create return; Iron_global_pool and Iron_io_pool could be NULL if malloc fails | Check return and either abort or skip pool-dependent features | test_alloc_threads_init_oom |

**Runtime + Stdlib subtotal:** 2H, 16M, 14L = 32 findings

---

## 7. Cross-Platform (AUDIT-08)

| # | File:Line | Severity | Description | Suggested Fix | Regression Fixture |
|---|-----------|----------|-------------|---------------|-------------------|
| 1 | iron_string.c:29 | L | `#include <pthread.h>` inside `#else` of `#ifdef _WIN32` -- correct POSIX-only include | None needed | test_xplat_string_pthread |
| 2 | iron_string.c:18-36 | L | Win32 path uses `INIT_ONCE` / POSIX uses `pthread_once` for intern lock -- correctly gated | None needed | test_xplat_intern_once |
| 3 | iron_threads.c:18-24 | L | Conditional includes: Win32 gets `<windows.h>`, POSIX gets `<unistd.h>`, `<time.h>`, `<errno.h>` -- correct | None needed | test_xplat_threads_includes |
| 4 | iron_threads.c:29-39 | L | Iron_monotonic_now_ms: GetTickCount64 (Win32) vs clock_gettime(CLOCK_MONOTONIC) (POSIX) -- correctly gated | None needed | test_xplat_monotonic |
| 5 | iron_threads.c:44-71 | L | iron_cond_timedwait_ms: SleepConditionVariableCS (Win32) vs pthread_cond_timedwait (POSIX) -- correctly gated | None needed | test_xplat_timedwait |
| 6 | iron_threads.c:53-71 | M | POSIX timedwait uses CLOCK_REALTIME -- can jump during NTP adjustment causing spurious timeout/hang; documented caveat | Upgrade to CLOCK_MONOTONIC via pthread_condattr_setclock (when supported) | test_xplat_timedwait_clock |
| 7 | iron_threads.c:458 | L | `pthread_self()` inside `#ifndef _WIN32` -- correctly gated | None needed | test_xplat_elastic_self |
| 8 | iron_threads.c:462-484 | M | Elastic worker self-retire: Win32 path matches "first non-zero slot" because GetCurrentThread() returns pseudo-handle -- correct but fragile if two workers retire simultaneously | Use GetCurrentThreadId() and store DWORD IDs alongside HANDLEs | test_xplat_elastic_retire_race |
| 9 | iron_threads.c:915-921 | L | iron_threads_init: GetSystemInfo (Win32) vs sysconf(_SC_NPROCESSORS_ONLN) (POSIX) for CPU count -- correctly gated | None needed | test_xplat_cpu_count |
| 10 | iron_net_init.c:32-81 | L | WSAStartup/WSACleanup inside `#ifdef _WIN32`, no-ops on POSIX -- correctly gated | None needed | test_xplat_wsa_init |
| 11 | iron_net_init.c:95-107 | L | SIGPIPE ignore inside `#ifndef _WIN32` -- correctly gated | None needed | test_xplat_sigpipe |
| 12 | iron_net.c:31-69 | L | Platform-specific socket includes and type aliases (SOCKET vs int, closesocket vs close) -- correctly gated with `#ifdef _WIN32` | None needed | test_xplat_net_platform_types |
| 13 | iron_net.c:155-168 | L | iron_net_set_nonblocking: ioctlsocket(FIONBIO) vs fcntl(O_NONBLOCK) -- correctly gated | None needed | test_xplat_net_nonblocking |
| 14 | iron_net.c:176-196 | L | iron_net_poll: WSAPoll vs poll -- correctly gated; POSIX retries EINTR | None needed | test_xplat_net_poll |
| 15 | iron_io.c:5 | H | `#include <sys/stat.h>` -- available on both platforms but `mkdir(p, 0755)` at line 106 is **POSIX-only**; Windows needs `_mkdir(p)` from `<direct.h>` | Add `#ifdef _WIN32` with `_mkdir` | test_xplat_io_mkdir |
| 16 | iron_io.c:6 | H | `#include <dirent.h>` -- **not available on Windows**; Iron_io_list_files_result uses opendir/readdir/closedir | Add Win32 FindFirstFile/FindNextFile/FindClose path | test_xplat_io_dirent |
| 17 | iron_io.c:100 | M | `stat(p, &st)` -- available on Windows via `<sys/stat.h>` but `S_ISDIR` macro may not be defined on MSVC | Add `_S_IFDIR` fallback for MSVC | test_xplat_io_stat_isdir |
| 18 | iron_io.c:206 | M | `strrchr(p, '/')` in basename/dirname -- Windows uses both `/` and `\\`; only `/` is checked | Search for both separators | test_xplat_io_path_separator |
| 19 | iron_log.c:4 | H | `#include <unistd.h>` -- **not available on Windows** | Gate with `#ifdef _WIN32` using `<io.h>` and `_isatty(_fileno(stderr))` | test_xplat_log_unistd |
| 20 | iron_log.c:31 | H | `localtime_r(&now, &tm_buf)` -- **POSIX-only**; Windows has `localtime_s(&tm_buf, &now)` (args reversed) | Add `#ifdef _WIN32` with `localtime_s` | test_xplat_log_localtime |
| 21 | iron_log.c:36 | H | `isatty(STDERR_FILENO)` -- **POSIX-only**; Windows needs `_isatty(_fileno(stderr))` from `<io.h>` | Add `#ifdef _WIN32` path | test_xplat_log_isatty |
| 22 | iron_math.c:54 | M | `__thread` for TLS -- GCC/Clang extension; MSVC uses `__declspec(thread)` | Define IRON_THREAD_LOCAL macro in iron_runtime.h | test_xplat_math_tls |
| 23 | iron_math.c:58-60 | M | `clock_gettime(CLOCK_MONOTONIC, &ts)` in lazy init -- POSIX-only | Add Win32 QueryPerformanceCounter or GetTickCount64 path | test_xplat_math_rng_seed |
| 24 | iron_time.c:8 | H | `clock_gettime(CLOCK_REALTIME, &ts)` in Iron_time_now -- **POSIX-only** | Add Win32 GetSystemTimeAsFileTime or equivalent | test_xplat_time_now |
| 25 | iron_time.c:16 | H | `clock_gettime(CLOCK_MONOTONIC, &ts)` in Iron_time_now_ms -- **POSIX-only** | Add Win32 QueryPerformanceCounter path | test_xplat_time_now_ms |
| 26 | iron_time.c:23 | H | `clock_gettime(CLOCK_MONOTONIC, &ts)` in Iron_time_now_ns -- same | Same | test_xplat_time_now_ns |
| 27 | iron_time.c:33-34 | H | `nanosleep(&ts, NULL)` in Iron_time_sleep -- **POSIX-only** | Add Win32 Sleep(ms) path | test_xplat_time_sleep |
| 28 | iron_runtime.h:12-101 | L | Platform threading abstraction is fully gated with Win32/POSIX paths -- correctly done | None needed | test_xplat_runtime_threading |
| 29 | iron_hint.c:15-26 | L | GCC/Clang inline asm vs volatile-sink fallback -- correctly gated with `#if defined(__GNUC__) || defined(__clang__)` | None needed | test_xplat_hint_blackbox |
| 30 | iron_time.h:6 | M | `#include <time.h>` unconditionally -- `<time.h>` is available on Windows but struct timespec and clock_gettime are not standard MSVC | Gate timespec usage or provide Win32 polyfill | test_xplat_time_header |

**Runtime + Stdlib subtotal:** 9H, 7M, 14L = 30 findings

---
