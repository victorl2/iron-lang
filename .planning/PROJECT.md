# Iron Language

## What This Is

Iron is a programming language that compiles to native C and WebAssembly. It targets game and graphics work first: the standard library ships with a full raylib binding so users can write 2D and 3D games end-to-end in idiomatic Iron and build them for desktop or web with `iron build` / `iron build --target=web`.

## Core Value

Every Iron feature compiles to correct, working C code that produces a fast native binary or WebAssembly module — and the standard library exposes raylib in idiomatic Iron form, not as a thin C transliteration.

## Current Milestone: v2.0.0-alpha — Iron Builds Real Games

**Goal:** Close every gap in the raylib binding so any 2D or 3D game is writable in pure Iron, with audio, textures, fonts, shaders, models, and math.

**Target features:**
- Full type and enum coverage — Vector2/3/4, Matrix, Rectangle, Image, Texture2D, RenderTexture2D, Camera2D/3D, Font, Sound, Music, Wave, Mesh, Model, Material, Shader, Ray, BoundingBox + 21 raylib enums (KeyboardKey, MouseButton/Cursor, GamepadButton/Axis, ConfigFlags, BlendMode, PixelFormat, TextureFilter/Wrap, CameraMode/Projection, etc.)
- Window & system — full window management, monitor info, clipboard, screenshot, ConfigFlags
- Input — full keyboard (101 keys), mouse + cursors, gamepad (buttons + axes + vibration), touch + gestures
- 2D rendering — every shape, spline, polygon, gradient, with `BeginMode2D` + `Camera2D`
- Collision — 2D (Rectangle/Circle/Point) and 3D (Ray/BoundingBox/Sphere/Box)
- Textures & images — `LoadImage`, `LoadTexture`, image manipulation, `RenderTexture2D`, n-patches, image drawing primitives
- Text & fonts — custom font loading (TTF/BMP/SDF), glyph access, codepoint utilities, `MeasureText`/`DrawTextEx`
- Audio — `InitAudioDevice`, sound effects, music streaming, wave manipulation, audio streams
- 3D rendering — every 3D draw primitive, `BeginMode3D`, `Camera3D` with all camera modes
- Models, meshes, materials — `LoadModel`, procedural mesh generation, model animation, materials, bones
- Shaders — `LoadShader`, uniform setting (all types), shader location indices
- Math (raymath) — all ~143 helpers as idiomatic Iron methods (`v.add(other)`, `v.normalize()`, `m.multiply(n)`, `q.lerp(r, t)`)
- File I/O & utilities — file path lists, file drops, time helpers, misc utilities
- Idiomatic API layer — methods on types, constructor sugar (`Color.rgb(r, g, b)`), tuple returns where natural, type-safe enum parameters everywhere

## Requirements

### Active

<!-- Current scope. Building toward these in v2.0.0-alpha. -->

See `.planning/REQUIREMENTS.md` for the full categorized list.

### Out of Scope

<!-- Explicit boundaries for v2.0.0-alpha. -->

- **VR stereo rendering** (`VrDeviceInfo`, `VrStereoConfig`, `BeginVrStereoMode`) — niche, defer to a later milestone
- **Automation events / input replay** — tooling-oriented, not blocking real games
- **`rlgl.h` low-level OpenGL abstraction** — wraps the layer below raylib; not needed for application code
- **Custom raylib build flags / platform detection beyond current wiring** — current native + emcc setup stays as-is
- **Re-architecting the raylib build pipeline** — the existing `src/cli/build.c` / `build_web.c` per-source compilation is already correct

## Context

- raylib 5.5 is vendored at `src/vendor/raylib/` (raylib.h, raymath.h, rlgl.h + 8 source files: rcore, rshapes, rtextures, rtext, rmodels, raudio, utils, rglfw)
- Native builds compile raylib sources via clang in `src/cli/build.c`; web builds compile via emcc in `src/cli/build_web.c` with `-sUSE_GLFW=3`
- Current binding lives in `src/stdlib/raylib.iron` (~68 lines, ~25 bound symbols — 3% coverage)
- No `iron_raylib.c` C shim exists today — bindings are pure `extern func` against raylib symbols. This milestone may introduce a shim for cases where raylib's API needs translation (e.g., out-params, varargs, struct returns) to fit Iron's call convention.
- Iron compiler features already in place that this milestone leans on: tuple returns (v1.2.0), full closure capture, generic enums, lambdas with capture, definite assignment, sized integers (UInt8 etc.), Float32 (for C float ABI compatibility), `extern func` FFI
- Real users hitting gaps today: `examples/pong/pong.iron`, `tests/manual/game_raylib.iron`, `tests/integration/web/hello_raylib.iron` — all 2D/text-only, blocked from any non-trivial game

## Constraints

- **Tech stack:** raylib 5.5 vendored sources only (no system raylib, no version bump in this milestone)
- **ABI:** Iron `Float` = C `double`; raylib uses C `float` everywhere — must use `Float32` in struct fields and parameters that map to raylib (existing convention)
- **String marshalling:** Iron `String` → C `char*` is handled at the FFI boundary; this milestone must not regress string-passing for raylib calls
- **Build targets:** Every binding must work on both native (clang) and web (emcc + Emscripten GLFW) targets — no native-only or web-only features
- **API style:** Methods on types where idiomatic; bare `extern func` only when no sensible receiver exists; constructor sugar where it improves readability

## Key Decisions

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Fresh PROJECT.md (no historical reconstruction) | `.planning/` was untracked April 2026; bootstrapping prior milestones from CHANGELOG adds friction without payoff for this scope | — Pending |
| Bind raymath fully (143 helpers) | 3D code is unwritable without vector/matrix/quaternion math; partial bindings would force users to drop into raw extern calls | — Pending |
| Idiomatic Iron API (methods + sugar), not 1:1 C mirror | User explicitly asked for Iron-native style; raylib API in C is verbose and stateful — wrapping it in methods gives a much better Iron experience | — Pending |
| 100% of public API per in-scope category | "Close the gaps" reading; partial coverage leaves users hitting walls and is harder to validate | — Pending |
| v2.0.0-alpha (major bump from v1.2.0-alpha) | First milestone where Iron can build real games end-to-end; large enough surface area to justify the bump | — Pending |
| Defer VR + automation + rlgl | VR is niche, automation is tooling, rlgl is below raylib — none of these block "real games" use case | — Pending |
| New `iron_raylib.c` C shim allowed if needed | Some raylib APIs (varargs `TextFormat`, out-params, struct-by-value returns) may not map cleanly to Iron's FFI; a shim is the cleanest escape hatch | — Pending |

---
*Last updated: 2026-04-17 after Phase 65 (raymath) — all 143 raymath functions bound across Vector2/3/4, Matrix, Quaternion + RMath scalar namespace. Proved 3-tuple auto-emit (`MatrixDecompose → (Vector3, Quaternion, Vector3)`), first 64B Matrix struct-by-value RETURN, Float3/Float16 helper types, RAYMATH_STATIC_INLINE strategy. 54 ABI round-trip assertions pass. Milestone 6/14 phases complete. Next: Phase 66 (Textures & Images).*
