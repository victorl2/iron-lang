# Phase 62 Handoff — Input Bindings

**Handed off:** 2026-04-15
**Status:** Wave 1 complete, Wave 2 in progress (2/4 plans done)
**Branch:** `gsd/phase-60-61-raylib-foundation`
**PR:** #28

## TL;DR for the next contributor

Phase 62 binds raylib's input section (40 functions + 2 FilePathList accessors) to 5 new namespace types. Plans 62-01 and 62-02 are fully executed and committed. Plans 62-03 (Gamepad) and 62-04 (Touch/Gestures/FileDrop) are written and verification-passed but not yet executed.

**Resume with:**

```bash
# 1. Check out the branch
git checkout gsd/phase-60-61-raylib-foundation
git pull

# 2. Execute the remaining plans
/gsd:execute-phase 62
# ...or run plans manually one at a time:
# /gsd:execute-plan .planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-03-PLAN.md
# /gsd:execute-plan .planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-04-PLAN.md
```

## Completed Work (committed)

### Plan 62-01 — Keyboard + namespace collision resolution

- **3 commits**: `0d14923`, `d213075`, `728046a`
- **Files**: `src/stdlib/raylib.iron`, `src/stdlib/iron_raylib.h`, `src/stdlib/iron_raylib.c`, `examples/pong/pong.iron`
- **Deliverables**:
  - 5 new namespace `object`s added to `raylib.iron` (lines 950–954): `Keyboard {}`, `Mouse {}`, `Gamepad {}`, `Touch {}`, **`Gestures {}`** (PLURAL — see collision note below)
  - 8 `func Keyboard.*` stubs covering `is_pressed`, `is_pressed_repeat`, `is_down`, `is_released`, `is_up`, `get_pressed`, `get_char_pressed`, `set_exit_key`
  - 8 `Iron_keyboard_*` C prototypes and shim implementations
  - `examples/pong/pong.iron` Phase 62 markers rewritten from the nonexistent `Input.*` namespace to real `Keyboard.*` calls
  - Canonical end-to-end validation: `./build/ironc build examples/pong/pong.iron` produces a 2.5 MB Mach-O arm64 binary
- **Requirements closed**: INPUT-01, INPUT-02, INPUT-03
- **Summary**: `62-01-SUMMARY.md`

**Critical decision — `Gesture` collision is real.** Iron's module scope does NOT allow an `enum` and an `object` to share a name — ironc reports `E0201: duplicate type declaration`. Plan 62-01 adopted the plural fallback `object Gestures {}` (matches raylib's internal `rgestures.c` module name). Plan 62-04 is already written to use the plural form.

### Plan 62-02 — Mouse + cursor

- **3 commits**: `24161d6`, `2f3b798`, `df06bf1`
- **Files**: `src/stdlib/raylib.iron`, `src/stdlib/iron_raylib.h`, `src/stdlib/iron_raylib.c`
- **Deliverables**:
  - 14 `func Mouse.*` stubs — 4 button queries with typed `MouseButton`, 2 axis getters (`get_x`/`get_y` → `Int32`), 3 `Vector2` struct-by-value returns (`get_position`, `get_delta`, `get_wheel_move_v`), 3 setters (`set_position`/`set_offset`/`set_scale`), `get_wheel_move` (`Float32`), `set_cursor` with typed `MouseCursor`
  - 14 `Iron_mouse_*` C prototypes and shim implementations
  - `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` exits 0 with no warnings
- **Requirements closed**: INPUT-04, INPUT-05, INPUT-06, INPUT-07
- **Summary**: `62-02-SUMMARY.md`

**ABI notes**: The 3 Vector2 struct-by-value returns worked on first try, reusing the Phase 61 field-copy pattern from `Iron_window_get_window_position`. No new ABI surprises.

## Pending Work

### Plan 62-03 — Gamepad (written, verified, not executed)

- **File**: `62-03-PLAN.md`
- **Frontmatter**: `wave: 2`, `depends_on: [62-01, 62-02]`, `requirements: [INPUT-08, INPUT-09, INPUT-10]`
- **Scope**: 11 `Iron_gamepad_*` functions
- **Notable**: First use of `const char *` → Iron `String` return in Phase 62 (for `GetGamepadName`) — follows the Phase 61 `Iron_window_get_monitor_name` precedent.
- **No research needed** — pattern is locked from Phase 61.

### Plan 62-04 — Touch + Gestures + File drop (written, verified, not executed)

- **File**: `62-04-PLAN.md`
- **Frontmatter**: `wave: 2`, `depends_on: [62-01, 62-03]`, `requirements: [INPUT-11, INPUT-12, INPUT-13]`
- **Scope**: 18 new stubs — 5 Touch + 8 Gestures + 3 Files file-drop + 2 FilePathList accessors (`count()`, `get(i)`)
- **Notable**:
  - Binds to the **plural** `Gestures` namespace (confirmed by 62-01 SUMMARY)
  - First struct-by-value `FilePathList` return in the stdlib (`Iron_files_load_dropped`) — should work via same path as Vector2
  - First methods attached to a non-namespace data type (`FilePathList.count()` / `.get(i)`) — if Iron's typechecker rejects `func TypeName.method` on non-namespace types, fall back to freestanding `func filepathlist_count(list: FilePathList) -> Int32` helpers and document in the SUMMARY
  - Task 4 closes the remaining `-- PHASE 62:` marker in `tests/manual/game_raylib.iron` line 33 (rewrite to `Keyboard.is_down(down_key)`)

**Gesture namespace in Plan 62-04**: The plan file currently says "shown for SINGULAR `Gesture` — replace each with `Gestures` if plural". The executor MUST use **plural** throughout based on Plan 62-01's collision result. Grep patterns in the acceptance criteria already accept either form (`grep -Ec '^func (Gesture|Gestures)\.'`).

## Serial Wave 2 Chain

Plans 62-02/03/04 are all at `wave: 2` but have been serialized via `depends_on` because they all append to the tail of the Input section in the same 3 shim files. Parallel execution would produce merge conflicts.

```
62-01 (wave 1)
  └── 62-02 (wave 2, depends_on: [62-01])
        └── 62-03 (wave 2, depends_on: [62-01, 62-02])
              └── 62-04 (wave 2, depends_on: [62-01, 62-03])
```

`/gsd:execute-phase 62` will respect these dependencies automatically.

## Key Decisions Already Locked in CONTEXT.md

1. **Namespace structure**: Split into per-device namespaces (Keyboard/Mouse/Gamepad/Touch/**Gestures**). Plural on the last one.
2. **Return typing**: `Keyboard.get_pressed() -> KeyboardKey`, `Gamepad.get_button_pressed() -> GamepadButton`, `Gestures.get_detected() -> Gesture` (the enum). `Keyboard.get_char_pressed()` stays `Int32` (unicode codepoint, not an enum).
3. **FilePathList accessors**: Add `count()` and `get(i)` in Plan 62-04 to satisfy INPUT-13's iteration clause in-phase, not defer to Phase 72.
4. **`Gestures.set_enabled(flags: Gesture)` / `Gestures.is_detected(gesture: Gesture)`**: Parameters MUST be the typed `Gesture` enum (not `UInt32` raw bitmask). The plan-checker's first-round review flagged this and we already fixed it. The phase goal "through typed Iron enum parameters — no raw ints" is binding.

## Known Gotchas

1. **Clangd false positives** — `iron_raylib.c` and `iron_raylib_layout.c` are not in the CMake `iron_stdlib` target by design. Clangd reports spurious `raylib.h not found` errors. Verify with direct `clang -c` commands, not clangd diagnostics. Example:
   ```bash
   clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra -o /tmp/check.o
   ```

2. **ironc memory** — `./build/ironc build` consumes ~10 GB per invocation. Plans 62-03 and 62-04 do NOT need to invoke ironc — the canonical end-to-end validation happened in Plan 62-01 (pong.iron build). Wave 2 plans verify with `clang -c` only.

3. **`.planning/` is gitignored** — The phase 62 planning directory (this file, plans, summaries, context) was force-added with `git add -f` to include them in PR #28 for this handoff. Normal execution does NOT track planning files. After resume, keep using `git add -f` if you want to update the tracked planning files on the branch, or revert to gitignored mode (they stay locally on disk either way).

4. **Executor agent timeouts** — Both 62-01 and 62-02 agents hit stream idle timeouts near the end of their runs (after ~15 min and ~46 min respectively). Work was fully committed but SUMMARY.md writes were incomplete. Plan 62-01's SUMMARY was written by the agent; 62-02's SUMMARY was written by the orchestrator post-hoc from the commit messages. Plans 62-03 and 62-04 may hit similar timeouts — if an executor times out, verify commits landed via `git log --oneline | grep 62-NN` and spot-check the acceptance criteria before considering the plan failed.

## Files to Review in this PR

- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-CONTEXT.md` — locked decisions
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-01-PLAN.md` — Keyboard + collision check
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-01-SUMMARY.md` — authoritative Gesture→Gestures decision
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-02-PLAN.md` — Mouse
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-02-SUMMARY.md` — post-hoc summary
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-03-PLAN.md` — Gamepad (pending execution)
- `.planning/phases/62-input-keyboard-mouse-gamepad-touch-gestures/62-04-PLAN.md` — Touch/Gestures/FileDrop (pending execution)

## Verification Checklist (for reviewer)

After executing 62-03 and 62-04, verify:

- [ ] `grep -c '^func Keyboard\.' src/stdlib/raylib.iron` returns 8
- [ ] `grep -c '^func Mouse\.' src/stdlib/raylib.iron` returns 14
- [ ] `grep -c '^func Gamepad\.' src/stdlib/raylib.iron` returns 11
- [ ] `grep -c '^func Touch\.' src/stdlib/raylib.iron` returns 5
- [ ] `grep -c '^func Gestures\.' src/stdlib/raylib.iron` returns 8
- [ ] `grep -c '^func Files\.' src/stdlib/raylib.iron` returns 3 or more (3 new from 62-04)
- [ ] `grep -c '^func FilePathList\.' src/stdlib/raylib.iron` returns 2 (or freestanding fallback)
- [ ] `grep -c 'PHASE 62' tests/manual/game_raylib.iron examples/pong/pong.iron tests/integration/web/hello_raylib.iron` returns 0
- [ ] `clang -c src/stdlib/iron_raylib.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` exits 0
- [ ] `clang -c src/stdlib/iron_raylib_layout.c -Isrc -Isrc/stdlib -Isrc/vendor/raylib -Wall -Wextra` exits 0 (Phase 60 `_Static_assert` grid unchanged)
- [ ] All 13 INPUT-01..13 requirements have traceability entries in `.planning/REQUIREMENTS.md`

Then run the phase-level verifier:

```bash
/gsd:verify-phase 62
```

## After Phase 62 Closes

Next phases (per ROADMAP.md):
- **Phase 63** — 2D Drawing (rshapes.c + rcore.c draw section, ~55 functions)
- **Phase 64** — Collision
- **Phase 65** — raymath
- ... through Phase 73 (API polish + showcase)

Phase 63 and onward also have `-- PHASE 63:` / `-- PHASE 6N:` markers in the three consumer files that will need to be closed in-phase.
