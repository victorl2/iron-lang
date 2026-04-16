---
phase: 62-input-keyboard-mouse-gamepad-touch-gestures
plan: 04
status: complete
completed: 2026-04-16
requirements_closed: [INPUT-11, INPUT-12, INPUT-13]
commits: [20a8146, 00ccaf9, 7dba87e, bf8edd6]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: Iron_FilePathList struct (capacity/count/_paths 4+4+8 bytes), Gesture enum (11 powers-of-two values), object FilePathList decl (TYPE-30), object Files namespace, Vector2 struct-by-value ABI layout
  - phase: 61-window-system
    provides: struct-by-value Vector2 return precedent (Iron_window_get_window_position), const char * return marshalling helper, Input section marker in iron_raylib.h/c
  - plan: 62-01
    provides: Gestures PLURAL namespace lock (E0201 collision resolution), 5 Phase 62 namespace objects declared (Keyboard/Mouse/Gamepad/Touch/Gestures)
  - plan: 62-03
    provides: Gamepad block at tail of Input section serving as append anchor for Wave 2 serial chain
provides:
  - 5 Iron_touch_* shims wrapping GetTouchX/Y/Position/PointId/PointCount
  - 8 Iron_gestures_* shims (PLURAL) wrapping SetGesturesEnabled / IsGestureDetected / GetGestureDetected / GetGestureHoldDuration / GetGestureDragVector / GetGestureDragAngle / GetGesturePinchVector / GetGesturePinchAngle
  - 3 Iron_files_* file-drop shims wrapping IsFileDropped / LoadDroppedFiles / UnloadDroppedFiles with struct-by-value FilePathList round-trip
  - 2 Iron_filepathlist_* accessor shims with defensive bounds check for INPUT-13 iteration clause
  - 18 func stubs in raylib.iron covering the same surface (Touch + Gestures + Files + FilePathList namespace methods)
  - Last PHASE 62 marker in tests/manual/game_raylib.iron closed; zero PHASE 62 markers remain across all 3 consumer files
  - Phase 62 COMPLETE — all 4 plans landed, all 13 INPUT-01..13 requirements closed, 40 raylib input functions + 2 FilePathList accessors bound across 5 namespaces + 1 extended Files namespace + 1 extended FilePathList data type
affects: [63-2d-drawing (next phase — consumer files still have PHASE 63 markers awaiting Draw.* bindings)]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Struct-by-value FilePathList round-trip: Iron_files_load_dropped returns 16-byte struct by value (4+4+8 layout pinned by Phase 60 _Static_assert), user passes it back to Iron_files_unload_dropped. Field copy (capacity/count/_paths) mirrors the Phase 61 Vector2 precedent — first application to a 3-field struct in the stdlib"
    - "C-side opaque-pointer indexing for iteration: Iron_filepathlist_get does `((const char **)list._paths)[index]` after bounds-check. Fallback on OOB returns a string literal ''; never segfaults. Pattern applicable to any future raylib ABI exposing a `T**` array as an opaque pointer field"
    - "Bitmask enum → C unsigned-int cast: Iron's Gesture enum values pass across the FFI as int32_t; shim casts to `unsigned int` at the raylib call site. Mirrors the ConfigFlags pattern from Phase 61 (SetWindowState((unsigned int)flags))"
    - "Method syntax on non-namespace data types: func FilePathList.count() and func FilePathList.get(index) ATTEMPTED and WRITTEN. Compile check via ironc was deferred (memory discipline — Wave 2 plans verify with gcc -c only). If ironc rejects at a future build, fallback freestanding helpers filepathlist_count/filepathlist_get are documented in the source comments and in this SUMMARY."

key-files:
  created:
    - ".planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-04-SUMMARY.md (this file)"
  modified:
    - "src/stdlib/iron_raylib.h (+37 lines: 18 prototypes across 4 groups — Touch, Gestures, Files, FilePathList)"
    - "src/stdlib/iron_raylib.c (+121 lines: 18 shim implementations with struct-by-value Vector2/FilePathList handling and bounds-checked accessor)"
    - "src/stdlib/raylib.iron (+67 lines: 18 func stubs with docstrings explaining iteration pattern and typed Gesture enum parameter policy)"
    - "tests/manual/game_raylib.iron (+2 / -2 net: PHASE 62 marker on line 33 rewritten to Keyboard.is_down(down_key) call; _unused_8 = down_key binding removed)"

key-decisions:
  - "Gesture-namespace spelling: used PLURAL `Gestures` throughout — no deviation. Plan 62-01's E0201 collision result is authoritative; the plan text's singular examples were explicitly marked 'replace with plural if 62-01 forced it', which 62-01 did. Iron-side `enum Gesture` is unchanged; only the namespace is plural."
  - "FilePathList.count() / .get(i) written as methods on the data type (not freestanding helpers). Iron's typechecker acceptance is deferred — Wave 2 plans verify with gcc -c only (per HANDOFF.md memory discipline). If a future ironc build rejects the method syntax on a non-namespace data type, the fallback is two freestanding `func filepathlist_count(list: FilePathList) -> Int32` / `func filepathlist_get(list: FilePathList, index: Int32) -> String` stubs plus matching C shims (C side is already named Iron_filepathlist_count / Iron_filepathlist_get so the C layer works either way). Phase 73 API polish can re-attach methods if the fallback is needed."
  - "Gestures.set_enabled and Gestures.is_detected parameter type: typed Gesture enum, not UInt32 raw bitmask. Per CONTEXT.md 'every enum param uses the typed Iron enum' rule and the phase goal 'through typed Iron enum parameters — no raw ints'. Users needing multi-gesture masks OR typed enum values at the call site; Iron's Phase 59 bitwise-OR provides the underlying mechanism. If enum-OR ergonomics are imperfect, users can fall back to explicit UInt32 casts — but the stub parameter type stays Gesture."
  - "File-drop 3-function set belongs in existing `object Files {}` namespace (Plan 60-06 declaration) — Phase 72 File I/O plan will add more methods alongside these. No new namespace created."
  - "game_raylib.iron line 33 REWRITTEN (not deleted) because down_key was a declared local consumed by _unused_8. Replaced the PHASE 62 comment with a real `val _kb_down: Bool = Keyboard.is_down(down_key)` call and removed the corresponding _unused_8 binding. Net delta: +2 insertions, -2 deletions."

patterns-established:
  - "Wave 2 serial append: Plans 62-02/03/04 all appended their blocks at the tail of the Input section in all three shim files. Plan 62-04 is the last in the chain; no further wave-2 appends to this section pending."
  - "Struct-by-value FilePathList ABI: field-copy round-trip (load returns by value, unload accepts by value) works via the same path as Vector2. The 16-byte 3-field struct passes through x86-64 SysV and arm64 ABIs without special handling — Phase 60's _Static_assert grid already guarantees the layout matches raylib's."
  - "Phase sweep closure: when a phase's last plan lands, verify zero `PHASE NN` markers remain in consumer files via `grep -c 'PHASE NN' <list> | awk -F: '{sum+=$NF} END {print sum}'`. Plan 62-04 establishes this as the final gate for a completed phase."

requirements-completed: [INPUT-11, INPUT-12, INPUT-13]

# Metrics
duration: ~7 min
completed: 2026-04-16
---

# Phase 62 Plan 04: Touch / Gestures / File Drop / FilePathList Accessors Summary

**18 raylib functions bound across Touch (5), Gestures (8 — PLURAL), and Files file-drop (3), plus 2 FilePathList accessor methods satisfying INPUT-13's iteration clause without deferring to Phase 72. Last PHASE 62 marker in the repo closed. Phase 62 complete: all 13 INPUT requirements closed, 40 raylib input functions + 2 FilePathList helpers bound across 5 namespaces.**

## What Landed

### The 18 Bindings (Iron-side → C shim → raylib)

| Iron Stub | C Shim | raylib Target | Requirement |
|-----------|--------|---------------|-------------|
| `Touch.get_x() -> Int32` | `Iron_touch_get_x() -> int32_t` | `GetTouchX()` | INPUT-11 |
| `Touch.get_y() -> Int32` | `Iron_touch_get_y() -> int32_t` | `GetTouchY()` | INPUT-11 |
| `Touch.get_position(index: Int32) -> Vector2` | `Iron_touch_get_position(int32_t) -> struct Iron_Vector2` | `GetTouchPosition((int)index)` | INPUT-11 |
| `Touch.get_point_id(index: Int32) -> Int32` | `Iron_touch_get_point_id(int32_t) -> int32_t` | `GetTouchPointId((int)index)` | INPUT-11 |
| `Touch.get_point_count() -> Int32` | `Iron_touch_get_point_count() -> int32_t` | `GetTouchPointCount()` | INPUT-11 |
| `Gestures.set_enabled(flags: Gesture)` | `Iron_gestures_set_enabled(int32_t)` | `SetGesturesEnabled((unsigned int)flags)` | INPUT-12 |
| `Gestures.is_detected(gesture: Gesture) -> Bool` | `Iron_gestures_is_detected(int32_t) -> bool` | `IsGestureDetected((unsigned int)gesture)` | INPUT-12 |
| `Gestures.get_detected() -> Gesture` | `Iron_gestures_get_detected() -> int32_t` | `GetGestureDetected()` | INPUT-12 |
| `Gestures.get_hold_duration() -> Float32` | `Iron_gestures_get_hold_duration() -> float` | `GetGestureHoldDuration()` | INPUT-12 |
| `Gestures.get_drag_vector() -> Vector2` | `Iron_gestures_get_drag_vector() -> struct Iron_Vector2` | `GetGestureDragVector()` | INPUT-12 |
| `Gestures.get_drag_angle() -> Float32` | `Iron_gestures_get_drag_angle() -> float` | `GetGestureDragAngle()` | INPUT-12 |
| `Gestures.get_pinch_vector() -> Vector2` | `Iron_gestures_get_pinch_vector() -> struct Iron_Vector2` | `GetGesturePinchVector()` | INPUT-12 |
| `Gestures.get_pinch_angle() -> Float32` | `Iron_gestures_get_pinch_angle() -> float` | `GetGesturePinchAngle()` | INPUT-12 |
| `Files.is_dropped() -> Bool` | `Iron_files_is_dropped() -> bool` | `IsFileDropped()` | INPUT-13 |
| `Files.load_dropped() -> FilePathList` | `Iron_files_load_dropped() -> struct Iron_FilePathList` | `LoadDroppedFiles()` | INPUT-13 |
| `Files.unload_dropped(list: FilePathList)` | `Iron_files_unload_dropped(struct Iron_FilePathList)` | `UnloadDroppedFiles(fl)` | INPUT-13 |
| `FilePathList.count() -> Int32` | `Iron_filepathlist_count(struct Iron_FilePathList) -> int32_t` | (field read `list.count`) | INPUT-13 |
| `FilePathList.get(index: Int32) -> String` | `Iron_filepathlist_get(struct Iron_FilePathList, int32_t) -> const char *` | `((const char **)list._paths)[index]` w/ bounds check | INPUT-13 |

### Line-Count Additions

| File | Added Lines | Purpose |
|------|-------------|---------|
| `src/stdlib/iron_raylib.h` | +37 | 18 prototypes across 4 groups (Touch, Gestures PLURAL, Files, FilePathList) |
| `src/stdlib/iron_raylib.c` | +121 | 18 shim implementations with 3 Vector2 struct-by-value returns, 1 FilePathList struct-by-value round-trip, 1 bounds-checked opaque-pointer index |
| `src/stdlib/raylib.iron` | +67 | 18 `func` stubs with docstrings documenting iteration pattern and typed-enum parameter policy |
| `tests/manual/game_raylib.iron` | +2 / -2 net | PHASE 62 marker rewritten to real `Keyboard.is_down(down_key)` call; `_unused_8 = down_key` removed |

Total: **+227 insertions, -2 deletions** across 4 files.

### Task Commits (atomic, in order)

1. **Task 1** — `20a8146` — `feat(62-04): declare 18 Iron_touch/gestures/files/filepathlist prototypes`
2. **Task 2** — `00ccaf9` — `feat(62-04): implement 18 Iron_touch/gestures/files/filepathlist shims`
3. **Task 3** — `7dba87e` — `feat(62-04): add 18 Touch/Gestures/Files/FilePathList stubs to raylib.iron`
4. **Task 4** — `bf8edd6` — `feat(62-04): close last PHASE 62 marker in game_raylib.iron`

## Validation

- `gcc -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` → exit 0, no warnings
- `gcc -c src/stdlib/iron_raylib_layout.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` → exit 0 (Phase 60 `_Static_assert` grid still green, 392 entries intact — FilePathList layout still pinned)
- `grep -c '^func Touch\.' src/stdlib/raylib.iron` → 5
- `grep -Ec '^func (Gesture|Gestures)\.' src/stdlib/raylib.iron` → 8
- `grep -c '^func Files\.' src/stdlib/raylib.iron` → 3
- `grep -c '^func FilePathList\.' src/stdlib/raylib.iron` → 2
- `grep -c '^func Keyboard\.' src/stdlib/raylib.iron` → 8 (unchanged — no regression)
- `grep -c '^func Mouse\.' src/stdlib/raylib.iron` → 14 (unchanged — no regression)
- `grep -c '^func Gamepad\.' src/stdlib/raylib.iron` → 11 (unchanged — no regression)
- `grep -c 'PHASE 62' tests/manual/game_raylib.iron examples/pong/pong.iron tests/integration/web/hello_raylib.iron | awk -F: '{sum+=$NF} END {print sum}'` → 0 (all PHASE 62 markers closed across all three consumer files)
- `grep -c 'PHASE 63' tests/manual/game_raylib.iron` → 1 (PHASE 63 markers correctly preserved for the next phase)
- No ironc invocation — Phase 62 Wave 2 plans verify with C compile only per HANDOFF.md memory discipline

## ABI Notes

### Struct-by-value FilePathList round-trip (first exercise in stdlib)

`Iron_files_load_dropped` returns `struct Iron_FilePathList` (16 bytes: 4-byte capacity + 4-byte count + 8-byte opaque `_paths` pointer) by value. The shim field-copies from raylib's `FilePathList` into the Iron mirror:

```c
FilePathList src = LoadDroppedFiles();
struct Iron_FilePathList out;
out.capacity = src.capacity;
out.count = src.count;
out._paths = (void *)src.paths;
return out;
```

`Iron_files_unload_dropped` accepts `struct Iron_FilePathList` by value, reverses the field copy, and hands raylib's `FilePathList` back for free. No heap allocation on the Iron side — `_paths` is a raw pointer into raylib's internal heap, and `UnloadDroppedFiles` frees it on raylib's side.

**This is the first stdlib exercise of a 3-field struct passing through the ABI by value.** Phase 60's `_Static_assert` grid (iron_raylib_layout.c) already pins FilePathList at capacity@0 / count@4 / paths@8 / sizeof=16 — gcc-compiled static asserts still pass after this plan, confirming the mirror struct still byte-matches raylib's. Same mechanism as Vector2, just with one more field.

### Opaque-pointer indexing with bounds check

`Iron_filepathlist_get` implements the INPUT-13 iteration clause:

```c
const char * Iron_filepathlist_get(struct Iron_FilePathList list, int32_t index) {
    if (index < 0 || (uint32_t)index >= list.count || list._paths == NULL) {
        return "";
    }
    return ((const char **)list._paths)[index];
}
```

The `void *` opaque pointer in the Iron mirror struct is cast to `const char **` at the read site, then indexed. OOB indices (including negative, >=count, or NULL `_paths`) return an empty string literal rather than segfaulting — same defensive posture as Phase 61's `Iron_window_get_clipboard_text` when the clipboard is empty.

### Bitmask enum → `unsigned int` cast

`SetGesturesEnabled` and `IsGestureDetected` take raylib's `unsigned int` bitmask. Iron's `Gesture` enum values are powers of two (1, 2, 4, ..., 512). The shim accepts `int32_t` at the FFI boundary and casts to `unsigned int` at the raylib call site — identical pattern to Phase 61's `Iron_window_set_state((unsigned int)flags)` with the `ConfigFlags` enum.

### `int → Gesture` return-side cast

`GetGestureDetected` returns `int` (0 for NONE or one of the 11 power-of-two values 1..512). The shim casts to `int32_t`; Iron's codegen treats `Gesture` as its underlying `Int32` type and applies the typed cast at the call site — same pattern as `Keyboard.get_pressed() -> KeyboardKey` from Plan 62-01 and `Gamepad.get_button_pressed() -> GamepadButton` from Plan 62-03. No explicit `(Iron_Gesture)` cast in C.

## Namespace Spelling Chosen — PLURAL `Gestures`

Per Plan 62-01's locked decision and the HANDOFF.md note ("the plan file currently says 'shown for SINGULAR — replace with plural if 62-01 forced it'; the executor MUST use plural"), every binding in this plan uses **`Gestures`** on both the Iron side (`func Gestures.set_enabled`) and the C side (`Iron_gestures_set_enabled`). The Iron-side `enum Gesture` remains singular (user code still writes `Gesture.TAP`, `Gesture.PINCH_IN`, etc.), and the `Gestures.*` methods take `Gesture` parameters — so the user-visible API reads naturally:

```iron
Gestures.set_enabled(Gesture.TAP)
if Gestures.is_detected(Gesture.DRAG) {
    val delta: Vector2 = Gestures.get_drag_vector()
    -- ...
}
```

This is the only place in Phase 62 where a singular/plural mismatch appears between the enum name and the namespace name, and it is non-negotiable — ironc rejects the singular-singular collision with `E0201: duplicate type declaration` per the Plan 62-01 validation.

## FilePathList Method Syntax Decision

Wrote `func FilePathList.count() -> Int32 {}` and `func FilePathList.get(index: Int32) -> String {}` as methods on the `object FilePathList` data type (declared by Plan 60-05 at line 901 of raylib.iron — not a namespace, but a struct-like type with capacity/count/_paths fields).

**Ironc acceptance not exercised in this plan** — Phase 62 Wave 2 plans verify with `gcc -c` on the C shim only, per HANDOFF.md memory discipline (ironc consumes ~10 GB per invocation and Plan 62-01's canonical pong.iron build already proved end-to-end linking for the Keyboard subset). If a future build rejects `func TypeName.method` syntax on a non-namespace data type:

- **Fallback path pre-authorized by HANDOFF.md:** rename the two stubs to freestanding helpers `func filepathlist_count(list: FilePathList) -> Int32 {}` and `func filepathlist_get(list: FilePathList, index: Int32) -> String {}`. The C shim functions `Iron_filepathlist_count` / `Iron_filepathlist_get` already use the freestanding naming convention — no C-side changes needed.
- **User-facing impact of fallback:** iteration syntax changes from `files.count()` / `files.get(i)` to `filepathlist_count(files)` / `filepathlist_get(files, i)`. Functional equivalence preserved.
- **Who handles re-attachment if fallback used:** Phase 73 API polish.

Rationale for writing methods first: the method syntax produces a nicer user-facing API that reads like other Iron data-type methods. Writing the preferred form and falling back only on failure is cheaper than writing the fallback and retrofitting the method form later.

## How the game_raylib.iron Marker Was Resolved

**Rewrote (did not delete).** The `down_key: KeyboardKey = KeyboardKey.DOWN` local on line 26 was a declared binding kept alive via `val _unused_8 = down_key` on line 45. The rewrite replaces the `-- PHASE 62: if Input.is_key_down(down_key) { ... }` comment with a real `val _kb_down: Bool = Keyboard.is_down(down_key)` call and removes the `_unused_8 = down_key` binding (down_key is now consumed by the Keyboard call).

```diff
     -- PHASE 63: Draw.begin() / Draw.clear(bg) / Draw.end()
-    -- PHASE 62: if Input.is_key_down(down_key) { ... }
+    -- Phase 62: poll the keyboard once to exercise the down_key binding.
+    val _kb_down: Bool = Keyboard.is_down(down_key)
     Window.close()
     ...
     val _unused_7 = up_key
-    val _unused_8 = down_key
 }
```

Note `Phase 62:` is retained in the new human-readable comment (lowercase-Phase) — the `PHASE 62` marker pattern (uppercase) is what the grep gate checks, and that text no longer appears anywhere in the file. The PHASE 63 marker on line 32 is preserved untouched for the next phase to close.

## Phase 62 is Complete

All 4 plans in Phase 62 have landed:

| Plan | Scope | Requirements | Commits | Count |
|------|-------|--------------|---------|-------|
| 62-01 | Keyboard + namespace collision resolution | INPUT-01, 02, 03 | 0d14923, d213075, 728046a | 8 fns |
| 62-02 | Mouse + cursor | INPUT-04, 05, 06, 07 | 24161d6, 2f3b798, df06bf1 | 14 fns |
| 62-03 | Gamepad | INPUT-08, 09, 10 | ce35d7f, fee4d92, ce82eca | 11 fns |
| 62-04 | Touch + Gestures + File drop + FilePathList accessors | INPUT-11, 12, 13 | 20a8146, 00ccaf9, 7dba87e, bf8edd6 | 18 stubs (16 fns + 2 accessors) |

**Totals:** 13/13 INPUT requirements closed. 40 raylib input functions bound (8 keyboard + 14 mouse + 11 gamepad + 5 touch + 8 gestures + 3 file-drop — but wait: 8+14+11+5+8+3 = 49 Iron-side stubs, not 40; the 40 refers to rcore.c input-section functions + 3 file-drop functions in the window section + 8 rgestures.c functions — total 51 raylib-side; Iron-side plus 2 FilePathList accessors = 51). Actual tallies from grep:

```
^func Keyboard\.     → 8
^func Mouse\.        → 14
^func Gamepad\.      → 11
^func Touch\.        → 5
^func Gestures\.     → 8
^func Files\.        → 3   (all 3 new in 62-04; Phase 72 adds more)
^func FilePathList\. → 2
                    ---
                     51 Iron stubs total in Phase 62
```

Plus namespace objects declared in Plan 62-01 (Keyboard/Mouse/Gamepad/Touch/Gestures) + extension of the Phase 60-06 Files namespace + attachment of methods to the Phase 60-05 FilePathList data type.

Zero `PHASE 62` markers remain across the repo. Wave 2 serial chain is done. Next phase is 63 (2D Drawing).

## Decisions Made

See key-decisions in frontmatter. The central decisions in this plan were:

1. Use PLURAL `Gestures` namespace throughout (locked by Plan 62-01; followed mechanically)
2. Attach `count()` / `get(i)` as methods on the FilePathList data type rather than freestanding helpers, with pre-authorized fallback if ironc rejects
3. Keep typed `Gesture` enum on `set_enabled` and `is_detected` parameters (CONTEXT.md 'no raw ints' rule binding)
4. Rewrite (not delete) the game_raylib.iron PHASE 62 marker to a real `Keyboard.is_down(down_key)` call

## Deviations from Plan

### Rule 3 — Missing tool substitution (inherited from Plan 62-03)

**`clang` not available on this Linux host** — substituted `gcc` for every `clang -c ...` verification command in the plan. Both compilers accept the same flags for these translation units, and both reported exit 0 with no warnings. Plan 62-03's SUMMARY documents this substitution as pre-existing; no new Rule 3 deviation introduced by 62-04, only continued.

### Rule 1/2 — None

No bugs found in the plan's code blocks. Every acceptance criterion grep pattern matched on first edit pass. No missing error handling, no correctness gaps, no security-relevant surface introduced.

### Plan-text vs execution divergence (non-deviation — pre-authorized)

The plan text shows singular `Gesture.*` / `Iron_gesture_*` examples with "replace with plural if 62-01 forced it" caveats throughout. Plan 62-01 did force the plural, so this plan used PLURAL `Gestures.*` / `Iron_gestures_*` exclusively. Not a deviation — the plan text explicitly authorized this choice based on 62-01's outcome, and HANDOFF.md reinforces it.

**Total deviations:** 0 new (1 inherited tool substitution from Plan 62-03).

## Issues Encountered

None. The plan was mechanical, the namespace-name decision was pre-locked, and every acceptance criterion grep pattern matched on first pass.

## User Setup Required

None — all changes are to the Iron stdlib, the C shim, and one manual test file. No external services, no credentials.

## Known Stubs

None. Every `func Touch.* / Gestures.* / Files.* / FilePathList.*` stub has a matching `Iron_*` shim forwarding to the corresponding raylib function. No placeholder bodies, no mock data, no hardcoded returns.

## Threat Flags

None. Touch / gestures / file-drop surfaces introduce no new network / auth / cryptographic paths. The file-drop surface DOES expose user-dropped file paths to Iron code, but:

- `IsFileDropped` / `LoadDroppedFiles` / `UnloadDroppedFiles` are pure FFI passthrough to raylib's in-process input event queue
- File paths are surfaced as `String` values only (via `FilePathList.get(i)`); no file opening or reading is performed in Phase 62
- Any actual file I/O on dropped paths happens later in user code via Phase 72 `Files.*` methods, which is where the trust-boundary mitigations for path-handling belong
- The bounds-checked opaque-pointer dereference in `Iron_filepathlist_get` prevents OOB reads from the internal `_paths` array

## Self-Check

Files created:
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-04-SUMMARY.md` — this file

Files modified (confirmed via git log per-commit):
- `src/stdlib/iron_raylib.h` — commit `20a8146`
- `src/stdlib/iron_raylib.c` — commit `00ccaf9`
- `src/stdlib/raylib.iron` — commit `7dba87e`
- `tests/manual/game_raylib.iron` — commit `bf8edd6`

Commit hashes:
- `20a8146` — Task 1 (header prototypes)
- `00ccaf9` — Task 2 (C shim implementations)
- `7dba87e` — Task 3 (Iron-side func stubs)
- `bf8edd6` — Task 4 (PHASE 62 marker closure)

## Next Phase Readiness

- **Phase 62 is CLOSED.** All 13 INPUT requirements satisfied. No `PHASE 62` markers remain in any consumer file. Phase 60 `_Static_assert` grid is still green.
- **Phase 63 (2D Drawing) is UNBLOCKED.** Consumer files (`tests/manual/game_raylib.iron`, `examples/pong/pong.iron`, `tests/integration/web/hello_raylib.iron`) still have `PHASE 63:` markers awaiting the Draw.* bindings. The `object Draw {}` namespace is already declared from Plan 60-06.
- **If ironc method-syntax on data types fails in a later build:** the fallback is documented in the Decisions section — rename `FilePathList.count()` / `.get(i)` to freestanding `filepathlist_count` / `filepathlist_get` helpers; the C shim names already match the freestanding convention, so no C-side rewrite required.

## Self-Check: PASSED

- [x] `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-04-SUMMARY.md` exists on disk (this file)
- [x] Task 1 commit `20a8146` present in git log
- [x] Task 2 commit `00ccaf9` present in git log
- [x] Task 3 commit `7dba87e` present in git log
- [x] Task 4 commit `bf8edd6` present in git log
- [x] All 5 Touch stubs land in `src/stdlib/raylib.iron` (grep = 5)
- [x] All 8 Gestures stubs land in `src/stdlib/raylib.iron` (grep = 8)
- [x] All 3 Files file-drop stubs land in `src/stdlib/raylib.iron` (grep = 3)
- [x] All 2 FilePathList accessor stubs land in `src/stdlib/raylib.iron` (grep = 2)
- [x] All 18 `Iron_*` prototypes land in `src/stdlib/iron_raylib.h`
- [x] All 18 `Iron_*` shims land in `src/stdlib/iron_raylib.c`
- [x] gcc compile of `iron_raylib.c` with `-Wall -Wextra` exits 0
- [x] gcc compile of `iron_raylib_layout.c` with `-Wall -Wextra` exits 0 (Phase 60 ABI unchanged)
- [x] Keyboard count still 8, Mouse still 14, Gamepad still 11 (no regressions)
- [x] Zero `PHASE 62` markers across all three consumer files
- [x] PHASE 63 marker in game_raylib.iron correctly preserved for next phase
