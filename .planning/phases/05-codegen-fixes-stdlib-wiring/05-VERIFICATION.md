---
phase: 05-codegen-fixes-stdlib-wiring
verified: 2026-03-26T22:00:00Z
status: gaps_found
score: 4/5 must-haves verified
re_verification: false
gaps:
  - truth: "import io followed by val content, err = read_file(\"test.txt\") compiles and returns file contents"
    status: failed
    reason: "IO.read_file() is declared as returning String in io.iron, but the C implementation Iron_io_read_file() returns Iron_Result_String_Error. Assigning val content = IO.read_file(...) generates C code that fails to compile with: initializing 'const Iron_String' with an expression of incompatible type 'Iron_Result_String_Error'."
    artifacts:
      - path: "src/stdlib/io.iron"
        issue: "Declares 'func IO.read_file(path: String) -> String {}' but the C implementation returns Iron_Result_String_Error, not Iron_String"
    missing:
      - "Fix io.iron read_file return type to match actual C API, OR wrap the C call in auto-static codegen to extract .value from the result struct"
      - "The integration test (test_io.iron) only tests IO.file_exists, which works correctly — IO.read_file is untested end-to-end"
---

# Phase 5: Codegen Fixes + Stdlib Wiring Verification Report

**Phase Goal:** String interpolation and parallel-for produce correct output; stdlib modules (math, io, time, log) are callable from Iron source via import
**Verified:** 2026-03-26T22:00:00Z
**Status:** gaps_found
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| #   | Truth | Status | Evidence |
| --- | ----- | ------ | -------- |
| 1   | `"value is {x}"` where x=42 produces "value is 42" at runtime (not empty string) | VERIFIED | test_interp binary prints "value is 42"; generated C uses snprintf two-pass with %lld; test passes in integration suite |
| 2   | `parallel for i in range(100) {}` correctly distributes range across pool workers | VERIFIED (partial) | test_parallel binary runs without crash; codegen emits Iron_parallel_ctx_N struct, chunk functions, Iron_pool_submit, Iron_pool_barrier; each iteration range is split correctly; array[i]=i form not expressible in Iron v1 (no native arrays), empty body tests codegen correctness |
| 3   | `import math` followed by `Math.sin(1.0)` compiles and produces the correct result | VERIFIED | Math.sin(1.0) emits Iron_math_sin(1.0) via auto-static dispatch; test_math binary prints "math works"; test_sc3.iron produces correct output |
| 4   | `import io` followed by `IO.read_file("test.txt")` compiles and returns file contents | FAILED | IO.read_file() returns Iron_Result_String_Error in C but io.iron declares return type String — C compile error when assigning result; test_io.iron only tests IO.file_exists (which works) |
| 5   | `import time` and `import log` modules are callable from Iron source | VERIFIED | test_time binary calls Time.now_ms() and prints "time works"; test_log binary compiles import log and prints "log works"; all four modules import and run |

**Score:** 4/5 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
| -------- | -------- | ------ | ------- |
| `src/codegen/gen_exprs.c` | IRON_NODE_INTERP_STRING snprintf two-pass | VERIFIED | Lines 324-490: full implementation with %lld/%g/%s format specifiers, stack buffer + heap fallback, iron_string_from_literal wrapping |
| `src/codegen/gen_stmts.c` | Parallel-for capture struct + chunk function | VERIFIED | Lines 244-360: Iron_parallel_ctx_N struct, void(*)(void*) chunk signature, range splitting, Iron_pool_submit, Iron_pool_barrier |
| `src/codegen/gen_exprs.c` | Auto-static METHOD_CALL and FIELD_ACCESS dispatch | VERIFIED | Lines 802-882: IRON_SYM_TYPE detection emits Iron_<lower>_<method>() for methods and IRON_<UPPER> for fields |
| `src/stdlib/math.iron` | Math object with trig/sqrt/pow/random methods | VERIFIED | All methods declared as top-level func Math.method() syntax; PI/TAU/E fields declared |
| `src/stdlib/io.iron` | IO object with file operation methods | PARTIAL | file_exists/write_file/list_files/create_dir/delete_file work; read_file has wrong return type (String vs Iron_Result_String_Error) |
| `src/stdlib/time.iron` | Time object with timing methods | VERIFIED | Time.now_ms() compiles and runs correctly |
| `src/stdlib/log.iron` | Log object with logging methods | VERIFIED | Log module imports and compiles; Log.info excluded from test due to dynamic timestamp |
| `src/cli/build.c` | strstr import detection for math/io/time/log | VERIFIED | Lines 572-658: four detection blocks for import math/io/time/log with .iron prepend |
| `tests/test_interp_codegen.c` | 6 Unity tests for interpolation codegen | VERIFIED | All 6 tests pass: int/float/bool/string/from_literal/no_exprs |
| `tests/test_parallel_codegen.c` | 5 Unity tests for parallel codegen | VERIFIED | All 5 tests pass: ctx_struct/chunk_fn/pool_submit/barrier/capture_in_struct |
| `tests/integration/test_interp.iron` | Integration test for string interpolation | VERIFIED | Passes: value is 42, pi is 3.14, flag is true, hello world, sum is 30 |
| `tests/integration/test_parallel.iron` | Integration test for parallel-for | VERIFIED | Passes: parallel sum = 4950, parallel done |
| `tests/integration/test_math.iron` | Integration test for math stdlib | VERIFIED | Passes: sin(0) is 0, sqrt(4) is 2, math works |
| `tests/integration/test_io.iron` | Integration test for io stdlib | PARTIAL | Tests IO.file_exists only; IO.read_file is untested (broken return type) |
| `tests/integration/test_time.iron` | Integration test for time stdlib | VERIFIED | Passes: time works |
| `tests/integration/test_log.iron` | Integration test for log stdlib | VERIFIED | Passes: log works |
| `tests/integration/test_combined.iron` | Combined integration test | VERIFIED | Passes: interpolation + Math.sqrt + parallel-for all work together |
| `tests/integration/hello.iron` | Updated hello.iron with interpolation | VERIFIED | Uses println("x is {x}"); outputs "hello from iron" + "x is 42" |

### Key Link Verification

| From | To | Via | Status | Details |
| ---- | -- | --- | ------ | ------- |
| gen_exprs.c INTERP_STRING case | iron_string_from_literal | GNU statement expression with snprintf | WIRED | Generated C confirmed: `Iron_String _ir_N = iron_string_from_literal(_ibuf_N, ...)` |
| gen_stmts.c parallel-for | Iron_pool_submit / Iron_pool_barrier | malloc per chunk, void(*)(void*) signature | WIRED | Generated C confirmed: Iron_pool_submit(Iron_global_pool, Iron_parallel_chunk_0, _ctx) + barrier |
| gen_exprs.c METHOD_CALL | Iron_math_sin / Iron_io_ / Iron_time_ / Iron_log_ | IRON_SYM_TYPE detection in receiver | WIRED | Generated C confirmed: `Iron_math_sin(1.0)` for `Math.sin(1.0)` |
| build.c import detection | math.iron / io.iron / time.iron / log.iron | strstr source scan + prepend | WIRED | Lines 572-658 confirmed; tested in integration suite |
| io.iron read_file -> Iron_io_read_file | Iron_Result_String_Error return value | auto-static dispatch | BROKEN | io.iron declares `-> String` but C returns `Iron_Result_String_Error`; C compile error on assignment |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
| ----------- | ----------- | ----------- | ------ | -------- |
| GEN-01 | 05-01 | C code emitted for all Iron constructs compiles with clang -std=c11 -Wall -Werror | SATISFIED | Build passes; all integration tests compile clean |
| GEN-11 | 05-02 | Parallel-for generates range splitting, chunk submission, and barrier | SATISFIED | Codegen verified; 5 unit tests pass; integration test passes |
| STD-01 | 05-03 | math module: trig, sqrt, pow, lerp, random, PI/TAU/E | SATISFIED | Math.sin/sqrt/cos/pow/random all callable; Math.PI via IRON_PI |
| STD-02 | 05-03 | io module: file read/write, file_exists, list_files, create_dir | PARTIAL | IO.file_exists works; IO.read_file has return type mismatch causing C compile error |
| STD-03 | 05-03 | time module: now, now_ms, sleep, since, Timer | SATISFIED | Time.now_ms() compiles and runs |
| STD-04 | 05-03 | log module: info/warn/error/debug with level filtering | SATISFIED | Log module imports and compiles; Log.info works (excluded from .expected due to timestamp) |

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
| ---- | ---- | ------- | -------- | ------ |
| src/cli/build.c | 206, 226 | "placeholder" / strdup tmp path | Info | Pre-existing Windows/temp file handling; not related to Phase 5 |

No functional stubs, empty return bodies, or TODO blockers found in Phase 5 files.

### Human Verification Required

None — all Phase 5 behaviors are verifiable programmatically except the one automated gap above.

### Gaps Summary

**One gap blocks full goal achievement:**

SC4 states that `import io` followed by `val content, err = read_file("test.txt")` should compile and return file contents. The io.iron wrapper declares `func IO.read_file(path: String) -> String {}` but the underlying C function `Iron_io_read_file()` returns `Iron_Result_String_Error`. When Iron generates `const Iron_String content = Iron_io_read_file(...)`, clang rejects it with a type mismatch error.

The integration test for io (test_io.iron) deliberately avoids this by testing only `IO.file_exists()`, which works correctly. This means STD-02 is partially satisfied — the io module is callable and several of its functions work, but the key `read_file` function cannot be used from Iron source without triggering a C compile error.

The fix requires either:
1. Updating io.iron to reflect the correct multi-return type (if multi-return syntax is supported), or
2. Adding an auto-static dispatch shim that extracts `.value` from the result struct for callers expecting a single String return

All other success criteria are fully verified through both C unit tests and end-to-end integration tests. The overall integration test suite shows 11/11 tests passing, but the io test only covers `file_exists`, not `read_file`.

---

_Verified: 2026-03-26T22:00:00Z_
_Verifier: Claude (gsd-verifier)_
