# Iron Cross-Platform Technical Debt

**Date:** 2026-04-12
**Scope:** All C source files in src/ (excluding src/vendor/ and src/cli/toml.c)
**Purpose:** Log all code that assumes specific platform characteristics, for a future Windows-compat milestone

## Known Deferred Issues (from REQUIREMENTS.md)

These are already tracked as WIN-01 through WIN-04:
- WIN-01: strbuf.h `__attribute__((format(...)))` not guarded for MSVC
- WIN-02: diagnostics.c `<unistd.h>` not ifdef-guarded
- WIN-03: pkg/*.c POSIX-deprecation warnings
- WIN-04: Windows CI not in required matrix

---

## 1. POSIX-Only Headers and APIs

| File:Line | API/Header | Platform | Impact | Fix Complexity |
|-----------|-----------|----------|--------|---------------|
| iron_io.c:5 | `#include <sys/stat.h>` + `mkdir(p, 0755)` at line 106 | POSIX-only mkdir with mode | No Windows directory creation via `iron build` | Medium - add `#ifdef _WIN32` with `_mkdir(p)` from `<direct.h>` |
| iron_io.c:6 | `#include <dirent.h>` with opendir/readdir/closedir | Not available on Windows | Iron_io_list_files_result completely broken on Windows | High - add Win32 FindFirstFile/FindNextFile/FindClose path |
| iron_io.c:100 | `stat(p, &st)` + `S_ISDIR` macro | Available on Windows via `<sys/stat.h>` but `S_ISDIR` not defined on MSVC | iron_io_is_directory broken on MSVC | Low - add `_S_IFDIR` fallback for MSVC |
| iron_log.c:4 | `#include <unistd.h>` unconditionally | Not available on Windows | iron_log.c fails to compile on Windows | Low - gate with `#ifdef _WIN32` using `<io.h>` |
| iron_log.c:31 | `localtime_r(&now, &tm_buf)` | POSIX-only; Windows has `localtime_s(&tm_buf, &now)` (args reversed) | Log timestamps broken on Windows | Low - add `#ifdef _WIN32` with `localtime_s` |
| iron_log.c:36 | `isatty(STDERR_FILENO)` | POSIX-only | Color detection broken on Windows | Low - add `_isatty(_fileno(stderr))` from `<io.h>` |
| iron_time.c:8 | `clock_gettime(CLOCK_REALTIME, &ts)` in Iron_time_now | POSIX-only | No wall-clock time on Windows | Medium - add Win32 GetSystemTimeAsFileTime |
| iron_time.c:16 | `clock_gettime(CLOCK_MONOTONIC, &ts)` in Iron_time_now_ms | POSIX-only | No monotonic clock on Windows | Medium - add Win32 QueryPerformanceCounter |
| iron_time.c:23 | `clock_gettime(CLOCK_MONOTONIC, &ts)` in Iron_time_now_ns | POSIX-only | Same as above, nanosecond variant | Medium - same fix as above |
| iron_time.c:33-34 | `nanosleep(&ts, NULL)` in Iron_time_sleep | POSIX-only | No sleep on Windows | Low - add Win32 `Sleep(ms)` path |
| iron_math.c:58-60 | `clock_gettime(CLOCK_MONOTONIC, &ts)` in RNG lazy init | POSIX-only | RNG seed initialization broken on Windows | Low - add Win32 QueryPerformanceCounter or GetTickCount64 |
| comptime.c:22 | `#include <sys/stat.h>` | Used for `mkdir()` at lines 180-181 | Comptime cache directory creation fails on Windows | Low - add `#ifdef _WIN32` with `_mkdir` from `<direct.h>` |
| comptime.c:180-181 | `mkdir(".iron-build", 0755)` and `mkdir(".iron-build/comptime", 0755)` | `mkdir()` with mode is POSIX-only | Comptime cache system broken on Windows | Low - add `_mkdir` without mode on Windows |
| diagnostics.c:7 | `#include <unistd.h>` unconditionally | Not available on Windows | diagnostics.c fails to compile on Windows | Low - gate with `#ifdef _WIN32` using `<io.h>` |
| diagnostics.c:74 | `isatty(STDERR_FILENO)` in use_color() | POSIX-only | Color detection broken on Windows | Low - add `_isatty(_fileno(stderr))` |
| test_runner.c:7 | `#include <unistd.h>` unconditionally | Not available on Windows | test_runner.c fails to compile on Windows | Part of full test_runner rewrite |
| test_runner.c:8 | `#include <dirent.h>` unconditionally | Not available on Windows | Test discovery broken on Windows | High - add Win32 FindFirstFile path |
| test_runner.c:9 | `#include <sys/wait.h>` unconditionally | Not available on Windows | Test process management broken | High - add WaitForSingleObject path |
| test_runner.c:12 | `#include <spawn.h>` unconditionally | Not available on Windows | Cannot launch test processes on Windows | High - add CreateProcess path |
| test_runner.c:27 | `isatty(STDOUT_FILENO)` | POSIX-only | Color detection in test output broken | Low - add `_isatty(_fileno(stdout))` |
| test_runner.c:76 | `posix_spawn` / `waitpid` / `WIFEXITED` | Entirely POSIX-only process management | test_runner completely non-functional on Windows | High - full Win32 CreateProcess + WaitForSingleObject + GetExitCodeProcess rewrite |
| iron_io.c:206 | `strrchr(p, '/')` in basename/dirname | Only checks `/`, not `\` | Path operations return wrong results for Windows paths with backslashes | Low - search for both `/` and `\\` separators |

## 2. GCC/Clang-Only Attributes

| File:Line | API/Header | Platform | Impact | Fix Complexity |
|-----------|-----------|----------|--------|---------------|
| strbuf.h:22-23 | `__attribute__((format(printf, 2, 3)))` | GCC/Clang-only; MSVC ignores unknown attributes but older versions may warn | Compile warning on older MSVC; no runtime impact | Low - gate with `#if defined(__GNUC__) \|\| defined(__clang__)` |
| hir_to_lir.c:378 | `__attribute__((unused))` on `hirlir_dominates` | GCC/Clang-specific | MSVC ignores; may warn | Low - add `#ifdef __GNUC__` guard or use `(void)hirlir_dominates;` |
| iron_hint.c:15-26 | GCC/Clang inline asm for blackbox hint | GCC/Clang-specific | Already correctly gated with volatile-sink fallback | None needed - already handled |

## 3. 64-bit Pointer Assumptions

| File:Line | API/Header | Platform | Impact | Fix Complexity |
|-----------|-----------|----------|--------|---------------|
| emit_structs.c:311-327 | `emit_estimate_type_size` assumes String=16, Closure=16, arrays=24, others=8 | These sizes are compile-time estimates for variant split decisions | On 32-bit systems, actual struct sizes differ; variant split heuristic may make suboptimal (but not incorrect) decisions | Low - cosmetic; does not affect correctness |

## 4. Endianness Assumptions

No endianness assumptions found. The codebase does not perform manual byte-order operations outside of the network layer, and the network layer uses standard `htons`/`ntohs` (which are platform-abstracted).

## 5. Struct Padding Assumptions

| File:Line | API/Header | Platform | Impact | Fix Complexity |
|-----------|-----------|----------|--------|---------------|
| iron_rc.c:20,74 | `(char *)ctrl + sizeof(Iron_RcControl)` to access value after control block | Relies on single-allocation layout with no padding between control and value | If alignment requirements differ, value access reads wrong bytes; correct on all current targets (x86-64, ARM64) | Low - add _Static_assert or offsetof check |

## 6. Path Separator Assumptions

| File:Line | API/Header | Platform | Impact | Fix Complexity |
|-----------|-----------|----------|--------|---------------|
| iron_io.c:206 | `strrchr(p, '/')` only in basename/dirname | Hardcoded `/` separator | Windows paths using `\` return wrong basename/dirname | Low - search for both separators |
| comptime.c:95,189 | `fopen(cache_path, "r"/"w")` with `/` separator | Windows fopen accepts `/` | No issue - Windows fopen handles forward slashes | None needed |
| parser.c:1970 | `path_buf` for import paths uses `/` | Standard for Iron source imports | No issue unless platform-native paths leak into import resolution | Low - normalize path separators at import boundary |

## 7. Other Platform-Specific Code

| File:Line | API/Header | Platform | Impact | Fix Complexity |
|-----------|-----------|----------|--------|---------------|
| value_range.c:69-90 | `__builtin_add_overflow`, `__builtin_sub_overflow`, `__builtin_mul_overflow` | GCC/Clang builtins, not available on MSVC | Value range analysis fails to compile on MSVC | Medium - need `#ifdef _MSC_VER` fallback with manual overflow checks using _addcarry_u64 or equivalent |
| emit_c.c:3718 | Generated C uses `alloca()` for VLA fill arrays | `alloca` not standard C; some MSVC configurations need `_alloca` | Generated C may not compile on Windows | Low - add `#ifdef _WIN32` with `_alloca` or use malloc+free |
| emit_c.c:3673 | Generated closure dispatch: `(ret (*)(void*, ...))fn` variadic cast | Casting function pointer to variadic type is implementation-defined in C | Works on x86-64 and ARM64 calling conventions but technically non-portable | Medium - consider alternative dispatch mechanism |
| emit_c.c:3382 | `Iron_List` (unparameterized) in SLICE emission | Hardcoded type name | Slice on non-int arrays produces wrong type | Medium - parameterize slice emission |
| iron_math.c:54 | `__thread` for TLS | GCC/Clang extension; MSVC uses `__declspec(thread)` | iron_math.c fails to compile on MSVC | Low - define IRON_THREAD_LOCAL macro in iron_runtime.h |
| iron_time.h:6 | `#include <time.h>` unconditionally | `<time.h>` available on Windows but struct timespec and clock_gettime are not standard MSVC | timespec usage may fail on older MSVC | Low - gate timespec usage or provide Win32 polyfill |
| iron_threads.c:6 | NTP clock slew caveat with CLOCK_REALTIME | POSIX timedwait uses CLOCK_REALTIME which can jump during NTP adjustments | Spurious timeout or hang during NTP adjustment on POSIX | Low - upgrade to CLOCK_MONOTONIC via pthread_condattr_setclock when supported |
| iron_threads.c:462-484 | Elastic worker self-retire Win32 path uses GetCurrentThread() pseudo-handle | Correct but fragile if two workers retire simultaneously | Race condition in elastic pool worker retirement on Windows | Medium - use GetCurrentThreadId() and store DWORD IDs alongside HANDLEs |
| parser.c:226,816,1720 | `_Alignof(Iron_Node *)` / `_Alignof(const char *)` | C11 standard; MSVC in C mode requires `__alignof` | Parser may not compile on MSVC without C11 mode | Low - use `sizeof(void *)` as alignment or add `#ifdef _MSC_VER` with `__alignof` |
| hir_lower.c:174 | `_Alignof(Iron_Type *)` | C11 standard | Same as above | Low - same fix |

---

## Summary

| Category | Count | Fix Complexity (High/Medium/Low) |
|----------|-------|----------------------------------|
| POSIX-Only Headers and APIs | 22 | 4 high, 5 medium, 13 low |
| GCC/Clang-Only Attributes | 3 | 0 high, 0 medium, 2 low (1 already handled) |
| 64-bit Pointer Assumptions | 1 | 0 high, 0 medium, 1 low |
| Endianness Assumptions | 0 | -- |
| Struct Padding Assumptions | 1 | 0 high, 0 medium, 1 low |
| Path Separator Assumptions | 3 | 0 high, 0 medium, 1 low (2 non-issues) |
| Other Platform-Specific Code | 11 | 0 high, 3 medium, 8 low |
| **Total** | **41** | **4 high, 8 medium, 26 low** |

Overall severity breakdown from audit dimensions:
- **High (20):** 17 from runtime/stdlib/infra, 3 from comptime
- **Medium (14):** 8 from runtime/stdlib/infra, 6 from LIR
- **Low (42):** 29 from runtime/stdlib/infra, 7 from LIR, 3 from comptime, 3 from parser

## Recommendations

1. **test_runner.c is the worst offender and should be addressed first.** It has 8 H-severity POSIX-only findings (6 unique APIs: unistd.h, dirent.h, sys/wait.h, spawn.h, isatty, posix_spawn+waitpid). A complete Win32 process management path (CreateProcess + WaitForSingleObject + GetExitCodeProcess) plus Win32 directory scanning (FindFirstFile) is required. This is the highest fix complexity in the entire cross-platform debt.

2. **iron_time.c and iron_log.c are trivial ifdef guards.** All 7 H-severity findings in these files (clock_gettime x3, nanosleep, localtime_r, isatty, unistd.h include) have well-known Win32 equivalents (QueryPerformanceCounter, Sleep, localtime_s, _isatty). Each fix is a few lines of `#ifdef _WIN32` code.

3. **diagnostics.c and iron_io.c are medium-complexity.** diagnostics.c needs `<io.h>` + `_isatty` (low). iron_io.c needs both `_mkdir` from `<direct.h>` (low) and a full Win32 directory listing path using FindFirstFile/FindNextFile (high).

4. **comptime.c mkdir is trivial.** Two `mkdir()` calls with POSIX mode need `_mkdir` on Windows.

5. **value_range.c overflow builtins require MSVC fallback.** The `__builtin_add_overflow` family needs manual overflow check implementations for MSVC. This is medium complexity but affects the correctness of the value range analysis on Windows.

6. **The GCC/Clang attribute findings are cosmetic.** MSVC ignores unknown `__attribute__` directives. The only action needed is ifdef guards to suppress warnings on strict MSVC configurations.

7. **Priorities for a Windows-compat milestone:**
   - Phase 1 (trivial): iron_log.c, iron_time.c, diagnostics.c, comptime.c, strbuf.h -- all ifdef guards (estimated 1-2 hours)
   - Phase 2 (medium): iron_io.c directory listing, iron_math.c TLS, value_range.c builtins, emit_c.c alloca (estimated 4-6 hours)
   - Phase 3 (substantial): test_runner.c full Win32 process management rewrite (estimated 8-12 hours)
   - Phase 4 (optional): iron_threads.c elastic worker race fix, emit_c.c variadic cast alternative (estimated 4-6 hours)
