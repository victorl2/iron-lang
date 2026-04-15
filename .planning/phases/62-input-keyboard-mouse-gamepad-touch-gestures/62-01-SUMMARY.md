---
phase: 62-input-keyboard-mouse-gamepad-touch-gestures
plan: 01
subsystem: input
tags: [raylib, keyboard, ffi, shim, namespace-objects, pong]

# Dependency graph
requires:
  - phase: 60-type-enum-foundation
    provides: KeyboardKey enum (101 values, NULL=0/SPACE=32/W=87/S=83/UP=265/DOWN=264/ESCAPE=256), Gesture enum (bitmask), 4 existing namespace objects (Window/Draw/Audio/Files), pong.iron placeholder body with `-- PHASE 62:` markers
  - phase: 61-window-system
    provides: Window.init/close/should_close/set_target_fps bindings that pong.iron calls in its placeholder body, shim-only binding precedent, Iron_window_* enum-int-cast pattern (e.g. SetWindowState((unsigned int)flags)) that Plan 62-01 mirrors for IsKeyPressed((int)key)
provides:
  - 5 new namespace `object`s in src/stdlib/raylib.iron (Keyboard, Mouse, Gamepad, Touch, Gestures) ready to receive Plans 62-02/03/04 bindings
  - 8 Iron_keyboard_* shim wrappers forwarding to raylib's IsKeyPressed / IsKeyPressedRepeat / IsKeyDown / IsKeyReleased / IsKeyUp / GetKeyPressed / GetCharPressed / SetExitKey
  - 8 func Keyboard.* empty-body stubs in raylib.iron with typed KeyboardKey / Bool / Int32 signatures
  - Resolved Gesture-vs-Gesture name-collision question: Iron's module scope does NOT allow an enum and object to share a name — plural `object Gestures {}` is the canonical namespace for Plans 62-02/03/04
  - pong.iron end-to-end ironc build validated (2.5 MB Mach-O 64-bit arm64 binary) — canonical Phase 62 Wave 1 proof-of-work
affects: [62-02 mouse bindings, 62-03 gamepad bindings, 62-04 touch/gestures/file-drop bindings]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Shim-only keyboard binding pattern: Iron `func Keyboard.is_pressed(key: KeyboardKey) -> Bool {}` lowers to C `bool Iron_keyboard_is_pressed(int32_t key)` which forwards to raylib `IsKeyPressed((int)key)` — identical to Phase 61 Iron_window_set_state((unsigned int)flags) pattern"
    - "Return-side typed-enum cast: Iron `func Keyboard.get_pressed() -> KeyboardKey` + C `int32_t Iron_keyboard_get_pressed()` relies on Iron's enum-as-underlying-integer lowering — no explicit (KeyboardKey) cast in the shim, Iron's HIR handles the Int32-to-enum coercion at the call site"
    - "Enum-vs-namespace name collision is ILLEGAL in Iron (`E0201: duplicate type declaration`). Mitigation: use plural fallback (`Gestures`) when a namespace name would collide with an existing enum"

key-files:
  created: []
  modified:
    - "src/stdlib/raylib.iron (+63 lines: 5 namespace decls + Input section header + 8 func Keyboard.* stubs + collision-note comment block)"
    - "src/stdlib/iron_raylib.h (+11 lines: 8 Iron_keyboard_* prototypes)"
    - "src/stdlib/iron_raylib.c (+31 lines: 8 shim implementations with enum-int cast comments)"
    - "examples/pong/pong.iron (+22 insertions, -7 deletions net: 2 -- PHASE 62: comments replaced with 5 real Keyboard.* calls, 5 _unused_N bindings removed because those KeyboardKey locals are now consumed)"

key-decisions:
  - "Collision fallback: object Gesture {} collides with enum Gesture {} at module scope (ironc E0201). Plan 62-01 adopts plural object Gestures {} — matches raylib's internal rgestures.c module name. Iron-side enum Gesture is preserved unchanged."
  - "Gesture-vs-Gestures naming is NOW LOCKED for Plans 62-02/03/04. They MUST bind Gestures.* methods (plural), NOT Gesture.*. The rgestures module-name precedent makes the plural form readable without looking awkward."
  - "Deferred collision check to Task 3 (canonical pong.iron build) instead of running a separate probe iron file in Task 1. Rationale: ironc is ~10 GB per invocation, running it twice = 20 GB memory churn. One pong.iron build catches the collision AND validates the Keyboard bindings AND validates end-to-end link — strictly dominates the two-invocation path."
  - "Shim returns int32_t, not a Iron_KeyboardKey typedef. Iron's enum lowering treats KeyboardKey as its underlying integer (Int32), so func Keyboard.get_pressed() -> KeyboardKey {} lowers to Iron_keyboard_get_pressed() returning int32_t at the C boundary. No explicit (KeyboardKey) cast in C — the type-ness is an Iron HIR concern only."
  - "pong.iron placeholder body uses 5 separate Keyboard.* calls consuming all 5 KeyboardKey locals (start_key / left_up / left_down / right_up / right_down). Removes the _unused_6..10 bindings and gives the binary 5 live raylib-symbol references, so any link failure would surface immediately."

patterns-established:
  - "Namespace-then-method ordering: declare empty `object Keyboard {}` alongside existing Window/Draw/Audio/Files in the namespace-objects section, then add `func Keyboard.*` stubs in a NEW section at end of file (same shape as Phase 61's Window section)"
  - "Collision-check-via-build pattern: when testing whether two declarations can coexist at module scope, the cheapest verification is running the canonical end-to-end build (ironc pong.iron), not a separate probe iron file. Dovetails with one-ironc-per-plan memory budget."

requirements-completed: [INPUT-01, INPUT-02, INPUT-03]

# Metrics
duration: 4 min
completed: 2026-04-14
---

# Phase 62 Plan 01: Keyboard Bindings Summary

**8 raylib keyboard functions bound to a new `Keyboard` namespace, Gesture-vs-enum collision resolved via plural `Gestures` fallback, pong.iron end-to-end validated through ironc as a 2.5 MB Mach-O arm64 binary.**

## Performance

- **Duration:** 4 min
- **Started:** 2026-04-15T04:56:19Z
- **Completed:** 2026-04-15T05:00:26Z
- **Tasks:** 3 (all type="auto")
- **Files modified:** 4

## Accomplishments

- 5 new namespace `object`s declared in raylib.iron (Keyboard, Mouse, Gamepad, Touch, Gestures) — Plans 62-02/03/04 are now unblocked and have their receiver types already defined
- 8 keyboard functions bound across three files: 5 boolean queries (`is_pressed`, `is_pressed_repeat`, `is_down`, `is_released`, `is_up`), 2 queue-drain queries (`get_pressed`, `get_char_pressed`), 1 setter (`set_exit_key`)
- Gesture-vs-Gesture name-collision question answered decisively: Iron's module scope does NOT allow an enum and an object to share a name. Plural fallback `object Gestures {}` is now canonical for Phase 62 downstream plans.
- pong.iron's `-- PHASE 62:` markers rewritten to real `Keyboard.is_pressed` / `Keyboard.is_down` calls — end-to-end ironc build produces a 2.5 MB Mach-O 64-bit arm64 executable that links against raylib's `IsKeyPressed` / `IsKeyDown` symbols

## The 8 Keyboard Bindings

| Iron Signature | raylib Function | Requirement |
| --- | --- | --- |
| `func Keyboard.is_pressed(key: KeyboardKey) -> Bool` | `IsKeyPressed(int key)` | INPUT-01 |
| `func Keyboard.is_pressed_repeat(key: KeyboardKey) -> Bool` | `IsKeyPressedRepeat(int key)` | INPUT-01 |
| `func Keyboard.is_down(key: KeyboardKey) -> Bool` | `IsKeyDown(int key)` | INPUT-01 |
| `func Keyboard.is_released(key: KeyboardKey) -> Bool` | `IsKeyReleased(int key)` | INPUT-01 |
| `func Keyboard.is_up(key: KeyboardKey) -> Bool` | `IsKeyUp(int key)` | INPUT-01 |
| `func Keyboard.get_pressed() -> KeyboardKey` | `int GetKeyPressed(void)` | INPUT-02 |
| `func Keyboard.get_char_pressed() -> Int32` | `int GetCharPressed(void)` | INPUT-02 |
| `func Keyboard.set_exit_key(key: KeyboardKey)` | `void SetExitKey(int key)` | INPUT-03 |

All shim wrappers use the `(int)key` cast at the FFI boundary mirroring Phase 61's `SetWindowState((unsigned int)flags)` precedent. The `get_pressed` return-side type flip (C `int32_t` → Iron `KeyboardKey`) is handled by Iron's enum-as-underlying-integer lowering — no explicit C cast needed, the Iron HIR handles the type-ness at the call site.

## Line-Count Additions

| File | Lines Added | What |
| --- | --- | --- |
| `src/stdlib/raylib.iron` | +63 | 5 namespace `object` decls (Keyboard/Mouse/Gamepad/Touch/Gestures), collision-note comment block, Input (Phase 62) section header, 8 `func Keyboard.*` stubs with docstrings |
| `src/stdlib/iron_raylib.h` | +11 | 8 `Iron_keyboard_*` prototypes in the Input (Phase 62) section |
| `src/stdlib/iron_raylib.c` | +31 | 8 shim implementations with enum-int-cast comments |
| `examples/pong/pong.iron` | +15/-7 | 2 `-- PHASE 62:` comments removed, 5 real `Keyboard.*` calls added, 5 `_unused_N` bindings removed (start_key/left_up/left_down/right_up/right_down now consumed by Keyboard calls) |

Total: **+120 insertions, -7 deletions** across 4 files.

## Task Commits

Each task was committed atomically to the working branch:

1. **Task 1: Add object Keyboard {} + collision check namespaces** — `0d14923` (feat)
2. **Task 2: Bind 8 keyboard functions** — `d213075` (feat)
3. **Task 3: Rewrite pong.iron + end-to-end ironc build + Gesture→Gestures rename** — `728046a` (feat)

## End-to-End ironc Build Result

**PASS.** Canonical Phase 62 validation command:

```
./build/ironc build examples/pong/pong.iron -o /tmp/iron_pong_62_01
```

Output: `Built: /tmp/iron_pong_62_01` (exit 0 on the second invocation, after the Gesture→Gestures rename resolved E0201).

Artifact: `/tmp/iron_pong_62_01`, 2.5 MB, `Mach-O 64-bit executable arm64`. File removed post-verification per Task 3 cleanup step.

First invocation (before rename) reported:

```
error[E0201]: duplicate type declaration
  --> examples/pong/pong.iron:997:1
  996 | object Touch {}
  997 | object Gesture {}
      | ^
```

This was the canonical collision check and the answer we needed. Plans 62-02/03/04 now have a definitive namespace name (`Gestures`) to bind against.

## ABI Notes — int → KeyboardKey return cast

No surprises. Iron's `func Keyboard.get_pressed() -> KeyboardKey {}` stub lowers to a call to `Iron_keyboard_get_pressed()` that returns `int32_t` at the C boundary; Iron's HIR treats the KeyboardKey enum as its underlying Int32 type (same as every other Iron enum — enums have no runtime tag, they are plain integer type-aliases with symbolic constants). The shim simply does `return (int32_t)GetKeyPressed();` and the Iron-side receiver variable's KeyboardKey type-ness is Iron's typechecker problem, not the C wrapper's.

The same pattern works for `KeyboardKey` enum parameters in the other direction: Iron passes `KeyboardKey.SPACE` as `int32_t 32` across the FFI, and the shim casts `(int)key` before handing to raylib's `IsKeyPressed(int)`. Phase 61's `Iron_window_set_state((unsigned int)flags)` already established this path with the `ConfigFlags` enum.

## Note for Plans 62-02 / 62-03 / 62-04

**USE PLURAL `Gestures` FOR THE GESTURE NAMESPACE.** The singular `Gesture` collides with the existing `enum Gesture` at module scope (ironc E0201: duplicate type declaration). The Iron-side enum `Gesture` stays unchanged — user code still writes `Gesture.TAP`, `Gesture.HOLD`, etc. — but the namespace for gesture methods is spelled `Gestures`:

```iron
-- Plan 62-04 will add methods like:
func Gestures.set_enabled(flags: Gesture) {}
func Gestures.is_detected(g: Gesture) -> Bool {}
func Gestures.get_detected() -> Gesture {}
func Gestures.get_hold_duration() -> Float32 {}
func Gestures.get_drag_vector() -> Vector2 {}
func Gestures.get_drag_angle() -> Float32 {}
func Gestures.get_pinch_vector() -> Vector2 {}
func Gestures.get_pinch_angle() -> Float32 {}
```

The four other namespaces (`Keyboard`, `Mouse`, `Gamepad`, `Touch`) have no collision and use the natural singular form per the CONTEXT.md decision.

## Removed `_unused_*` Bindings from pong.iron

The five `_unused_N = <KeyboardKey local>` tombstones at the bottom of pong.iron's `main()` body were deleted in Task 3 because the five KeyboardKey locals they referenced are now consumed by real `Keyboard.*` calls. Removed bindings:

- `_unused_6 = start_key` (now consumed by `Keyboard.is_pressed(start_key)`)
- `_unused_7 = left_up` (now consumed by `Keyboard.is_down(left_up)`)
- `_unused_8 = left_down` (now consumed by `Keyboard.is_down(left_down)`)
- `_unused_9 = right_up` (now consumed by `Keyboard.is_down(right_up)`)
- `_unused_10 = right_down` (now consumed by `Keyboard.is_down(right_down)`)

Preserved bindings (locals still not consumed by Phase 62):

- `_unused_0..5` (title / bg / fg / accent / divider / ball_origin — these are String / Color / Vector2 values consumed in Phase 63 Draw.* bindings)
- `_unused_11..12` (initial_state / score_str — GameState enum + String needed for the Phase 73 gameplay restoration)

## Decisions Made

See key-decisions in frontmatter. The central decision was the Gesture→Gestures rename, executed as a Rule 1 auto-fix after Task 3's canonical ironc build failed with E0201. The rename was pre-authorized by the plan's CONTEXT.md ("If illegal, fallback is `object Gestures {}` (plural, matches raylib's internal `rgestures` module name)") and the collision-note comment block in raylib.iron was updated to document the outcome so Plans 62-02/03/04 see the answer inline.

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 1 - Bug] Renamed `object Gesture {}` → `object Gestures {}` after ironc E0201 collision**
- **Found during:** Task 3 (canonical pong.iron ironc build)
- **Issue:** `object Gesture {}` declared in Task 1 collided with the existing `enum Gesture {}` from Phase 60-07. Ironc correctly reported `error[E0201]: duplicate type declaration` at line 997 of `raylib.iron`. This was expected as one of two possible outcomes of the collision check — the plan explicitly pre-authorized the rename as the fallback path.
- **Fix:** Renamed `object Gesture {}` → `object Gestures {}` in raylib.iron. Updated the collision-note comment block above the namespace declarations to document the outcome (COLLISION NOTE — RESOLVED). The Iron-side `enum Gesture` was NOT modified — only the namespace spelling is plural.
- **Files modified:** `src/stdlib/raylib.iron` (1 line + 5 lines of updated comment)
- **Verification:** Re-ran `./build/ironc build examples/pong/pong.iron -o /tmp/iron_pong_62_01` — succeeded, produced a 2.5 MB Mach-O 64-bit arm64 executable. Exit 0.
- **Committed in:** `728046a` (Task 3 commit)

---

**Total deviations:** 1 auto-fixed (1 bug — the collision-check outcome that was pre-authorized in CONTEXT.md)

**Impact on plan:** Zero scope change. The fallback was explicitly planned. Plans 62-02/03/04 now have a definitive namespace name locked in (`Gestures`). The rename adds 0 net complexity to downstream plans — they simply bind `Gestures.*` instead of `Gesture.*`, one letter difference.

## Issues Encountered

None. The E0201 collision was expected-behavior (the collision check was the whole point of Task 1), not a problem.

## User Setup Required

None — no external service configuration required. Plan 62-01 only modifies the Iron stdlib, the shim C file, and pong.iron.

## Next Phase Readiness

- **Plans 62-02 / 62-03 / 62-04 are unblocked.** All 5 Phase 62 namespaces exist in raylib.iron (Keyboard populated; Mouse / Gamepad / Touch / Gestures declared and waiting for Plans 62-02/03/04 to add their method bindings).
- **The `Gestures` spelling is locked** — Plans 62-02/03/04 MUST use the plural form. The collision-note comment block in raylib.iron is the in-source record.
- **ironc is proven end-to-end on the new keyboard bindings.** Plans 62-02/03/04's canonical validation path (pong.iron or a touch/gamepad-specific example) should work the same way — build → binary → Mach-O.
- **No Phase 60 ABI regression.** `clang -c iron_raylib_layout.c` still exits 0 with all 392 `_Static_assert` entries intact (Phase 62 adds no new types, as expected per CONTEXT.md).

---
*Phase: 62-input-keyboard-mouse-gamepad-touch-gestures*
*Completed: 2026-04-14*

## Self-Check: PASSED

All 5 claimed files exist on disk (`src/stdlib/raylib.iron`, `src/stdlib/iron_raylib.h`, `src/stdlib/iron_raylib.c`, `examples/pong/pong.iron`, `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-01-SUMMARY.md`).

All 3 claimed task commits exist in git history (`0d14923`, `d213075`, `728046a`).
