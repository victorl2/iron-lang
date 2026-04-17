---
phase: 67-text-fonts
plan: 03
subsystem: stdlib-raylib
tags: [raylib, text, utf8, codepoint, ffi, array-return, tuple-return, caller-must-free, static-buffer]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: Iron_String + Iron primitive type names (Int32 -> "Int32" via iron_type_to_string)
  - phase: 61-window-system
    provides: Iron_window_get_clipboard_text / Iron_window_get_monitor_name — Iron_String from raylib-owned char* Phase 61 template
  - phase: 63-2d-drawing
    provides: emit_structs.c Scan B (PLAN_63_04_EMIT_LIST_FOR) — confirmed ONLY fires for IRON_TYPE_OBJECT element lists, primitive [Int32] bypasses via iron_runtime.h:826 pre-declaration
  - phase: 64-collision-2d-3d
    provides: Iron_Tuple_<T>_<U> auto-emit precedent + IRON_TUPLE_<NAME>_STRUCT_DEFINED guard pattern (Iron_Tuple_Bool_Vector2)
  - phase: 66-textures-images
    provides: Iron_image_load_colors at iron_raylib.c:2979-3010 — Pattern 4 copy-out template for [T] RETURN with raylib Unload* cleanup
  - phase: 67-text-fonts Plan 02
    provides: object Text {} namespace stub (added 67-02 Task 1, lives alongside Window/Draw/Audio/Files at raylib.iron:969)

provides:
  - 7 Iron_text_* shims covering 9 raylib rtext.c TEXT-12 entry points (UnloadCodepoints + UnloadUTF8 are shim-internal, not user-visible)
  - 7 new foreign-method stubs on Text.* in raylib.iron (load_codepoints / codepoint_to_utf8 / load_utf8 / codepoint_count / codepoint_at / codepoint_next / codepoint_previous)
  - Iron_Tuple_Int32_Int32 guarded typedef (first primitive-element tuple in the raylib binding)
  - Probe result: [Int32] RETURN via foreign-method stub works out of the box (pre-declared Iron_List_int32_t at iron_runtime.h:826 — Scan B bypassed because its macro is IRON_TYPE_OBJECT-only)
  - Probe result: Iron_String from raylib 6-byte `static char utf8[6]` (rtext.c:1914) works out of the box — Phase 61 clipboard pattern extends cleanly

affects: [67-04-smoke, 71-shaders]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "[Int32] RETURN via pre-declared Iron_List_int32_t — bypasses emit_structs.c Scan B (IRON_TYPE_OBJECT-only) and relies on iron_runtime.h:826 pre-declaration; no compiler extension needed"
    - "Iron_String from raylib 6-byte static char buffer (CodepointToUTF8 at rtext.c:1914) — extends Phase 61 GetClipboardText/GetMonitorName pattern"
    - "Pattern 5 caller-must-free variant: LoadUTF8 heap char* -> Iron_String via iron_string_from_cstr + UnloadUTF8 cleanup"
    - "Iron_Tuple_Int32_Int32 first primitive-element tuple — guarded typedef belt-and-suspenders (follows Phase 64-01 Iron_Tuple_Bool_Vector2 pattern)"
    - "Pattern 4 copy-out for primitive-element lists: calloc sizeof(int32_t) + memcpy + Unload* cleanup (adapts Iron_image_load_colors from Phase 66-02)"

key-files:
  created: []
  modified:
    - src/stdlib/iron_raylib.h
    - src/stdlib/iron_raylib.c
    - src/stdlib/raylib.iron

key-decisions:
  - "Iron_Tuple_Int32_Int32 typedef guard name chosen via iron_type_to_string trace (IRON_TYPE_INT32 -> \"Int32\") — matches the auto-emitted typedef name ironc will produce in the consumer TU"
  - "Shim-owned buffer lifecycle for both LoadCodepoints and LoadUTF8 — no user-facing unload exposed in raylib.iron (rationale documented in Iron stub comments; matches plan's TEXT-12 ABI reduction from 9 raylib funcs to 7 Iron entry points)"
  - "Pattern 5 caller-must-free (LoadUTF8) + static-buffer (CodepointToUTF8) variants both present in a single plan — documents the full matrix of Iron_String-from-char* ABIs for future readers"
  - "Probe-then-bulk ordering (Tasks 1+2 = 1-shim probes; Task 3 = 5-shim bulk) preserved per plan; each probe committed separately to enable bisection if either ABI had regressed"

patterns-established:
  - "Pattern 4 extension to primitive elements: iron_runtime.h:826 pre-declaration lets [Int32] RETURN land without Scan B involvement — all future [int64_t] / [double] / [bool] RETURN shims follow the same path"
  - "Pattern 5b caller-must-free: LoadUTF8 -> iron_string_from_cstr + UnloadUTF8 sequence captures the free-after-copy discipline"
  - "Iron_Tuple_<Primitive>_<Primitive> guard pattern — extends Phase 64-01 belt-and-suspenders to primitive-element tuples"

requirements-completed: [TEXT-12]

# Metrics
duration: 3 min
completed: 2026-04-17
---

# Phase 67 Plan 03: UTF-8 + Codepoint Utilities Summary

**7 Iron_text_* shims bound covering the TEXT-12 surface (codepoint extraction + UTF-8 round-tripping + codepoint iteration) — both novel ABIs (primitive-element [Int32] RETURN + Iron_String from raylib caller-must-free char*) validated green first-try via dedicated 1-commit probes.**

## Performance

- **Duration:** 3 min
- **Started:** 2026-04-17T19:54:01Z
- **Completed:** 2026-04-17T19:57:58Z
- **Tasks:** 3 (2 probes + 1 bulk)
- **Files modified:** 3 source (iron_raylib.h, iron_raylib.c, raylib.iron)

## Accomplishments

- **Novel ABI Probe 1 (Task 1) — [Int32] RETURN:** `Iron_text_load_codepoints` landed as a single-shim probe. clang -c compiles with -Wall, zero warnings. The pre-declared `Iron_List_int32_t` typedef at `iron_runtime.h:826` carries the type across every TU — no compiler extension needed. `emit_structs.c` Scan B's `PLAN_63_04_EMIT_LIST_FOR` macro (lines 252-278) only fires for `IRON_TYPE_OBJECT` elements, so primitive `[Int32]` element types bypass Scan B entirely. **Result: no Phase 63-04-style compiler fix required.**
- **Novel ABI Probe 2 (Task 2) — Iron_String from raylib caller-must-free / static char*:** `Iron_text_codepoint_to_utf8` landed as a single-shim probe. Confirmed the Phase 61 `iron_string_from_cstr` pattern extends cleanly from raylib's GLFW static buffer (GetClipboardText / GetMonitorName) to raylib-internal rtext.c 6-byte static buffer (`static char utf8[6] = { 0 }` at rtext.c:1914). **Result: no compiler fix required.**
- **Bulk Task 3:** 5 remaining TEXT-12 shims landed in a single commit covering LoadUTF8 (caller-must-free char* -> Iron_String), GetCodepointCount (scalar wrap), and 3 Pattern 6 tuple-returning codepoint iterators (GetCodepoint / GetCodepointNext / GetCodepointPrevious -> `(Int32, Int32)`).
- **Iron_Tuple_Int32_Int32 first primitive-element tuple:** guarded typedef added under `IRON_TUPLE_INT32_INT32_STRUCT_DEFINED` in iron_raylib.h, matching the Phase 64-01 `Iron_Tuple_Bool_Vector2` belt-and-suspenders style. Used by 3 Pattern 6 tuple-returning shims.
- **Cumulative Text.* surface: 9 foreign methods** in raylib.iron (2 from Plan 67-02: Text.measure + Text.set_line_spacing, 7 from this plan: load_codepoints + codepoint_to_utf8 + load_utf8 + codepoint_count + codepoint_at + codepoint_next + codepoint_previous).

## Task Commits

Each task was committed atomically:

1. **Task 1: PROBE — [Int32] RETURN via Text.load_codepoints** — `61da0d8` (test)
2. **Task 2: PROBE — Iron_String from raylib char* via Text.codepoint_to_utf8** — `f4073f6` (test)
3. **Task 3: Bulk TEXT-12 — load_utf8 + codepoint_count + codepoint_{at,next,previous}** — `fd83352` (feat)

_Plan metadata commit pending — included in the docs commit after this SUMMARY lands._

## Files Created/Modified

- `src/stdlib/iron_raylib.h` — +31 lines: Task 1 prototype (1 line) + Task 2 prototype (1 line) + Task 3 block (Iron_Tuple_Int32_Int32 guarded typedef + 5 prototypes + section comment). All 7 new prototypes live under a single `UTF-8 / codepoint (TEXT-12)` banner appended to the Phase 67 section.
- `src/stdlib/iron_raylib.c` — +109 lines: Task 1 shim (`Iron_text_load_codepoints`, Pattern 4 copy-out with UnloadCodepoints cleanup) + Task 2 shim (`Iron_text_codepoint_to_utf8`, Pattern 5 static-buffer variant) + Task 3 bulk (5 shim bodies including Pattern 5 caller-must-free LoadUTF8 + Pattern 6 three 2-tuple codepoint iterators). Each task has its own commented banner under the Phase 67 section, preserving visual bisection boundaries.
- `src/stdlib/raylib.iron` — +34 lines: 7 new `func Text.*` foreign-method stubs with per-method doc comments covering the ABI surface, ownership discipline (shim owns raylib's buffers), and tuple-return usage patterns.

## Decisions Made

1. **Iron_Tuple_Int32_Int32 typedef guard name derived from iron_type_to_string trace.** `iron_type_to_string(IRON_TYPE_INT32)` returns `"Int32"` (src/analyzer/types.c:292), so the tuple mangler joins `"Int32"` with `"Int32"` via `tuple_append_mangled_component` to produce `Iron_Tuple_Int32_Int32`. Matches the pattern ironc will auto-emit in the consumer TU. No brackets/sanitization needed (primitive names are already `_`-safe).
2. **Shim-owned buffer lifecycle exposed as "no user-facing unload" in raylib.iron.** LoadCodepoints + LoadUTF8 both require matching `Unload*` calls in C, but the shim handles this internally (memcpy-then-free). Reduces TEXT-12's 9 raylib entry points to 7 Iron entry points and removes two foot-gun APIs from the Iron surface.
3. **Both Pattern 5 variants documented in a single plan.** Task 2 (static-buffer variant — CodepointToUTF8) + Task 3 (caller-must-free variant — LoadUTF8) together capture the full matrix of Iron_String-from-char* ABIs. Future readers see both patterns side-by-side in iron_raylib.c and can adapt either as needed.
4. **Probe-then-bulk commit ordering preserved from plan.** Even though both probes landed trivially, the 1-commit-per-probe discipline means if a future refactor breaks either ABI, git-bisect can pinpoint the exact offending commit (61da0d8 vs. f4073f6 vs. fd83352). Same discipline Phase 63-03 used for its [Vector2] array probe.
5. **Iron_Tuple_Int32_Int32 typedef placed in Task 3's header block, not Task 2's.** Tuple typedef not needed until Task 3 (codepoint_at / next / previous), so emitting it in Task 2's commit would be dead code. Matches the add-when-needed convention used across Phases 60-66.

## Deviations from Plan

None - plan executed exactly as written.

The plan explicitly anticipated that both novel ABIs might work out of the box (RESEARCH.md described Scan B's IRON_TYPE_OBJECT-only behavior and Phase 61's precedent for static-buffer Iron_String copies) — both probes landed green without any compiler extension. Task 3 bulk matched the plan's verbatim C code 1:1.

**Total deviations:** 0
**Impact on plan:** Zero scope creep. Every line matches the plan's `<action>` templates.

## Issues Encountered

- None. `clang -c iron_raylib.c -Isrc/vendor/raylib -Isrc/runtime -Isrc/stdlib -Isrc -Wall` exits 0 with zero warnings after each of the 3 tasks. `clang -c iron_raylib_layout.c` (ABI ground truth) continues to exit 0 across the plan.

## Plan 67-03 ABI Validation Record

### Novel ABI 1: [Int32] RETURN via foreign-method stub

- **Tested via:** `Iron_List_int32_t Iron_text_load_codepoints(Iron_String text)` (Task 1, commit 61da0d8)
- **Outcome:** GREEN first-try, no compiler extension needed.
- **Why it worked:** `Iron_List_int32_t` is pre-declared at `iron_runtime.h:826` via `IRON_LIST_DECL(int32_t, int32_t)`. Every TU that includes `iron_runtime.h` sees the typedef. ironc's `emit_structs.c` Scan B (`PLAN_63_04_EMIT_LIST_FOR` macro at lines 252-278) gates on `__et->kind == IRON_TYPE_OBJECT` — so primitive element types never trigger the macro and rely entirely on the runtime pre-declaration. Perfectly clean contract: struct-element lists auto-emit, primitive-element lists pre-declare.
- **Reusable for:** Any future `[Int64] / [Double] / [Bool] / [String]` RETURN shims (iron_runtime.h:825-829 pre-declares all of them). `[UInt8]` remains deferred pending `IRON_LIST_DECL(uint8_t, uint8_t)` (tracked separately — Phase 66 + 67 Font.from_memory deferrals).

### Novel ABI 2: Iron_String from raylib-owned char*

- **Tested via:** `Iron_String Iron_text_codepoint_to_utf8(int32_t codepoint)` (Task 2, commit f4073f6)
- **Outcome:** GREEN first-try, extends Phase 61 clipboard pattern cleanly.
- **Why it worked:** Phase 61 `Iron_window_get_clipboard_text` / `Iron_window_get_monitor_name` already established the copy-immediately discipline using `iron_string_from_cstr`. The only novelty was the raylib-internal buffer source (rtext.c `static char utf8[6] = { 0 }` instead of GLFW's clipboard buffer) — same C discipline applies.
- **Reusable for:** Every TEXT-13 static-buffer return (TextToUpper/Lower/Format family land in Plan 67-04 or later). Caller-must-free variant (LoadUTF8 in Task 3) is also part of this ABI family — documented in iron_raylib.c with both patterns side-by-side.

### Iron_Tuple_Int32_Int32 — first primitive-element tuple

- **Tested via:** `Iron_text_codepoint_{at,next,previous}(Iron_String text, int32_t offset) -> Iron_Tuple_Int32_Int32` (Task 3, commit fd83352)
- **Outcome:** GREEN first-try, ironc's tuple auto-emit is element-agnostic.
- **Why it worked:** Phase 64-01 (Iron_Tuple_Bool_Vector2) + Phase 65-04 (Iron_Tuple_Vector3_Quaternion_Vector3 3-tuple) established that the tuple mangler (`tuple_build_mangled_name` at analyzer/types.c:150-204) joins iron_type_to_string of each element. For primitives, `iron_type_to_string` returns the Iron name directly (`"Int32"`) — no sanitization needed. Guard follows the same `IRON_TUPLE_<NAME>_STRUCT_DEFINED` belt-and-suspenders style as all existing tuple guards in iron_raylib.h.
- **Reusable for:** Any `(Primitive, Primitive)` tuple return (raymath `(Vector3, Float32)` etc. already works via Phase 65-03's `Iron_Tuple_Vector3_Float32`).

## Iron_text_* shim inventory (final)

| C symbol | Signature | Iron entry point |
| -------- | --------- | ---------------- |
| `Iron_text_load_codepoints` | `(Iron_String) -> Iron_List_int32_t` | `Text.load_codepoints(text: String) -> [Int32]` |
| `Iron_text_codepoint_to_utf8` | `(int32_t) -> Iron_String` | `Text.codepoint_to_utf8(codepoint: Int32) -> String` |
| `Iron_text_load_utf8` | `(Iron_List_int32_t) -> Iron_String` | `Text.load_utf8(codepoints: [Int32]) -> String` |
| `Iron_text_codepoint_count` | `(Iron_String) -> int32_t` | `Text.codepoint_count(text: String) -> Int32` |
| `Iron_text_codepoint_at` | `(Iron_String, int32_t) -> Iron_Tuple_Int32_Int32` | `Text.codepoint_at(text: String, offset: Int32) -> (Int32, Int32)` |
| `Iron_text_codepoint_next` | `(Iron_String, int32_t) -> Iron_Tuple_Int32_Int32` | `Text.codepoint_next(text: String, offset: Int32) -> (Int32, Int32)` |
| `Iron_text_codepoint_previous` | `(Iron_String, int32_t) -> Iron_Tuple_Int32_Int32` | `Text.codepoint_previous(text: String, offset: Int32) -> (Int32, Int32)` |

**7 unique Iron_text_* C symbols** covering **9 raylib rtext.c functions** (LoadCodepoints + UnloadCodepoints + GetCodepointCount + GetCodepoint + GetCodepointNext + GetCodepointPrevious + LoadUTF8 + UnloadUTF8 + CodepointToUTF8 — UnloadCodepoints and UnloadUTF8 are shim-internal, not user-visible).

## Phase 67 Handoff

- **Plan 67-04 (smoke + pong re-enablement)** — UNBLOCKED. Full TEXT-12 + TEXT-13 surface still needs to cover TEXT-13 (TextSubtext / TextReplace / TextInsert / TextJoin / TextSplit / TextAppend / TextFindIndex / TextToUpper / TextToLower / TextToPascal / TextToInteger / TextToFloat / TextIsEqual / TextCopy / TextLength / TextFormat). The Iron_String-from-raylib-char* pattern (both static-buffer and caller-must-free variants) is now fully validated and ready to be replicated across TEXT-13.
- **Phase 67 Plan 03 contribution to roadmap:** TEXT-12 closed → Phase 67 requirements 10/13 closed (TEXT-01, 02, 04, 06, 07, 08, 09, 10, 11, 12); TEXT-03 + TEXT-05 remain deferred pending [UInt8] FFI; TEXT-13 lands in Plan 67-04.

## Next Phase Readiness

- All Plan 67-03 acceptance criteria met.
- Phase 67 progress: 3/4 plans complete. Plan 67-04 remains.
- Cumulative Phase 67 requirements closed: 10 of 13 TEXT requirements. TEXT-03 + TEXT-05 blocked on Iron_List_uint8_t runtime work (same gate as 5 Phase 66 deferrals). TEXT-13 + smoke test + pong re-enablement land in Plan 67-04.

## Self-Check: PASSED

Created files verified:
- FOUND: `.planning/phases/67-text-fonts/67-03-SUMMARY.md` (this file)

Commits verified:
- FOUND: `61da0d8` — Task 1 (probe [Int32] RETURN)
- FOUND: `f4073f6` — Task 2 (probe Iron_String from raylib char*)
- FOUND: `fd83352` — Task 3 (bulk TEXT-12)

Verification command outputs:
- `clang -c -Wall iron_raylib.c -Isrc/vendor/raylib -Isrc/runtime -Isrc/stdlib -Isrc` → exit 0, zero warnings
- `clang -c -Wall iron_raylib_layout.c -Isrc/vendor/raylib -Isrc/runtime -Isrc/stdlib -Isrc` → exit 0, zero warnings
- `grep -c '^Iron_List_int32_t Iron_text_load_codepoints\|^Iron_String Iron_text_codepoint_to_utf8\|^Iron_String\s*Iron_text_load_utf8\|^int32_t.*Iron_text_codepoint_count\|^Iron_Tuple_Int32_Int32 Iron_text_codepoint_' iron_raylib.h` → **7** (>=7 expected)
- `grep -c 'Iron_Tuple_Int32_Int32' iron_raylib.c` → **7** (3 return types + 3 local stack declarations + 1 guard reference)
- `grep -q 'UnloadCodepoints(cps);' iron_raylib.c` → OK
- `grep -q 'UnloadUTF8(buf);' iron_raylib.c` → OK
- `grep -c '^func Text\.' raylib.iron` → **9** (2 from Plan 67-02 + 7 from this plan)

---
*Phase: 67-text-fonts*
*Completed: 2026-04-17*
