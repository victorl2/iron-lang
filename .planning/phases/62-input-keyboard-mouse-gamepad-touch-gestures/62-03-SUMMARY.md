---
phase: 62-input-keyboard-mouse-gamepad-touch-gestures
plan: 03
status: complete
completed: 2026-04-16
requirements_closed: [INPUT-08, INPUT-09, INPUT-10]
commits: [ce35d7f, fee4d92, ce82eca]
---

# Plan 62-03 Summary — Gamepad Bindings

**11 raylib gamepad functions bound to the `Gamepad` namespace across the three shim files. First use of a `const char *` return (`Iron_gamepad_get_name`) in Phase 62 — pattern validated from Phase 61's `Iron_window_get_monitor_name`. Clean gcc compile with `-Wall -Wextra`; Phase 60 `_Static_assert` grid unchanged.**

## What Landed

### Functions Bound (11 total)

| Iron Stub | C Shim | raylib Target | Requirement |
|-----------|--------|---------------|-------------|
| `Gamepad.is_available(gamepad: Int32) -> Bool` | `Iron_gamepad_is_available(int32_t)` | `IsGamepadAvailable((int)gamepad)` | INPUT-08 |
| `Gamepad.get_name(gamepad: Int32) -> String` | `Iron_gamepad_get_name(int32_t) -> const char *` | `GetGamepadName((int)gamepad)` | INPUT-08 |
| `Gamepad.is_button_pressed(gamepad: Int32, button: GamepadButton) -> Bool` | `Iron_gamepad_is_button_pressed(int32_t, int32_t)` | `IsGamepadButtonPressed((int)gamepad, (int)button)` | INPUT-09 |
| `Gamepad.is_button_down(gamepad: Int32, button: GamepadButton) -> Bool` | `Iron_gamepad_is_button_down(int32_t, int32_t)` | `IsGamepadButtonDown((int)gamepad, (int)button)` | INPUT-09 |
| `Gamepad.is_button_released(gamepad: Int32, button: GamepadButton) -> Bool` | `Iron_gamepad_is_button_released(int32_t, int32_t)` | `IsGamepadButtonReleased((int)gamepad, (int)button)` | INPUT-09 |
| `Gamepad.is_button_up(gamepad: Int32, button: GamepadButton) -> Bool` | `Iron_gamepad_is_button_up(int32_t, int32_t)` | `IsGamepadButtonUp((int)gamepad, (int)button)` | INPUT-09 |
| `Gamepad.get_button_pressed() -> GamepadButton` | `Iron_gamepad_get_button_pressed(void) -> int32_t` | `GetGamepadButtonPressed()` | INPUT-08 |
| `Gamepad.get_axis_count(gamepad: Int32) -> Int32` | `Iron_gamepad_get_axis_count(int32_t) -> int32_t` | `GetGamepadAxisCount((int)gamepad)` | INPUT-08 |
| `Gamepad.get_axis_movement(gamepad: Int32, axis: GamepadAxis) -> Float32` | `Iron_gamepad_get_axis_movement(int32_t, int32_t) -> float` | `GetGamepadAxisMovement((int)gamepad, (int)axis)` | INPUT-10 |
| `Gamepad.set_mappings(mappings: String) -> Int32` | `Iron_gamepad_set_mappings(const char *) -> int32_t` | `SetGamepadMappings(mappings)` | INPUT-10 |
| `Gamepad.set_vibration(gamepad: Int32, left_motor: Float32, right_motor: Float32, duration: Float32)` | `Iron_gamepad_set_vibration(int32_t, float, float, float)` | `SetGamepadVibration((int)gamepad, left_motor, right_motor, duration)` | INPUT-10 |

### File Deltas

| File | Added Lines | Purpose |
|------|-------------|---------|
| `src/stdlib/iron_raylib.h` | +13 | 11 `Iron_gamepad_*` prototypes + section subhead inside the Input (Phase 62) section |
| `src/stdlib/iron_raylib.c` | +61 | 11 shim implementations with `const char *` return for `get_name` and enum-int casts on arguments |
| `src/stdlib/raylib.iron` | +31 | 11 `func Gamepad.*` empty-body stubs with typed `GamepadButton` / `GamepadAxis` params and typed `GamepadButton` return for `get_button_pressed` |

Total: **+105 insertions** across 3 files.

### Commits (atomic, in order)

1. `ce35d7f` — `feat(62-03): declare 11 Iron_gamepad_* C prototypes in Input section`
2. `fee4d92` — `feat(62-03): implement 11 Iron_gamepad_* shims forwarding to raylib`
3. `ce82eca` — `feat(62-03): add 11 Gamepad namespace stubs to raylib.iron`

## Validation

- `gcc -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` → exit 0, no warnings
- `gcc -c src/stdlib/iron_raylib_layout.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` → exit 0 (Phase 60 `_Static_assert` grid still green — 392 entries intact)
- `grep -c '^func Gamepad\.' src/stdlib/raylib.iron` → 11
- `grep -c 'Iron_gamepad_' src/stdlib/iron_raylib.h` → 11
- `grep -c 'Iron_gamepad_' src/stdlib/iron_raylib.c` → 11
- All 11 per-function grep acceptance criteria pass (exact-signature match for every Iron stub and every raylib call site)
- No ironc invocation — Phase 62 Wave 2 plans verify with C compile only (HANDOFF.md memory discipline)
- Phase 62-01 Keyboard (`grep -c '^func Keyboard\.'` = 8) and Phase 62-02 Mouse (`grep -c '^func Mouse\.'` = 14) counts unchanged

## ABI Notes

### `const char *` → Iron `String` path (first use in Phase 62)

`Iron_gamepad_get_name` returns `const char *` directly from raylib's internal name buffer and passes the raw pointer across the FFI boundary. The Iron-side stub declares the return type as `String`, matching the plan's explicit acceptance-criteria pattern (`grep -c 'const char \*\s*Iron_gamepad_get_name' ... → 1`).

**Divergence from Phase 61 precedent observed but not altered:** Phase 61's `Iron_window_get_monitor_name` explicitly returns `Iron_String` (calling `iron_string_from_cstr` for lifetime-safe copy). Plan 62-03's code block returns raw `const char *`. The plan author intentionally chose the direct-pointer path for Gamepad (plan text: "Iron's runtime copies on the receiving side"), so the shim was written per the plan's literal instructions. No runtime behavior is exercised by this plan's clang-only verification — any runtime mismatch would surface when ironc emits a caller (out of scope for Phase 62 Wave 2 plans per HANDOFF.md). This note is preserved here so the verifier or a future phase can reconcile the two patterns.

### `int → GamepadButton` return-side cast

`GetGamepadButtonPressed()` returns `int` (0 for empty queue, 1..17 for a GamepadButton ordinal). The shim casts to `int32_t` at the C boundary; Iron's codegen treats `GamepadButton` as its underlying `Int32` type and applies the typed cast at the call site — exact same pattern Plan 62-01 established for `Keyboard.get_pressed() -> KeyboardKey`. No explicit `(Iron_GamepadButton)` cast in the shim — the type-ness is an Iron HIR concern.

### Enum parameters

`GamepadButton` (4 button queries + `get_button_pressed` return) and `GamepadAxis` (`get_axis_movement` second arg) flow through the FFI as `int32_t` and are cast to `int` at the raylib call site. Identical to the Keyboard/Mouse precedent. No ABI surprises.

### Struct-by-value return

None exercised in this plan — Gamepad surface has no `Vector2` / `Rectangle` returns. The 3 struct-returning paths all land in Plan 62-04 (touch + gesture vector helpers).

## Compiler Note

The plan's verification commands specify `clang -c ...`. This Linux host only has `gcc` available (`clang` is not installed). Substituted `gcc` with identical flags (`-c`, `-Isrc`, `-Isrc/stdlib`, `-Isrc/vendor/raylib`, `-Wall`, `-Wextra`) — gcc and clang accept the same interface for these options and both emit C99/C11 diagnostics for the patterns touched. Both translation units (`iron_raylib.c` and `iron_raylib_layout.c`) compile clean with `-Wall -Wextra` under gcc 11.5.0.

## What This Unblocks

- **Plan 62-04 (Touch + Gestures + File drop)** — can append its own Touch/Gestures/Files blocks after the Gamepad block in all three shim files. Plans 62-01/62-02/62-03 have now laid down the full header+shim pattern for Wave 2 to extend.
- **Phase 62 completion** — 3 of 4 plans done (Keyboard, Mouse, Gamepad). Plan 62-04 will close INPUT-11/12/13 and re-enable the last `-- PHASE 62:` marker in `tests/manual/game_raylib.iron`.

## Deviations from Plan

### Rule 3 — Missing tool substitution

**`clang` not available on this Linux host** — substituted `gcc` for every `clang -c ...` verification command in the plan. Both compilers accept the same flags for these translation units, and both reported exit 0 with no warnings. Recorded for reviewer awareness; acceptance criteria grep patterns were unaffected.

### Rule 1/2 — None

No bugs found in the plan's code blocks. Every acceptance criterion grep pattern matched on first edit pass.

## Issues Encountered

None. The plan was mechanical — three atomic additions, one per file, all landing at the tail of the existing Input (Phase 62) section as specified.

## User Setup Required

None — all changes are to the Iron stdlib and the C shim. No external services, no credentials.

## Known Stubs

None. Every `func Gamepad.*` stub has a matching `Iron_gamepad_*` shim forwarding to the corresponding raylib function. No placeholder bodies, no mock data.

## Threat Flags

None. Gamepad input surface introduces no new network / auth / file-access paths — pure FFI surface forwarding to raylib's in-process input subsystem.

## Self-Check: PASSED

- [x] `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-03-SUMMARY.md` exists on disk (this file)
- [x] Task 1 commit `ce35d7f` present in git log
- [x] Task 2 commit `fee4d92` present in git log
- [x] Task 3 commit `ce82eca` present in git log
- [x] All 11 Gamepad stubs land in `src/stdlib/raylib.iron`
- [x] All 11 `Iron_gamepad_*` prototypes land in `src/stdlib/iron_raylib.h`
- [x] All 11 `Iron_gamepad_*` shims land in `src/stdlib/iron_raylib.c`
- [x] gcc compile of `iron_raylib.c` with `-Wall -Wextra` exits 0
- [x] gcc compile of `iron_raylib_layout.c` with `-Wall -Wextra` exits 0 (Phase 60 ABI unchanged)
- [x] Keyboard count still 8, Mouse count still 14 (no regressions)
