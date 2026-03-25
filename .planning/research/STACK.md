# Stack Research

**Domain:** Compiled programming language / transpile-to-C compiler
**Researched:** 2026-03-25
**Confidence:** HIGH (core choices verified against official docs and community consensus)

---

## Recommended Stack

### Core Technologies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| C11 standard | ISO/IEC 9899:2011 | Compiler implementation language | _Generic, _Alignas/_Alignof, stdint.h, stdbool.h, static_assert all available. Enough modern features to write clean code without complexity of C17/C23. Mandated by project. |
| clang | latest (18+) | Primary backend C compiler for generated code | Best cross-platform support, ships on macOS as system compiler, superior error messages, AddressSanitizer built in. Fallback: gcc. Both must be supported. |
| cmake | 3.25+ | Build system generator for the Iron compiler itself | De-facto standard for cross-platform C projects. Generates Makefiles on Linux/macOS, Visual Studio on Windows, Ninja anywhere. FetchContent module handles raylib. cmake 3.25 added `SYSTEM` keyword for cleaner third-party includes. |
| Ninja | latest (1.11+) | CMake backend for fast incremental builds | Used by clang's own build. cmake -G Ninja gives 10-50x faster no-op rebuilds vs GNU Make on large dependency graphs. Use as the default cmake backend in dev. |
| raylib | 5.5 (Nov 2024) | Game development graphics backend — Iron's target library | Purpose-built for game dev, pure C99 API, zlib license allows static linking in closed source, ships with OpenGL 3.3/ES2/ES3, Windows+macOS+Linux+Web from one codebase. No external dependencies beyond the OS graphics stack. |

### Testing Frameworks

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| Unity (ThrowTheSwitch) | v2.6.1 (Jan 2025) | C unit tests for compiler internals | All lexer, parser, semantic analysis, codegen unit tests. Two header files + one .c file — zero friction to embed. Works with GCC, Clang, MSVC. Extensive assertion macros for byte arrays, strings, integers. |
| Shell-based integration runner | custom (bash/python) | End-to-end `.iron` → compile → run → diff tests | Drive `iron build` on `.iron` test fixtures, capture stdout, diff against expected. No framework overhead needed — a 50-line shell script is cleaner than a test framework for this use case. |

### Memory Debugging Tools

| Tool | Purpose | Notes |
|------|---------|-------|
| AddressSanitizer (ASan) | Heap/stack/global memory errors, use-after-free, buffer overflows | `-fsanitize=address` on clang/gcc. Works on macOS and Linux. 2-3x slowdown — use in dev/CI, not release. |
| UndefinedBehaviorSanitizer (UBSan) | Integer overflow, null dereference, misaligned access, out-of-bounds | `-fsanitize=undefined`. Combine with ASan: `-fsanitize=address,undefined`. Catches bugs ASan misses. |
| ThreadSanitizer (TSan) | Data races in the Iron runtime's thread pool and channel code | `-fsanitize=thread`. Cannot combine with ASan — run as a separate CI pass on concurrency tests. |
| LeakSanitizer (LSan) | Memory leak detection | Bundled with ASan on Linux. On macOS: `ASAN_OPTIONS=detect_leaks=1`. Replace Valgrind — Valgrind no longer works reliably on macOS. |

### Development Tools

| Tool | Purpose | Notes |
|------|---------|-------|
| clang-format | Auto-format compiler source code | Use LLVM style as baseline, checked in `.clang-format`. Run as CMake custom target. Enforces consistency across the codebase. |
| clang-tidy | Static analysis of compiler source | Catches common C bugs. Generate `compile_commands.json` via `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. Run on CI. |
| bear | Generate `compile_commands.json` for Makefile-based projects | Useful if Makefile builds are ever needed outside CMake. |

### Internal Data Structure Libraries

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| stb_ds.h (nothings/stb) | master (public domain) | Type-safe dynamic arrays and hash maps in a single header | Use for symbol tables, string interning tables, AST node lists. Zero dependencies, vendored into `src/vendor/`. The hash map handles `char *` keys natively. |
| Hand-written arena allocator | custom (~100 lines) | Pool allocator for AST nodes, tokens, and scope trees | Arena-allocate everything produced during compilation; free the arena on exit. Avoids hundreds of individual `malloc/free` calls, no fragmentation, faster. Implement from scratch using `malloc` + bump pointer. |

---

## Installation

```bash
# macOS — all tools via Homebrew
brew install cmake ninja llvm raylib

# Linux (Ubuntu/Debian)
apt install cmake ninja-build clang libraylib-dev

# Windows
# Use winget or the raylib installer at https://raylib.com
# Recommended: use the raylib 5.5 Windows portable package which includes a
# self-contained build environment

# Verify raylib is found by cmake
cmake -DCMAKE_BUILD_TYPE=Debug -G Ninja -B build .
cmake --build build
```

For raylib — prefer to fetch and build from source via CMake FetchContent so version is pinned and cross-platform behavior is deterministic:

```cmake
include(FetchContent)
FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG        5.5
)
FetchContent_MakeAvailable(raylib)
```

This approach avoids system-version mismatch and ensures all three platforms (macOS/Linux/Windows) build against the same raylib.

---

## Alternatives Considered

| Recommended | Alternative | Why Not |
|-------------|-------------|---------|
| Hand-written recursive-descent parser | Flex + Bison / ANTLR | Flex/Bison produce poor error messages (hard to control recovery), require a separate grammar file with non-obvious syntax, shift/reduce conflicts are painful to debug. Clang, GCC (since 2006), and Go (since 1.5) all moved to hand-written parsers. For a language with Iron's complexity the hand-written approach is unambiguously correct. |
| CMake + Ninja | plain Makefile | Makefile works on macOS/Linux but Windows is painful. CMake handles platform differences (Windows SDK paths, MSVC flags, macOS frameworks). The Iron compiler targets all three platforms from day one — a plain Makefile becomes a liability immediately. |
| AddressSanitizer / UBSan | Valgrind | Valgrind no longer works reliably on modern macOS (Apple Silicon). ASan is built into clang, 100x faster than Valgrind, catches bugs Valgrind misses (stack errors, UB). No reason to use Valgrind for new projects. |
| Unity (ThrowTheSwitch) | Criterion | Criterion v2.4.3 is actively maintained but adds shared library / linker complexity. Unity is two headers + one .c file, zero configuration, works everywhere clang works. For compiler internals (pure C, no need for test isolation) Unity is simpler and faster to integrate. |
| stb_ds.h for hash maps | uthash / khash | stb_ds uses type-safe macros, public domain license, no separate compilation step, no external headers. uthash is macro-based but more complex. khash is faster but less ergonomic. stb_ds is the best fit for a project that needs 3-4 hash map uses (symbol table, string interning, module map). |
| Arena allocator (custom) | malloc everywhere | Compilers allocate thousands of small, same-lifetime objects (tokens, AST nodes). Per-object malloc/free is slow and leaks are hard to track. Arena: one malloc per compilation unit, free at the end. Tcc, chibicc, and most serious C compilers use this pattern. |
| raylib 5.5 (static, FetchContent) | SDL2 / SDL3 | Iron's purpose is game development with raylib specifically. SDL is lower-level and requires more glue. raylib has a simpler C API that maps more cleanly to Iron's intended bindings layer. |

---

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| Flex + Bison | Generates hard-to-maintain code, poor error recovery, confusing shift/reduce conflicts, not used by any major modern compiler | Hand-written recursive descent parser |
| Valgrind | Does not work on macOS (Apple Silicon), 100-500x slowdown makes CI impractical, misses stack errors | clang ASan + UBSan (`-fsanitize=address,undefined`) |
| C++ for the compiler | Adds ABI complexity, makes self-hosting harder, breaks the "compiler written in C" constraint defined in PROJECT.md | C11 (as mandated) |
| C99 standard | Missing `_Generic`, `_Alignas/_Alignof`, `static_assert` — all useful for writing a clean runtime. No reason to downgrade. | C11 |
| CMocka | More complex setup than Unity, requires shared library linking, better suited for Linux-only embedded work than a cross-platform game toolchain | Unity |
| autotools (configure/Makefile.am) | Extremely painful on Windows, slow, arcane failure modes, not used by new projects | CMake |
| Global malloc everywhere | Fragmentation, leak-prone for thousands of same-lifetime allocations in a compiler pass | Arena allocator per compile phase |

---

## Stack Patterns by Variant

**For compiler development (lexer, parser, semantic analysis, codegen):**
- Arena-allocate all AST nodes and tokens per source file
- Use stb_ds hash maps for symbol tables and string interning
- Test each phase with Unity unit tests
- Run ASan + UBSan on all test builds

**For the runtime library (`iron_runtime.c`):**
- Use pthreads directly (POSIX on Linux/macOS, pthreads-win32 on Windows)
- Use TSan in a separate CI pass to catch thread pool races
- Keep it under 1000 lines — complexity belongs in the language, not the runtime

**For raylib bindings (Phase 8+, out of scope for initial roadmap):**
- Use FetchContent to pin raylib 5.5
- Static link on all platforms — do NOT dynamically link raylib for distribution
- On macOS, CMake must add `-framework Cocoa -framework IOKit -framework OpenGL`

**For the CLI toolchain (`iron` binary):**
- Invoke `clang` (preferred) or `gcc` (fallback) via `execvp` / `CreateProcessW`
- Detect which is available at startup, not hardcoded
- Pass `-std=c11 -O2 -lpthread` flags; pass `-g -fsanitize=address,undefined` when `--debug` flag is set

---

## Version Compatibility

| Package | Compatible With | Notes |
|---------|-----------------|-------|
| raylib 5.5 | cmake 3.15+ | FetchContent_Declare requires cmake 3.14+, recommend 3.25+ |
| clang 18+ | C11 | Full C11 support. `-std=c11 -Wall -Werror -Wextra` recommended |
| Unity v2.6.1 | C11, GCC, Clang, MSVC | Works with `-std=c11`. No C++ required. |
| stb_ds.h | C99+, C11 | Single-header, just copy into `src/vendor/`. Public domain. |
| ASan + UBSan | clang 3.1+ | Cannot combine with TSan or MSan — run separately in CI |
| pthreads | Linux/macOS native | Windows: pthreads-win32 (`-DUSE_PTHREADS_WIN32`) or Windows Threads API via a thin shim |

---

## Diagnostic System Implementation Notes

For Rust-style rich diagnostics (source snippets, arrows, suggestions), implement from scratch in C — there is no widely-used C library equivalent to Rust's `miette` or `codespan`. The implementation is ~300-400 lines:

1. Store `(file, line_start, col_start, line_end, col_end)` in every AST node and token
2. At error emission, re-read the source line from the source buffer
3. Print: `file:line:col: error: message`
4. Print the source line
5. Print a `^~~~` underline spanning the error range
6. For suggestions, print a `help:` note with the corrected text

This is the pattern used by clang's own diagnostic engine. No external library needed.

---

## Sources

- [raylib 5.5 release notes](https://github.com/raysan5/raylib/releases/tag/5.5) — version confirmed Nov 18, 2024 (HIGH confidence)
- [raylib CMake wiki](https://github.com/raysan5/raylib/wiki/Working-with-CMake) — FetchContent and find_package patterns (HIGH confidence)
- [Unity v2.6.1 release](https://github.com/ThrowTheSwitch/Unity) — version confirmed Jan 1, 2025 (HIGH confidence)
- [Criterion v2.4.3](https://github.com/Snaipe/Criterion) — Oct 14, 2025, actively maintained (HIGH confidence)
- [AddressSanitizer clang docs](https://clang.llvm.org/docs/AddressSanitizer.html) — current flags confirmed (HIGH confidence)
- [Parser generators vs handwritten parsers — Lobsters 2021](https://lobste.rs/s/10pkib/parser_generators_vs_handwritten) — community survey; GCC/Clang/Go all use hand-written parsers (HIGH confidence)
- [stb_ds.h](https://github.com/nothings/stb/blob/master/stb_ds.h) — public domain, single-header hash map (HIGH confidence)
- [ASan vs Valgrind comparison — Red Hat](https://developers.redhat.com/blog/2021/05/05/memory-error-checking-in-c-and-c-comparing-sanitizers-and-valgrind) — Valgrind macOS deprecation confirmed (HIGH confidence)
- [CMake vs Make for cross-platform C](https://earthly.dev/blog/cmake-vs-make-diff/) — cmake recommended for Windows+macOS+Linux (MEDIUM confidence, WebSearch)
- [chibicc — small C compiler reference implementation](https://github.com/rui314/chibicc) — arena allocator and multi-pass patterns (MEDIUM confidence, WebSearch)

---
*Stack research for: Iron language compiler (transpile-to-C)*
*Researched: 2026-03-25*
