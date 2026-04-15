---
phase: 62-input-keyboard-mouse-gamepad-touch-gestures
plan: 02
status: complete
completed: 2026-04-15
requirements_closed: [INPUT-04, INPUT-05, INPUT-06, INPUT-07]
commits: [24161d6, 2f3b798, df06bf1]
---

# Plan 62-02 Summary — Mouse + Cursor Bindings

**14 raylib mouse functions bound to the `Mouse` namespace across three shim files. All three `Vector2` struct-by-value returns (`get_position`, `get_delta`, `get_wheel_move_v`) work on first try, reusing the Phase 61 field-copy pattern from `Iron_window_get_window_position`.**

## What Landed

### Functions Bound (14 total)

| Iron Stub | C Shim | raylib Target | Requirement |
|-----------|--------|---------------|-------------|
| `Mouse.is_button_pressed(button: MouseButton) -> Bool` | `Iron_mouse_is_button_pressed(int32_t)` | `IsMouseButtonPressed((int)button)` | INPUT-04 |
| `Mouse.is_button_down(button: MouseButton) -> Bool` | `Iron_mouse_is_button_down(int32_t)` | `IsMouseButtonDown((int)button)` | INPUT-04 |
| `Mouse.is_button_released(button: MouseButton) -> Bool` | `Iron_mouse_is_button_released(int32_t)` | `IsMouseButtonReleased((int)button)` | INPUT-04 |
| `Mouse.is_button_up(button: MouseButton) -> Bool` | `Iron_mouse_is_button_up(int32_t)` | `IsMouseButtonUp((int)button)` | INPUT-04 |
| `Mouse.get_x() -> Int32` | `Iron_mouse_get_x(void)` | `GetMouseX()` | INPUT-05 |
| `Mouse.get_y() -> Int32` | `Iron_mouse_get_y(void)` | `GetMouseY()` | INPUT-05 |
| `Mouse.get_position() -> Vector2` | `Iron_mouse_get_position(void) -> struct Iron_Vector2` | `GetMousePosition()` | INPUT-05 |
| `Mouse.get_delta() -> Vector2` | `Iron_mouse_get_delta(void) -> struct Iron_Vector2` | `GetMouseDelta()` | INPUT-05 |
| `Mouse.set_position(x: Int32, y: Int32)` | `Iron_mouse_set_position(int32_t, int32_t)` | `SetMousePosition((int)x, (int)y)` | INPUT-06 |
| `Mouse.set_offset(offset_x: Int32, offset_y: Int32)` | `Iron_mouse_set_offset(int32_t, int32_t)` | `SetMouseOffset((int)offset_x, (int)offset_y)` | INPUT-06 |
| `Mouse.set_scale(scale_x: Float32, scale_y: Float32)` | `Iron_mouse_set_scale(float, float)` | `SetMouseScale(scale_x, scale_y)` | INPUT-06 |
| `Mouse.get_wheel_move() -> Float32` | `Iron_mouse_get_wheel_move(void)` | `GetMouseWheelMove()` | INPUT-06 |
| `Mouse.get_wheel_move_v() -> Vector2` | `Iron_mouse_get_wheel_move_v(void) -> struct Iron_Vector2` | `GetMouseWheelMoveV()` | INPUT-06 |
| `Mouse.set_cursor(cursor: MouseCursor)` | `Iron_mouse_set_cursor(int32_t)` | `SetMouseCursor((int)cursor)` | INPUT-07 |

### File Deltas

| File | Added Lines | Purpose |
|------|-------------|---------|
| `src/stdlib/iron_raylib.h` | +15 | 14 `Iron_mouse_*` prototypes in the Input (Phase 62) section |
| `src/stdlib/iron_raylib.c` | +58 | 14 shim implementations with Vector2 struct-by-value returns |
| `src/stdlib/raylib.iron` | +29 | 14 `func Mouse.*` empty-body stubs with typed MouseButton/MouseCursor params |

### Commits (atomic, in order)

1. `24161d6` — `feat(62-02): declare 14 Iron_mouse_* C prototypes in Input section`
2. `2f3b798` — `feat(62-02): implement 14 Iron_mouse_* shims forwarding to raylib`
3. `df06bf1` — `feat(62-02): add 14 Mouse namespace stubs to raylib.iron`

## Validation

- `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` → exit 0, no warnings
- `grep -c '^func Mouse\.' src/stdlib/raylib.iron` → 14
- `grep -c 'Iron_mouse_' src/stdlib/iron_raylib.h` → 14
- `grep -c 'Iron_mouse_' src/stdlib/iron_raylib.c` → 14
- Phase 60 `_Static_assert` baseline unchanged (392 entries, still clean)
- No ironc invocation required — the canonical end-to-end build on pong.iron was validated in Plan 62-01 and Phase 62 Wave 2 plans verify with direct clang compile only

## ABI Notes

- **Vector2 struct-by-value returns:** 3 new call sites in this plan (`get_position`, `get_delta`, `get_wheel_move_v`) — all use the Phase 61 pattern of declaring a local `Vector2 v = Get*()`, constructing a `struct Iron_Vector2 out` via field copies, and returning it. No ABI surprises; the `_Static_assert(sizeof(struct Iron_Vector2) == sizeof(Vector2))` from Plan 60-02 continues to guard byte-compatibility.
- **Enum parameters:** `MouseButton` (4 button queries) and `MouseCursor` (1 setter) flow through the FFI as `int32_t` and get cast to `int` at the raylib call site — same uniform pattern as Keyboard's `KeyboardKey` parameter in Plan 62-01.

## What This Unblocks

- Plan 62-03 (Gamepad) — can append its own block after the Mouse block in all three shim files
- Phase 63 2D drawing (future) — `Mouse.get_position()` is ready to pass into 2D primitives once Phase 63 binds them
- Phase 73 API polish (future) — Mouse now has a full surface to potentially add sugar over

## Self-Check: PASSED

- [x] 14 Mouse functions land across raylib.iron + iron_raylib.h + iron_raylib.c
- [x] clang baseline exits 0
- [x] Atomic commits, one per file
- [x] No regressions to Phase 60 `_Static_assert` grid
- [x] Requirements INPUT-04 through INPUT-07 closed
