# Phase 62: Input — Keyboard, Mouse, Gamepad, Touch, Gestures - Context

**Gathered:** 2026-04-14
**Status:** Ready for planning
**Source:** Smart discuss (autonomous)

<domain>
## Phase Boundary

Bind every `rcore.c` input-section function (raylib.h lines 1143–1228) to per-device namespace types. Covers 40 raylib functions across 5 input domains: keyboard (6 fns), mouse (14 fns), gamepad (9 fns), touch (5 fns), gesture (8 fns), plus 3 file-drop functions.

Requirements: INPUT-01..13 (13 requirements). Closes the `-- PHASE 62:` markers in `examples/pong/pong.iron`, `tests/manual/game_raylib.iron`, and `tests/integration/web/hello_raylib.iron` (all three consumer files reference keyboard input at minimum).

**Out of scope for Phase 62:**
- Any non-input raylib category (2D draw is Phase 63, textures Phase 66, etc.)
- New type or enum definitions — every type/enum needed by Phase 62 already exists from Phase 60 (`KeyboardKey`, `MouseButton`, `MouseCursor`, `GamepadButton`, `GamepadAxis`, `Gesture`, `Vector2`, `FilePathList`)
- Platform-specific input plumbing (raylib handles it internally)
- `SetTraceLogCallback`-style callbacks (no function-pointer FFI yet)

</domain>

<decisions>
## Implementation Decisions

### Inherited from Phase 60/61 (locked — no discussion)

- **Binding architecture:** shim-only. Every raylib call goes through `src/stdlib/iron_raylib.c` via empty-body foreign-method stub pattern. Iron-side `func Keyboard.is_pressed(key: KeyboardKey) -> Bool {}` lowers to `Iron_keyboard_is_pressed(...)` in C, which forwards to raylib's `IsKeyPressed((int)key)`.
- **Method casing:** `snake_case` everywhere — `Keyboard.is_pressed(.w)`, `Mouse.get_position()`, `Gamepad.is_available(0)`.
- **Enum parameters:** every enum param uses the typed Iron enum — `Mouse.set_cursor(MouseCursor.pointing_hand)`, not `.set_cursor(4)`. `Keyboard.is_pressed(KeyboardKey.space)`, not `.is_pressed(32)`.
- **String marshalling:** compiler-managed for arguments. Return-side C strings (`GetGamepadName`) copied into Iron-owned `String` via the same helper Phase 61 uses for `Window.get_clipboard_text`.
- **Float32 vs Float distinction:** raylib input returns `float` → Iron `Float32` (mouse wheel, gamepad axis, gesture hold duration, gesture angle). No `double` returns in the input surface.
- **Struct-by-value return:** validated in Phase 61. Phase 62 uses the same path for `GetMousePosition`, `GetMouseDelta`, `GetMouseWheelMoveV`, `GetTouchPosition`, `GetGestureDragVector`, `GetGesturePinchVector` (all `Vector2` returns), and `LoadDroppedFiles` (`FilePathList` return). No micro-smoke-test needed — Phase 61 proved the mechanism.
- **`iron_raylib.c` sections:** wrappers land in the `/* ── Input (Phase 62) ─────────── */` section marker Plan 60-01 already scaffolded.
- **Layout verification:** no new types in Phase 62, so no new `_Static_assert` entries. The 392 existing assertions continue to guard ABI compatibility.
- **Auto-generated prototypes:** commit e3e5eee introduced auto-generation of stdlib foreign-method prototypes in `emit_c`. Phase 62 benefits — no manual prototype management needed beyond what Phase 61 already did.

### Phase 62 specific

#### Namespace grouping — **split per device**

Five namespace `object`s attach the bindings. Phase 62 adds the three that don't exist yet (`Keyboard`, `Mouse`, `Gamepad` — plus `Touch`, `Gesture` for parallel structure). Phase 60-06 already declared `Window`, `Draw`, `Audio`, `Files` as empty namespace `object`s; Phase 62 adds the same pattern for its 5 namespaces.

- **`object Keyboard {}`** — `Keyboard.is_pressed`, `is_pressed_repeat`, `is_down`, `is_released`, `is_up`, `get_pressed`, `get_char_pressed`, `set_exit_key`
- **`object Mouse {}`** — `is_button_pressed`, `is_button_down`, `is_button_released`, `is_button_up`, `get_position`, `get_x`, `get_y`, `get_delta`, `set_position`, `set_offset`, `set_scale`, `get_wheel_move`, `get_wheel_move_v`, `set_cursor`
- **`object Gamepad {}`** — `is_available`, `get_name`, `is_button_pressed`, `is_button_down`, `is_button_released`, `is_button_up`, `get_button_pressed`, `get_axis_count`, `get_axis_movement`, `set_vibration`, `set_mappings`
- **`object Touch {}`** — `get_x`, `get_y`, `get_position`, `get_point_id`, `get_point_count`
- **`object Gesture {} `** — `set_enabled`, `is_detected`, `get_detected`, `get_hold_duration`, `get_drag_vector`, `get_drag_angle`, `get_pinch_vector`, `get_pinch_angle`
- **File drop** (INPUT-13) — attaches to existing `object Files {}` (already declared in Plan 60-06): `Files.is_dropped`, `Files.load_dropped`, `Files.unload_dropped`. These sit alongside later Phase 72 `Files.*` methods.

**Name-collision warning:** `Keyboard.is_pressed` and `Mouse.is_button_pressed` use slightly different verb shapes because "mouse_pressed" is ambiguous (pressed what?). `Gamepad.is_button_pressed` matches for symmetry. This is intentional — keyboard has only one channel (keys), mouse and gamepad have both buttons and other state (position/axes).

**`Gesture` namespace vs `Gesture` enum collision:** Iron allows an `enum Gesture` and `object Gesture` to coexist at the module level — verified by precedent in the stdlib? **PLANNER MUST VERIFY this is legal in Iron's name space before committing to this split.** If illegal, fallback is `object Gestures {}` (plural, matches raylib's internal "rgestures" module name).

#### Return typing — **typed enums where applicable**

- `Keyboard.get_pressed() -> KeyboardKey` — raylib returns `int` (keycode or 0). Shim casts the `int` to the Iron `KeyboardKey` enum on return. Empty queue returns `KeyboardKey.null` (ordinal 0, already defined in Phase 60-07's KeyboardKey enum).
- `Keyboard.get_char_pressed() -> Int32` — returns unicode codepoint, NOT an enum value. Stay `Int32`.
- `Gamepad.get_button_pressed() -> GamepadButton` — shim casts `int` to `GamepadButton`. Empty returns `GamepadButton.unknown` (ordinal 0).
- `Gesture.get_detected() -> Gesture` (the enum) — shim casts `int` to `Gesture` enum. Empty returns `Gesture.none` (ordinal 0).
- `Gesture.is_detected(g: Gesture) -> Bool` — takes typed enum, passes as `unsigned int`. Raylib's `IsGestureDetected` accepts a bitmask; passing a single enum value works because each `Gesture` value is a power-of-two flag.
- `Gesture.set_enabled(flags: Gesture)` — takes typed enum as bitmask. Same treatment.

All enum-casting happens in the shim, NOT in Iron code. Iron sees a clean typed API; the C shim does `(int)key` on the argument side and `(Iron_KeyboardKey)result` on the return side.

#### FilePathList iteration — **add accessors this phase**

INPUT-13 requires "User can detect dropped files and iterate the resulting `FilePathList` in a typed Iron for-loop." To satisfy this in Phase 62 (not defer to Phase 72):

- `Files.load_dropped() -> FilePathList` — returns the struct by value. Phase 60 `_Static_assert` grid already pins FilePathList layout.
- `Files.unload_dropped(list: FilePathList)` — passes by value.
- **`FilePathList.count() -> Int32`** — reads `self.count` field directly (Iron `val count: Int32` already exists from Phase 60-05). Method is a thin wrapper — could be a field access if the planner prefers.
- **`FilePathList.get(i: Int32) -> String`** — requires a shim wrapper. C path: `((char **)list._paths)[i]` indexed read, then marshalled to Iron `String` via the existing cstr-to-String helper. Shim validates `i < list.count` and returns empty string on OOB (defensive — same pattern Phase 61 uses for clipboard reads).
- **Iteration pattern the user will write:**
  ```iron
  val files = Files.load_dropped()
  var i = 0
  while i < files.count() {
      print(files.get(i))
      i = i + 1
  }
  Files.unload_dropped(files)
  ```
  A range-for loop (`for i in 0 until files.count() { ... }`) also works if Iron's syntax supports it. Planner picks the idiomatic form.

#### Plan slicing — **4 plans**

- **Plan 62-01** — Keyboard: INPUT-01, INPUT-02, INPUT-03 (6 fns + SetExitKey = 7 fns). Adds `object Keyboard {}` namespace. Re-enables `-- PHASE 62:` keyboard lines in `pong.iron` (WASD / SPACE / UP / DOWN). Canonical validation: pong.iron compiles end-to-end via `ironc build examples/pong/pong.iron`.
- **Plan 62-02** — Mouse + cursor: INPUT-04, INPUT-05, INPUT-06, INPUT-07 (14 fns). Adds `object Mouse {}` namespace. Exercises 4 struct-by-value `Vector2` returns (`GetMousePosition`, `GetMouseDelta`, `GetMouseWheelMoveV`, + `GetMousePosition` receiving-side). Re-enables `-- PHASE 62:` mouse lines in consumer files if any exist.
- **Plan 62-03** — Gamepad: INPUT-08, INPUT-09, INPUT-10 (11 fns). Adds `object Gamepad {}` namespace. First return of a `const char *` via `GetGamepadName` — use the same Phase 61 pattern.
- **Plan 62-04** — Touch + Gesture + File drop: INPUT-11, INPUT-12, INPUT-13 (16 fns). Adds `object Touch {}` and `object Gesture {}` (or `Gestures {}` — see collision warning). Extends existing `object Files {}` with 3 file-drop methods + 2 `FilePathList` accessors. Last plan: re-enable any remaining `-- PHASE 62:` lines in all three consumer files, build end-to-end.

### Re-enabling broken user files (Plan 62-04 tail)

- Plan 60-08 left `-- PHASE 62:` commented-out function calls in `examples/pong/pong.iron`, `tests/manual/game_raylib.iron`, and `tests/integration/web/hello_raylib.iron`. Phase 62 MUST uncomment every line marked `-- PHASE 62:` across all three files and verify each compiles end-to-end via `ironc build`.
- Pong is the canonical validation target (verified in Phase 60-08); game_raylib and hello_raylib are secondary because ironc is ~10 GB per invocation (memory cap). Plan 62-04 should at minimum verify pong compiles fully.
- Lines marked `-- PHASE 63:` / `-- PHASE 66:` / etc. stay commented — those unlock in their respective phases.

### Claude's Discretion

- **Exact function→shim signature mapping.** E.g., whether `Keyboard.get_pressed() -> KeyboardKey` wraps in the shim with `(Iron_KeyboardKey)GetKeyPressed()` or does a validated-lookup. Default: direct cast, since every raylib keycode 0..330 is either 0 (empty) or a defined KeyboardKey ordinal.
- **Whether `FilePathList.count()` is a method or direct field access.** Iron may allow `list.count` (field) directly without a method wrapper — planner checks existing Iron object-field access syntax. Default: method form for consistency with other namespaces.
- **Handling of `GetKeyName` / equivalents.** Not in raylib 5.5's public API surface — confirmed by grep (no `GetKeyName` match). No action.
- **`Gesture` namespace vs enum name collision.** Verify Iron allows both at module scope. If not, rename `object Gesture {}` → `object Gestures {}` (plural).
- **Whether `Mouse.set_cursor` uses `MouseCursor` enum directly or accepts an Int32.** Default: `MouseCursor` enum for INPUT-07 compliance. Shim casts.
- **`Gamepad.set_mappings(mapping: String) -> Int32`** — raylib returns an `int` (success count). Keep as `Int32`, not Bool.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Phase 60/61 foundation (authoritative design)
- `.planning/phases/60-type-enum-foundation/60-CONTEXT.md` — locked architectural decisions (shim-only, naming, opaque ptrs, layout verification, namespace types)
- `.planning/phases/60-type-enum-foundation/60-08-SUMMARY.md` — clean-break documentation, `-- PHASE 62:` markers in user files, Float32-cast-at-literal-sites pattern
- `.planning/phases/61-window-system/61-CONTEXT.md` — struct-by-value return validation pattern, shim+section-marker pattern, snake_case + typed-enum conventions
- `.planning/phases/61-window-system/61-01-PLAN.md` through `61-04-PLAN.md` — plan structure precedent, `must_haves: truths/artifacts` format, wave grouping

### raylib upstream
- `src/vendor/raylib/raylib.h` lines 1143–1228 — authoritative C signatures for every Phase 62 function. Search markers: `RLAPI.*Key`, `RLAPI.*Mouse`, `RLAPI.*Gamepad`, `RLAPI.*Touch`, `RLAPI.*Gesture`, `RLAPI.*DroppedFiles`, `RLAPI.*FileDropped`.
- `src/vendor/raylib/raylib.h` lines 576–691 — KeyboardKey enum (101 values) C constants
- `src/vendor/raylib/raylib.h` lines 699–754 — Mouse/Gamepad enum C constants
- `src/vendor/raylib/raylib.h` lines 908–922 — Gesture enum C constants (bitmask — powers of two)

### Iron stdlib precedents
- `src/stdlib/iron_raylib.h` — declare `Iron_keyboard_*`, `Iron_mouse_*`, `Iron_gamepad_*`, `Iron_touch_*`, `Iron_gesture_*`, `Iron_files_*`, `Iron_filepathlist_*` prototypes in the Input section
- `src/stdlib/iron_raylib.c` — implement `Iron_*` wrappers in the `/* ── Input (Phase 62) ───── */` section marker
- `src/stdlib/raylib.iron` — add `object Keyboard {} / Mouse {} / Gamepad {} / Touch {} / Gesture {}` (verify no collision with enum) declarations near existing `object Window {}`/`Draw {}`/`Audio {}`/`Files {}`, then add `func <Namespace>.<method>()` empty-body stubs
- `src/stdlib/iron_raylib.c` `Iron_window_get_clipboard_text` — C-string-to-Iron-String marshalling precedent for `GetGamepadName`
- `src/stdlib/iron_raylib.c` `Iron_window_get_monitor_position` — Vector2 struct-by-value return precedent for all Phase 62 Vector2 returns

### Project-level specs
- `.planning/REQUIREMENTS.md` — INPUT-01..13 detailed descriptions (lines 92–104)
- `.planning/ROADMAP.md` — Phase 62 goal and 5 success criteria

### Files to re-enable (integration verification)
- `examples/pong/pong.iron` — uncomment `-- PHASE 62:` lines (keyboard input for paddles and space-to-start), verify `ironc build` compiles to a runnable Mach-O binary
- `tests/manual/game_raylib.iron` — same treatment (memory-permitting)
- `tests/integration/web/hello_raylib.iron` — same (web target)

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **`object Files {}`** — already declared in `src/stdlib/raylib.iron` by Plan 60-06. Phase 62 extends it with file-drop methods (Plan 62-04). Phase 72 will extend it further.
- **`object Window {}`** — Phase 61 added ~30 methods. Phase 62 does NOT touch Window (Phase 61 closed WIN-01..13).
- **`Iron_raylib_*` Input section marker** — already scaffolded in `iron_raylib.c` and `iron_raylib.h` by Plan 60-01. Phase 62 fills it in.
- **Enum→int casting pattern** — Phase 61's `Iron_window_set_config_flags((unsigned int)flags)` is the precedent for Phase 62's `Iron_keyboard_is_pressed((int)key)` etc.
- **C-string return pattern** — `Iron_window_get_clipboard_text` copies raylib's internal C string into an Iron-owned `String`. `Iron_gamepad_get_name` uses the same helper.
- **Vector2 by-value return pattern** — `Iron_window_get_monitor_position` / `Iron_window_get_window_position` / `Iron_window_get_window_scale_dpi` — Phase 62 copies this pattern for 6 more Vector2-returning functions.
- **Auto-generated prototypes in `emit_c`** — commit e3e5eee (2026-04-14) auto-generates stdlib foreign-method prototypes. Phase 62 planner does NOT need to hand-maintain prototype sync between `raylib.iron` stubs and `iron_raylib.c` wrappers — emit_c handles it.
- **Clangd false-positive workaround documented:** `iron_raylib.c` and `iron_raylib_layout.c` are not in the CMake `iron_stdlib` target by design. Verify with `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` directly, not clangd.
- **Memory discipline:** ironc uses ~10 GB per invocation. Canonical build verification = `ironc build examples/pong/pong.iron`. Secondary consumer files (game_raylib, hello_raylib) deferred if memory pressure arises in the execution environment.

### Established Patterns
- **Empty-body foreign-method stubs** (`func Keyboard.is_pressed(key: KeyboardKey) -> Bool {}`) lower to `Iron_keyboard_is_pressed(...)` at the C level. Emit_c auto-generates the prototype.
- **Enum parameters** flow as `Int32`/`UInt32` across the FFI; shim does `(int)key` cast. Iron's enum is represented as its underlying integer type.
- **Bitmask enums** (`Gesture`) flow as `UInt32` across the FFI; `SetGesturesEnabled(unsigned int)` and `IsGestureDetected(unsigned int)` accept OR-ed ordinals. Phase 62 does NOT yet provide enum OR-ing syntax — single-gesture calls work naturally; multi-gesture is "user bitwise-ORs two Int32 casts" until Phase 73 API polish adds enum-set literals.
- **String arguments** (SetGamepadMappings, name returns) are compiler-managed — no per-call shim code for `String` arguments beyond declaring the parameter.
- **Return by value for primitives** works trivially (Bool, Int32, Float32, Float).
- **Return by value for Vector2** validated in Phase 61 — works via direct shim function that returns the Iron_Vector2 struct, with `_Static_assert` proving byte-compatibility with raylib's Vector2.
- **Return by value for FilePathList** is first-exercised in Phase 62 — but the mechanism is the same as Vector2. The 16-byte FilePathList struct (4-byte capacity + 4-byte count + 8-byte opaque pointer) fits ABI return conventions on every target Iron supports. The `_Static_assert` grid already proves layout match.

### Integration Points
- `src/stdlib/iron_raylib.c` Input section — where shim wrappers go
- `src/stdlib/iron_raylib.h` Input section — where C prototypes go
- `src/stdlib/raylib.iron` after existing `object Window {}/Draw {}/Audio {}/Files {}` declarations — where new Keyboard/Mouse/Gamepad/Touch/Gesture namespace declarations and their `func *.*` stubs go
- `examples/pong/pong.iron`, `tests/manual/game_raylib.iron`, `tests/integration/web/hello_raylib.iron` — re-enable commented Phase 62 lines in Plan 62-04 (and earlier plans where individual keyboard keys are needed)

</code_context>

<specifics>
## Specific Ideas

- **`Gesture` name collision (enum vs namespace object)** — Iron's parser/resolver behavior with two top-level declarations sharing a name is UNKNOWN. Planner must verify BEFORE writing Plan 62-04. Fastest check: add `object Gesture {}` to raylib.iron alongside the existing `enum Gesture {}` and run `ironc check examples/pong/pong.iron`. If it compiles, the collision is legal. If it errors, rename to `object Gestures {}` (plural) — raylib's internal source module is named `rgestures` so the plural has precedent.
- **`Keyboard.get_pressed() -> KeyboardKey` ordinal validity** — raylib's `GetKeyPressed` returns 0 for empty queue or a `KEY_*` keycode 32..348. KeyboardKey.null = 0; ordinals 32–348 cover every defined key. Direct cast is safe.
- **`Gamepad.get_button_pressed() -> GamepadButton` ordinal validity** — raylib returns 0 (UNKNOWN) or 1..17. GamepadButton.unknown = 0 per ENUM-04. Direct cast is safe.
- **`Gesture.get_detected() -> Gesture` ordinal validity** — raylib returns 0 (NONE) or 1,2,4,8,16,32,64,128,256,512 (bitmask powers of two). Iron's `Gesture` enum defines all 11 of these. Direct cast is safe.
- **`Keyboard.set_exit_key(key: KeyboardKey)`** — passes the typed enum, shim casts to `int`. Sane default: `KeyboardKey.escape` matches raylib's ESC default.
- **`Mouse.get_position() -> Vector2`** — first per-frame-called Vector2-return function in the stdlib. Precedent from Phase 61 confirms it works. Performance cost is a function call + struct copy, comparable to raylib's own internal access.
- **`SetGamepadMappings` returns Int32** — raylib returns the count of newly registered mappings. Keep as `Int32`, not Bool. User may or may not check the return.
- **`FilePathList.get(i)` bounds check** — defensive programming per Phase 61 precedent. Empty string on OOB is preferable to an Iron-side abort or a C-side SEGV; matches how `get_clipboard_text` handles empty-clipboard.

</specifics>

<deferred>
## Deferred Ideas

- **Enum bitmask OR-ing syntax** (`Gesture.tap | Gesture.hold`) — Iron doesn't expose bitwise OR on enum values yet (Phase 59 added bitwise ops on integer types, but enum→integer coercion in OR expressions is untested). Phase 62 users who need multi-gesture detection must cast: `Int32(Gesture.tap) | Int32(Gesture.hold)`. Phase 73 API polish can add ergonomic enum-set syntax.
- **`GetKeyName`** — not in raylib 5.5 public API. No action.
- **Keyboard input replay / automation events** — explicitly out of scope for v2.0.0-alpha per PROJECT.md "Out of Scope" section.
- **Gamepad rumble sequencing / patterns** — `SetGamepadVibration` is fire-and-forget; any pattern library is user-level code, not stdlib.
- **Touch gesture recognizer extension** — raylib provides 11 gestures; any higher-level gesture detection (swipe velocity thresholds, multi-touch pinch vs rotate disambiguation) is user-level code.
- **FilePathList accessor `paths(): Array<String>`** — returning a heap-allocated Iron Array would require runtime allocation coordination. Deferred to Phase 72 if needed, or never if `FilePathList.count()`/`get(i)` is sufficient.
- **`Window` namespace input conveniences** — some raylib tutorials wrap keyboard checks into `Window.should_close_on(key)` helpers. Out of scope; Phase 62 stays 1:1 with raylib's function list.

</deferred>

---

*Phase: 62-input-keyboard-mouse-gamepad-touch-gestures*
*Context gathered: 2026-04-14 via smart discuss (autonomous)*
