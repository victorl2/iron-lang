---
phase: 04-comptime-game-dev-and-cross-platform
plan: 03
subsystem: compiler
tags: [raylib, codegen, extern-func, enum, ffi, game-dev]

# Dependency graph
requires:
  - phase: 04-01
    provides: extern func declarations, draw block codegen, is_extern/extern_c_name fields
  - phase: 04-02
    provides: comptime interpreter, iron_analyze updated signature with force_comptime
provides:
  - raylib.iron wrapper with Key enum (explicit ordinals matching raylib KEY_* constants)
  - Iron_EnumVariant explicit value support (has_explicit_value/explicit_value fields)
  - Parser parses VARIANT = N syntax in enum bodies
  - Codegen emits = N in C typedef enum when has_explicit_value is true
  - Build pipeline: import raylib prepends raylib.iron, toml-based raylib detection
  - invoke_clang extended with raylib source compilation and platform-specific flags
  - --force-comptime flag parsed and wired through CLI
  - Integration test: extern func calls via puts() work end-to-end
  - iron_snake_to_camel fixed to preserve names without underscores (puts stays puts)
affects: [04-04, 04-05]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Enum explicit values: has_explicit_value + explicit_value fields on Iron_EnumVariant; parser checks for ASSIGN+INTEGER after variant name"
    - "Raylib import: strstr for 'import raylib' in source, prepend raylib.iron before lex/parse/analyze"
    - "snake_to_camel only applies when name contains underscores; single-word C names pass through unchanged"
    - "invoke_clang accepts IronBuildOpts and dynamically builds argv for optional raylib compilation"

key-files:
  created:
    - src/stdlib/raylib.iron
    - tests/integration/extern_basic.iron
    - tests/integration/extern_basic.expected
  modified:
    - src/parser/ast.h
    - src/parser/parser.c
    - src/codegen/codegen.c
    - src/cli/build.h
    - src/cli/build.c
    - src/cli/main.c
    - tests/test_codegen.c
    - tests/test_comptime.c
    - tests/test_pipeline.c

key-decisions:
  - "iron_snake_to_camel skips conversion when name has no underscores — preserves single-word C library names like puts/printf unchanged"
  - "import raylib uses strstr-based source detection and prepends raylib.iron before lex phase (no multi-file parser needed)"
  - "invoke_clang uses dynamic argv array with IronBuildOpts for conditional raylib compilation"
  - "iron_analyze call sites updated to pass force_comptime from opts struct"

patterns-established:
  - "Enum explicit ordinals: parser checks IRON_TOK_ASSIGN after variant name and reads INTEGER literal"
  - "Build pipeline feature flags: IronBuildOpts fields gate optional compilation behavior"

requirements-completed: [GAME-01, GAME-03]

# Metrics
duration: 8min
completed: 2026-03-26
---

# Phase 04 Plan 03: Raylib Wrapper and Enum Explicit Values Summary

**raylib.iron wrapper with Key enum (RIGHT=262 etc), explicit enum ordinals in AST/parser/codegen, build pipeline compiles raylib inline with platform flags, extern func integration test passing**

## Performance

- **Duration:** 8 min
- **Started:** 2026-03-26T15:54:38Z
- **Completed:** 2026-03-26T16:02:38Z
- **Tasks:** 3
- **Files modified:** 10

## Accomplishments

- Key enum in raylib.iron uses explicit ordinal values matching raylib KEY_* constants so `is_key_down(.RIGHT)` passes integer 262 to C
- Iron_EnumVariant extended with has_explicit_value/explicit_value; parser parses `VARIANT = N` syntax; codegen emits `= N` in C typedef enum
- raylib.iron wrapper file with Vec2/Color objects, Key enum, window/drawing/input extern funcs (CamelCase names matching raylib API)
- Build pipeline: `import raylib` detection prepends raylib.iron source, `invoke_clang` extended for raylib source + platform-specific link flags
- extern_basic integration test proves extern func calls work end-to-end (puts → correct C, binary runs)

## Task Commits

1. **Task 1: Explicit enum value support** - `e6d8c34` (feat)
2. **Task 2: raylib.iron wrapper and build pipeline** - `d225442` (feat)
3. **Task 3: Integration tests** - `4a64bec` (feat)

## Files Created/Modified

- `src/stdlib/raylib.iron` - Raylib bindings: Key enum with explicit ordinals, Color/Vec2 objects, extern func declarations
- `src/parser/ast.h` - Iron_EnumVariant extended with has_explicit_value/explicit_value fields
- `src/parser/parser.c` - Parser handles VARIANT = INTEGER in enum; snake_to_camel fix for single-word names
- `src/codegen/codegen.c` - Enum codegen emits = N when has_explicit_value is true
- `src/cli/build.h` - IronBuildOpts: use_raylib and force_comptime fields added
- `src/cli/build.c` - import raylib source prepending, toml raylib detection, invoke_clang with raylib flags
- `src/cli/main.c` - --force-comptime flag parsing; new opts fields initialized
- `tests/integration/extern_basic.iron` - Extern func integration test (calls puts)
- `tests/integration/extern_basic.expected` - Expected output: "hello from extern"
- `tests/test_codegen.c` - test_enum_explicit_values added (verifies RIGHT=262 etc in C output)
- `tests/test_comptime.c` - Fixed iron_analyze call to use new 7-arg signature
- `tests/test_pipeline.c` - Fixed iron_analyze call sites to use new 7-arg signature

## Decisions Made

- `iron_snake_to_camel` skips conversion when name has no underscores — preserves single-word C library names like `puts` and `printf` unchanged. Names with underscores still get CamelCase conversion (init_window → InitWindow).
- `import raylib` uses `strstr`-based source detection and prepends raylib.iron before the lex phase — no multi-file parser complexity needed.
- `invoke_clang` uses a dynamically-built argv array parameterized by `IronBuildOpts` for conditional raylib compilation.
- Fixed pre-existing `iron_analyze` call sites in test_comptime.c and test_pipeline.c that used the old 3-argument signature from before plan 04-02 updated it to 7 arguments.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Fixed iron_analyze call sites with old 3-argument signature**
- **Found during:** Task 2 (build pipeline extension)
- **Issue:** test_comptime.c and test_pipeline.c called `iron_analyze(prog, &arena, &diags)` — the 3-arg signature from before 04-02 changed it to 7 args. Also, build.c itself had the old signature. All failed to compile.
- **Fix:** Updated all 6 call sites to pass `source_file_dir`, `source_text`, `source_len`, and `force_comptime` arguments.
- **Files modified:** src/cli/build.c, tests/test_comptime.c, tests/test_pipeline.c
- **Verification:** Build succeeded with zero errors after fix
- **Committed in:** d225442 (Task 2 commit)

**2. [Rule 1 - Bug] Fixed iron_snake_to_camel capitalizing single-word C names**
- **Found during:** Task 3 (integration test for extern_basic)
- **Issue:** `iron_snake_to_camel` always capitalized the first letter, turning `puts` → `Puts` which doesn't exist in libc. The extern_basic integration test called `Puts()` which caused a linker error.
- **Fix:** Added early return when name has no underscores — `strchr(name, '_') == NULL` → return name unchanged. `init_window` → `InitWindow` still works (has underscores).
- **Files modified:** src/parser/parser.c
- **Verification:** extern_basic integration test passes (`puts("hello from extern")` → correct output)
- **Committed in:** 4a64bec (Task 3 commit)

---

**Total deviations:** 2 auto-fixed (2 bugs)
**Impact on plan:** Both auto-fixes necessary for correctness. No scope creep.

## Issues Encountered

- Pre-existing untracked `tests/integration/comptime_basic.iron` test (from plan 04-02 session) was failing because global-level comptime val declarations don't propagate their evaluated value to generated C. Documented in `deferred-items.md`. Out of scope for this plan.

## Next Phase Readiness

- raylib.iron wrapper complete with correct Key ordinals — `is_key_down(.RIGHT)` will pass 262 to C
- Build pipeline ready for raylib game compilation when raylib source is placed at src/vendor/raylib/
- Explicit enum ordinals enable any C library's integer constant mapping via Iron enum syntax
- Integration test infrastructure: extern func calling proven end-to-end

---
*Phase: 04-comptime-game-dev-and-cross-platform*
*Completed: 2026-03-26*
