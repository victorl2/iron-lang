# Roadmap: Iron v2.0.0-alpha — Iron Builds Real Games

**Created:** 2026-04-13
**Milestone:** v2.0.0-alpha
**Core value:** Every Iron feature compiles to correct, working C code; the stdlib exposes raylib in idiomatic Iron form.
**Milestone goal:** Close every gap in the raylib binding so any 2D or 3D game is writable in pure Iron, on both native and web targets.

## Scope

- **Requirements:** 183 (see `.planning/REQUIREMENTS.md`)
- **Categories:** 15 (TYPE, ENUM, WIN, INPUT, DRAW2D, COLL, TEX, TEXT, AUDIO, DRAW3D, MODEL, SHADER, MATH, FILE, API)
- **Phase count:** 14 (Phase 60 through Phase 73)
- **Starting phase number:** 60 (previous milestone ended at Phase 59 — v1.2.0-alpha Networking Foundation, 2026-04-11)
- **Granularity:** standard
- **Coverage:** 183/183 requirements mapped (100%) ✓

## Phase Strategy

The 183 requirements sort into **14 vertical slices**. The slicing is dictated by raylib's own module structure (rcore, rshapes, rtextures, rtext, raudio, rmodels, raymath) and by the natural dependency graph between types, enums, and everything that consumes them.

**Dependency backbone:**

```
Phase 60 (Types + Enums)  ← foundation; every other phase depends on it
    ├── Phase 61 (Window & System)
    ├── Phase 62 (Input)
    ├── Phase 63 (2D Drawing) ──────┐
    │        └── Phase 64 (Collision — 2D + 3D)
    ├── Phase 65 (raymath) ─────────┤
    ├── Phase 66 (Textures & Images) ←── depends on 63 (BeginTextureMode lives in 2D draw)
    │        └── Phase 67 (Text & Fonts) ← Font contains Texture2D
    ├── Phase 68 (Audio) ← fully independent of graphics, parallelizable
    ├── Phase 69 (3D Drawing) ← needs 65 (math) + 66 (textures for materials indirectly)
    │        └── Phase 70 (Models, Meshes, Materials, Animations)
    │                 └── Phase 71 (Shaders) ← needs 66 (RenderTexture combos)
    ├── Phase 72 (File I/O & Utilities) ← fully independent
    └── Phase 73 (Idiomatic API Polish, Showcase, Integration Tests) ← sweeps all prior phases
```

Phase 60 unblocks every other phase. After 60, phases 61/62/65/68/72 can run in parallel (pure-leaf dependencies). 63 unblocks 64/66. 66 unblocks 67/71. 69 unblocks 70. 73 runs last as a cross-cutting sweep.

## Phases

- [x] **Phase 60: Type & Enum Foundation** — Bind all 30 raylib types and 22 enums with exact C layout (completed 2026-04-14)
- [x] **Phase 61: Window & System** — Full window/monitor/clipboard/screenshot/FPS/timing bindings from `rcore.c` (completed 2026-04-15)
- [x] **Phase 62: Input — Keyboard, Mouse, Gamepad, Touch, Gestures** — All ~40 input functions with typed enum params (completed 2026-04-16)
- [ ] **Phase 63: 2D Drawing** — Every shape/spline/gradient draw primitive plus draw-mode begin/end
- [ ] **Phase 64: Collision (2D + 3D)** — All 2D shape and 3D ray/box/sphere collision tests
- [ ] **Phase 65: raymath** — All 143 raymath helpers as idiomatic methods on Vector2/3/4, Matrix, Quaternion
- [ ] **Phase 66: Textures & Images** — Full image + texture + render-texture + n-patch + color palette bindings
- [ ] **Phase 67: Text & Fonts** — Custom font loading, glyph access, codepoint utilities, draw/measure text
- [ ] **Phase 68: Audio System** — Device init, Wave, Sound, Music, AudioStream (fully independent of graphics)
- [ ] **Phase 69: 3D Drawing & Camera3D** — All 3D primitives, camera modes, screen↔world rays
- [ ] **Phase 70: Models, Meshes, Materials, Animations** — Model load/draw, procedural meshes, materials, bones, billboards
- [ ] **Phase 71: Shaders** — Shader load/unload, uniform setting by type, shader location indices
- [ ] **Phase 72: File I/O & Utilities** — File/text I/O, filesystem queries, compress/encode, random
- [ ] **Phase 73: Idiomatic API Polish, Showcase & Integration Tests** — Cross-cutting sweep: methods-on-types, constructor sugar, typed enums everywhere, raylib showcase example, native + web parity, existing users still compile

## Phase Details

### Phase 60: Type & Enum Foundation

**Goal**: Every raylib type and enum is defined in Iron with exact C layout/ordinals, so every subsequent phase can freely pass these types across the FFI boundary without worrying about struct packing or enum value translation.

**Depends on**: Nothing (first phase of this milestone)

**Requirements**: TYPE-01, TYPE-02, TYPE-03, TYPE-04, TYPE-05, TYPE-06, TYPE-07, TYPE-08, TYPE-09, TYPE-10, TYPE-11, TYPE-12, TYPE-13, TYPE-14, TYPE-15, TYPE-16, TYPE-17, TYPE-18, TYPE-19, TYPE-20, TYPE-21, TYPE-22, TYPE-23, TYPE-24, TYPE-25, TYPE-26, TYPE-27, TYPE-28, TYPE-29, TYPE-30, TYPE-31, TYPE-32, ENUM-01, ENUM-02, ENUM-03, ENUM-04, ENUM-05, ENUM-06, ENUM-07, ENUM-08, ENUM-09, ENUM-10, ENUM-11, ENUM-12, ENUM-13, ENUM-14, ENUM-15, ENUM-16, ENUM-17, ENUM-18, ENUM-19, ENUM-20, ENUM-21, ENUM-22

**Success Criteria** (what must be TRUE):
  1. User can write `val v = Vector3(1.0, 2.0, 3.0)` for every raylib type listed in TYPE-01..30 and the resulting Iron object has exact C struct layout (verified by a round-trip test that passes the value to a C function and reads it back).
  2. User can write `.bilinear`, `.first_person`, `.key_space`, etc. for every raylib enum (ENUM-01..21), and the ordinal value passed across the FFI matches the raylib C constant byte-for-byte.
  3. User cannot dereference opaque pointer fields (Image.data, Mesh vertex pointers, Font glyphs, Sound stream, Music ctxData) from Iron code — they are hidden behind opaque handles.
  4. User can call `extern func TestStructLayout(v: Vector3, m: Matrix, r: Rectangle)` from Iron and the C side receives correctly-laid-out structs on both native (clang) and web (emcc) builds.

**Plans**: 8 plans

Plans:
- [x] 60-01-PLAN.md — Scaffold iron_raylib shim files (header + impl + layout assertion TU) and wire into native + web build pipelines
- [x] 60-02-PLAN.md — Core math types: Vector2, Vector3, Vector4, Quaternion, Matrix, Rectangle, Color (TYPE-01..06)
- [x] 60-03-PLAN.md — Image, Texture, RenderTexture, NPatchInfo, GlyphInfo, Font types with opaque pointers + nested embedding (TYPE-07..12)
- [x] 60-04-PLAN.md — Camera (2D), Camera3D, Transform, BoneInfo, Mesh, Shader, MaterialMap, Material, Model, ModelAnimation (TYPE-13..22)
- [x] 60-05-PLAN.md — Ray, RayCollision, BoundingBox, Wave, AudioStream, Sound, Music, FilePathList (TYPE-23..30)
- [x] 60-06-PLAN.md — TYPE-31/32 audit + Window/Draw/Audio/Files namespace type declarations
- [x] 60-07-PLAN.md — All 22 enums with explicit ordinals + compile-time raylib.h anchor assertions (ENUM-01..22)
- [x] 60-08-PLAN.md — Clean-break rewrite of pong.iron, game_raylib.iron, hello_raylib.iron + 6-color rescue palette

---

### Phase 61: Window & System

**Goal**: User can open a configured window, manage its state, query screen and monitor geometry, read/write the clipboard, take screenshots, drive the frame loop, and control the cursor — covering every `rcore.c` window/system function plus trace-log configuration.

**Depends on**: Phase 60 (needs ConfigFlags, TraceLogLevel, MouseCursor enums and Image type)

**Requirements**: WIN-01, WIN-02, WIN-03, WIN-04, WIN-05, WIN-06, WIN-07, WIN-08, WIN-09, WIN-10, WIN-11, WIN-12, WIN-13, FILE-07

**Success Criteria** (what must be TRUE):
  1. User can call `Window.init(800, 600, "Demo")` followed by a loop that runs until `Window.shouldClose()`, toggles fullscreen on F11, saves a screenshot on F12, and exits cleanly via `Window.close()`.
  2. User can query monitor count, get per-monitor position/width/height/refresh rate/name, and move the window to a chosen monitor at runtime.
  3. User can read the system clipboard text, write new clipboard text, and read a clipboard image into an `Image` value.
  4. User can hide/show/lock the cursor and query whether it is visible and on-screen.
  5. User can call `setTargetFPS(60)` and measure frame time via `getFrameTime()` and elapsed runtime via `getTime()` / `waitTime(seconds)`.

**Plans**: 4 plans

Plans:
- [ ] 61-01-PLAN.md — Struct-by-value return ABI smoke test + Window lifecycle (init/close/should_close) + state queries (is_ready/fullscreen/hidden/minimized/maximized/focused/resized)
- [ ] 61-02-PLAN.md — Window state toggles (toggle_fullscreen/borderless_windowed, maximize/minimize/restore, set/clear/is_state) + runtime properties (set_title/position/size/min_size/max_size/opacity/monitor/focused/icon)
- [ ] 61-03-PLAN.md — Screen geometry + monitor enumeration (Vector2 struct returns) + clipboard (text+image) + take_screenshot
- [ ] 61-04-PLAN.md — Config flags / trace log / event waiting + cursor control + frame loop (set_target_fps/get_fps/get_frame_time/get_time) + open_url + wait_time + remove ABI smoke shim + re-enable pong/game_raylib/hello_raylib PHASE 61 lines

---

### Phase 62: Input — Keyboard, Mouse, Gamepad, Touch, Gestures

**Goal**: User can read every input channel raylib supports — 101 keyboard keys, 7 mouse buttons plus position/delta/wheel, up to 4 gamepads with 18 buttons + 6 axes + vibration, touch points, and all 11 gestures — through typed Iron enum parameters (no raw ints).

**Depends on**: Phase 60 (needs KeyboardKey, MouseButton, MouseCursor, GamepadButton, GamepadAxis, Gesture enums and FilePathList type)

**Requirements**: INPUT-01, INPUT-02, INPUT-03, INPUT-04, INPUT-05, INPUT-06, INPUT-07, INPUT-08, INPUT-09, INPUT-10, INPUT-11, INPUT-12, INPUT-13

**Success Criteria** (what must be TRUE):
  1. User can write a program that logs `"W pressed"`, `"A held"`, `"S released"`, `"D up"` for every keyboard key via `isKeyPressed(.w)`, `isKeyDown(.a)`, `isKeyReleased(.s)`, `isKeyUp(.d)` — and `getKeyPressed()` / `getCharPressed()` drain the key queue as expected.
  2. User can read mouse position as a `Vector2`, read wheel movement as `Vector2`, set the cursor shape via `setMouseCursor(.pointing_hand)`, and handle all 7 mouse buttons with typed enums.
  3. User can detect connected gamepads, read their names, poll buttons with `GamepadButton` enum, read axes with `GamepadAxis` enum, trigger vibration, and load custom mapping strings.
  4. User can read touch position for up to N touch points, detect taps/holds/drags/swipes/pinches via typed `Gesture` enum values, and query drag/pinch vectors and angles.
  5. User can detect dropped files and iterate the resulting `FilePathList` in a typed Iron for-loop.

**Plans**: TBD

---

### Phase 63: 2D Drawing

**Goal**: User can draw every 2D primitive raylib supports — pixels, lines, circles, ellipses, rings, rectangles (with all gradient/rounded variants), triangles, polygons, splines — and control frame state via `beginDrawing`/`endDrawing`, `beginMode2D` with a `Camera2D`, `beginTextureMode`, `beginShaderMode`, `beginBlendMode`, and `beginScissorMode`.

**Depends on**: Phase 60 (needs Color, Vector2, Rectangle, Camera2D, BlendMode, RenderTexture, Shader types/enums)

**Requirements**: DRAW2D-01, DRAW2D-02, DRAW2D-03, DRAW2D-04, DRAW2D-05, DRAW2D-06, DRAW2D-07, DRAW2D-08, DRAW2D-09, DRAW2D-10, DRAW2D-11, DRAW2D-12, DRAW2D-13, DRAW2D-14, DRAW2D-15, DRAW2D-16

**Success Criteria** (what must be TRUE):
  1. User can compile and run a single Iron program that draws one variant of every 2D primitive — pixel, line (every variant), circle (every variant), ellipse, ring, rectangle (every variant including rounded and gradient), triangle (every variant), polygon, and every spline type — onto a single frame and see each one rendered correctly.
  2. User can wrap 2D draws in `beginMode2D(camera)` / `endMode2D` with a `Camera2D` that zooms and rotates, and the drawn shapes respond to camera transforms.
  3. User can draw into a `RenderTexture2D` via `beginTextureMode(target)` / `endTextureMode` and the resulting texture is sampleable in a later draw call.
  4. User can nest `beginShaderMode`, `beginBlendMode`, and `beginScissorMode` with stack-correct end calls without corrupting raylib's internal state.
  5. User can evaluate a point on every spline type via `getSplinePoint*` and the returned `Vector2` matches the drawn curve.

**Plans**: TBD

---

### Phase 64: Collision (2D + 3D)

**Goal**: User can test every collision raylib exposes — 2D rectangle/circle/point/line/triangle/polygon combinations and 3D sphere/box/ray/mesh/triangle/quad intersections — invoked as methods on `Rectangle`/`Vector2`/`Ray`/`BoundingBox` where the receiver is idiomatic.

**Depends on**: Phase 60 (needs Rectangle, Vector2, Vector3, Ray, RayCollision, BoundingBox). Can run in parallel with or after Phase 63.

**Requirements**: COLL-01, COLL-02

**Success Criteria** (what must be TRUE):
  1. User can write `rectA.collides(rectB)`, `circleA.collides(circleB)`, `point.insideRect(rect)`, `point.insidePolygon(poly)`, `lineA.intersects(lineB)`, and `rectA.intersection(rectB)` for every 2D combination raylib exposes, and the boolean/rect results match raylib's C-side answers.
  2. User can write `ray.hitSphere(center, radius)`, `ray.hitBox(box)`, `ray.hitMesh(mesh, transform)`, `ray.hitTriangle(p1, p2, p3)`, `ray.hitQuad(p1, p2, p3, p4)`, `boxA.collides(boxB)`, `box.collidesSphere(center, radius)`, `sphereA.collidesSphere(sphereB)` and get back `Bool` or `RayCollision` values with correct hit data.

**Plans**: TBD

---

### Phase 65: raymath

**Goal**: Every raymath function (all 143) is available as an idiomatic Iron method on the natural receiver (Vector2/3/4, Matrix, Quaternion) or as a freestanding scalar helper (Lerp, Clamp, Wrap, Remap, FloatEquals), with correct `Float32` ABI round-trip.

**Depends on**: Phase 60 (needs Vector2, Vector3, Vector4, Matrix types). Independent of graphics phases — can run in parallel with 61/62/63/64.

**Requirements**: MATH-01, MATH-02, MATH-03, MATH-04, MATH-05, MATH-06, MATH-07, MATH-08

**Success Criteria** (what must be TRUE):
  1. User can chain vector math idiomatically: `v.add(w).normalize().scale(5.0).dotProduct(axis)` compiles and evaluates to the same `Float32` result as the equivalent raymath C call sequence.
  2. User can build a view matrix with `Matrix.lookAt(eye, target, up)`, a projection matrix with `Matrix.perspective(fovy, aspect, near, far)`, multiply them, invert, transpose, and decompose — and each step matches the raymath C output.
  3. User can do quaternion rotations via `Quaternion.fromAxisAngle(axis, angle)`, `q.multiply(r)`, `q.slerp(s, t)`, `q.toMatrix()` and the resulting rotations match raymath's C reference.
  4. Every one of the ~143 raymath functions is exercised by at least one Iron-side test case with non-trivial input values, and the output matches raymath's C reference to within single-precision float tolerance.
  5. Freestanding scalar helpers (`Lerp(a, b, t)`, `Clamp(v, lo, hi)`, `Wrap(v, lo, hi)`, `Remap(v, inStart, inEnd, outStart, outEnd)`, `FloatEquals(a, b)`) are callable without a receiver.

**Plans**: TBD

---

### Phase 66: Textures & Images

**Goal**: User can load, generate, transform, draw, save, and unload both `Image` (CPU) and `Texture2D` (GPU) values, including cubemaps, render-textures, n-patches, and the full `Color` palette + color manipulation helpers — all ~65 `rtextures.c` functions.

**Depends on**: Phase 60 (Image, Texture, RenderTexture, NPatchInfo, PixelFormat, TextureFilter, TextureWrap, CubemapLayout, Color). Depends on Phase 63 (`beginTextureMode` lives in the 2D draw phase).

**Requirements**: TEX-01, TEX-02, TEX-03, TEX-04, TEX-05, TEX-06, TEX-07, TEX-08, TEX-09, TEX-10, TEX-11, TEX-12, TEX-13, TEX-14

**Success Criteria** (what must be TRUE):
  1. User can call `Image.load("test.png")`, manipulate the image (crop, resize, rotate, flip, color tint, blur, dither, alpha mask — every `Image*` transform), draw onto the image with `image.drawRectangle/Line/Circle/Text`, then call `image.toTexture()` and draw the resulting texture on screen.
  2. User can call `Texture.load("tex.png")`, configure its filter and wrap via `texture.setFilter(.bilinear)` / `texture.setWrap(.clamp)`, generate mipmaps, and draw it in every variant (basic, V, Ex, Rec, Pro, NPatch) with correct positioning.
  3. User can load a cubemap with `Texture.loadCubemap(image, .cross_3x4)` and a `RenderTexture2D` with `RenderTexture.load(w, h)`, use both in draw calls, and unload both without leaking GPU resources.
  4. User can manipulate colors via `color.tint(tint)`, `color.fade(0.5)`, `color.toHSV()`, `Color.fromHSV(hsv)`, `color.lerp(other, t)`, `color.alphaBlend(dst, src)` and get raylib-equivalent results.
  5. All 26 raylib `Color` palette constants (LIGHTGRAY through RAYWHITE) are available as Iron `val` constants and usable in any draw call.

**Plans**: TBD

---

### Phase 67: Text & Fonts

**Goal**: User can load custom fonts (TTF, BMP, FNT, from file or memory), draw text in every variant (`drawText`, `drawTextEx`, `drawTextPro`, codepoint variants), measure text, look up glyphs by codepoint, and manipulate UTF-8 / codepoints end-to-end.

**Depends on**: Phase 60 (Font, GlyphInfo, FontType types). Depends on Phase 66 (Font contains a `Texture2D` and uses `Image` for glyph atlas generation).

**Requirements**: TEXT-01, TEXT-02, TEXT-03, TEXT-04, TEXT-05, TEXT-06, TEXT-07, TEXT-08, TEXT-09, TEXT-10, TEXT-11, TEXT-12, TEXT-13

**Success Criteria** (what must be TRUE):
  1. User can call `Font.default()` to get raylib's built-in font and draw FPS overlay text at `(10, 10)` via `drawFPS`.
  2. User can call `Font.load("custom.ttf")` or `Font.loadEx("custom.ttf", 48, codepoints)`, draw with `font.drawEx(text, position, size, spacing, tint)` and `font.drawPro(text, position, origin, rotation, size, spacing, tint)`, measure with `font.measureEx(text, size, spacing) -> Vector2`, and unload cleanly.
  3. User can look up a glyph by codepoint via `font.getGlyphIndex(cp)` / `font.getGlyphInfo(cp)` / `font.getGlyphAtlasRec(cp)` and use the returned data to position custom draws.
  4. User can iterate a UTF-8 string codepoint-by-codepoint via `getCodepointNext` / `getCodepointPrevious` / `codepointToUTF8`, and load/unload codepoint and UTF-8 buffers without leaking.
  5. User can set text line spacing via `setTextLineSpacing` and multiline text draws respect the value.

**Plans**: TBD

---

### Phase 68: Audio System

**Goal**: User can initialize the audio device, load and play sound effects and music streams, convert between waves/sounds, stream raw audio via `AudioStream`, and control volume/pitch/pan for all three playback types — all ~35 `raudio.c` functions.

**Depends on**: Phase 60 (Wave, Sound, Music, AudioStream types). Fully independent of graphics phases — can run very early in parallel.

**Requirements**: AUDIO-01, AUDIO-02, AUDIO-03, AUDIO-04, AUDIO-05, AUDIO-06, AUDIO-07, AUDIO-08, AUDIO-09, AUDIO-10, AUDIO-11, AUDIO-12

**Success Criteria** (what must be TRUE):
  1. User can call `Audio.init()`, check `Audio.isReady()`, set master volume, and tear down with `Audio.close()` without crashing.
  2. User can call `Sound.load("beep.wav")`, play it via `sound.play()`, pause/resume/stop it, adjust volume/pitch/pan, check `sound.isPlaying()`, and unload cleanly.
  3. User can call `Music.load("track.ogg")`, play it in a loop that calls `music.update()` every frame, seek to a position via `music.seek(seconds)`, query `music.getTimeLength()` / `music.getTimePlayed()`, and unload cleanly.
  4. User can load a `Wave`, crop it, format-convert it, convert it to a `Sound` via `wave.toSound()`, and export it to disk.
  5. User can create an `AudioStream` with a custom sample rate/size/channel count, feed it raw frames via `stream.update(data, frameCount)`, check `stream.isProcessed()`, and attach a processor callback.

**Plans**: TBD

---

### Phase 69: 3D Drawing & Camera3D

**Goal**: User can draw every 3D primitive raylib supports (line, point, circle, triangle, cube, sphere, cylinder, capsule, plane, ray, grid — with Wires variants), drive a `Camera3D` through all 5 camera modes, and convert between screen and world coordinates.

**Depends on**: Phase 60 (Camera3D, CameraMode, CameraProjection, Ray, Vector3, Matrix). Depends on Phase 65 (raymath for matrix operations under the hood).

**Requirements**: DRAW3D-01, DRAW3D-02, DRAW3D-03, DRAW3D-04

**Success Criteria** (what must be TRUE):
  1. User can wrap 3D draws in `beginMode3D(camera)` / `endMode3D` with a `Camera3D` and see each 3D primitive (line, point, circle, triangle, triangle strip, cube, cube wires, sphere, sphere wires, cylinder, cylinder wires, capsule, capsule wires, plane, ray, grid) rendered correctly.
  2. User can call `camera.update(.first_person)`, `camera.update(.orbital)`, `camera.update(.free)`, `camera.update(.third_person)`, `camera.update(.custom)` and have keyboard/mouse input drive the camera correctly in each mode.
  3. User can convert a mouse screen position to a world ray via `getScreenToWorldRay(mouse, camera)` and project a world position back to screen via `getWorldToScreen(world, camera)`.
  4. User can extract the camera's view matrix via `getCameraMatrix(camera)` and pass it to a shader or custom transform.

**Plans**: TBD

---

### Phase 70: Models, Meshes, Materials, Animations

**Goal**: User can load `.obj`/`.gltf`/`.iqm` models, generate procedural meshes (cube, sphere, plane, cylinder, cone, torus, knot, heightmap, cubicmap, poly), load and assign materials with textures, play skeletal animations, draw individual meshes with instancing, draw billboards, and draw bounding boxes.

**Depends on**: Phase 60 (Model, Mesh, Material, MaterialMap, MaterialMapIndex, Transform, BoneInfo, ModelAnimation, BoundingBox). Depends on Phase 66 (Materials wrap Texture2D). Depends on Phase 69 (3D render context).

**Requirements**: MODEL-01, MODEL-02, MODEL-03, MODEL-04, MODEL-05, MODEL-06, MODEL-07, MODEL-08, MODEL-09, MODEL-10

**Success Criteria** (what must be TRUE):
  1. User can call `Model.load("character.gltf")`, draw it with `model.draw(position, scale, tint)` and all Ex/Wires/Points variants, query its bounding box, and unload cleanly.
  2. User can generate every procedural mesh type (`Mesh.cube`, `Mesh.sphere`, `Mesh.cylinder`, `Mesh.cone`, `Mesh.torus`, `Mesh.knot`, `Mesh.plane`, `Mesh.poly`, `Mesh.hemiSphere`, `Mesh.heightmap`, `Mesh.cubicmap`), upload them to the GPU, and draw them with a custom material and transform.
  3. User can load a material from a file, set its albedo/normal/metalness texture via `material.setTexture(.albedo, tex)`, apply it to a mesh index in a model, and see the texture rendered correctly.
  4. User can load a `.iqm` model with animations via `ModelAnimation.load("character.iqm")`, drive frame updates with `model.updateAnimation(anim, frame)`, and the skeleton animates.
  5. User can draw billboards via `drawBillboard` / `drawBillboardRec` / `drawBillboardPro` and bounding boxes via `drawBoundingBox` inside a 3D render pass.

**Plans**: TBD

---

### Phase 71: Shaders

**Goal**: User can load GLSL shaders from file or memory, resolve uniform and attribute locations, set uniforms by type (Float, Vec2/3/4, Int, IVec2/3/4, Sampler2D, Matrix, Texture), and combine shaders with render textures for post-processing.

**Depends on**: Phase 60 (Shader, ShaderLocationIndex, ShaderUniformDataType, ShaderAttributeDataType). Depends on Phase 66 (RenderTexture for post-processing pipelines) and Phase 63 (`beginShaderMode`).

**Requirements**: SHADER-01, SHADER-02, SHADER-03, SHADER-04

**Success Criteria** (what must be TRUE):
  1. User can call `Shader.load("vs.glsl", "fs.glsl")` or `Shader.loadFromMemory(vsCode, fsCode)`, check `shader.isValid()`, and unload cleanly.
  2. User can resolve a uniform location via `shader.getLocation("u_time")` and an attribute location via `shader.getLocationAttrib("a_position")`, then store and reuse the indices.
  3. User can set uniforms of every type — `shader.setValue(loc, 1.0, .float)`, `shader.setValue(loc, v2, .vec2)`, `shader.setValue(loc, iv3, .ivec3)`, `shader.setValueMatrix(loc, mat)`, `shader.setValueTexture(loc, tex)` — and the shader receives the correct data.
  4. User can combine `beginTextureMode(rt)` + draw + `endTextureMode` + `beginShaderMode(postFx)` + `drawTexture(rt.texture, ...)` + `endShaderMode` to implement a post-processing pipeline end-to-end.

**Plans**: TBD

---

### Phase 72: File I/O & Utilities

**Goal**: User can load and save raw file data and text, query the filesystem (exists, extension, directory, working dir), list directory contents, compress/decompress data, encode/decode base64, compute CRC32/MD5/SHA1, and drive a pseudo-random sequence — all from `rcore.c`'s file/data section.

**Depends on**: Phase 60 (FilePathList type). Fully independent of graphics/audio phases — can run early in parallel.

**Requirements**: FILE-01, FILE-02, FILE-03, FILE-04, FILE-05, FILE-06

**Success Criteria** (what must be TRUE):
  1. User can load raw file data into a byte buffer, save it back to a new path, and export it as a C array via `exportDataAsCode` with matching bytes.
  2. User can query whether a path exists, whether it's a file or directory, get its name/extension/parent directory, change the working directory, and make a new directory.
  3. User can list all files in a directory (optionally recursively, optionally filtered by extension) via `loadDirectoryFilesEx(path, filter, scanSubdirs)` returning a typed `FilePathList`, iterate it, and unload it.
  4. User can compress a byte buffer via `compressData`, decompress it back via `decompressData` and get byte-identical output; user can base64-encode and decode round-trip; user can compute CRC32/MD5/SHA1 hashes that match reference values.
  5. User can seed a random generator via `setRandomSeed(seed)`, pull values via `getRandomValue(lo, hi)`, and generate/release a `loadRandomSequence` buffer.

**Plans**: TBD

---

### Phase 73: Idiomatic API Polish, Showcase & Integration Tests

**Goal**: Verify the idiomatic Iron layer is consistent across every prior phase — methods on types where idiomatic, constructor sugar in place, typed enums at every call site, native + web parity, existing users still compile — and ship a `raylib_showcase` example that exercises a representative slice of every category end-to-end on both targets.

**Depends on**: Every prior phase (60 through 72). Runs as a cross-cutting sweep, not a new feature phase.

**Requirements**: API-01, API-02, API-03, API-04, API-05, API-06, API-07, API-08, API-09, API-10, API-11, API-12, API-13

**Success Criteria** (what must be TRUE):
  1. User can compile and run `examples/raylib_showcase/showcase.iron` on native via `iron run` and on web via `iron build --target=web` — and the showcase opens a window, handles input, draws 2D shapes, loads a texture, renders text with a custom font, plays a sound, enters 3D mode, draws a model with a material, applies a shader, calls several raymath helpers, reads a file, and exits cleanly.
  2. User can re-compile `examples/pong/pong.iron`, `tests/manual/game_raylib.iron`, and `tests/integration/web/hello_raylib.iron` without modifying a single line of source — the expanded API is a pure superset of the v1.2.0 binding surface.
  3. Every raylib function that takes a struct as its primary subject is callable as a method on the Iron type (`texture.setFilter(.bilinear)`, not `setTextureFilter(texture, 1)`); every `Load*`/`Unload*` pair is `Type.load(...)` / `instance.unload()`; every enum parameter uses the typed Iron enum, never raw `Int`.
  4. The new `src/stdlib/iron_raylib.c` shim file (if introduced) is minimal — only wrapping raylib functions that Iron's FFI cannot call directly — and is compiled into both `src/cli/build.c` and `src/cli/build_web.c` pipelines.
  5. The integration test suite has at least one end-to-end test per in-scope raylib category (WIN, INPUT, DRAW2D, COLL, TEX, TEXT, AUDIO, DRAW3D, MODEL, SHADER, MATH, FILE) asserting compile + link + symbol resolution succeed on both native and web targets.

**Plans**: TBD

---

## Progress

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 60. Type & Enum Foundation | 8/8 | Complete    | 2026-04-14 |
| 61. Window & System | 0/4 | Complete    | 2026-04-15 |
| 62. Input | 4/4 | Complete   | 2026-04-16 |
| 63. 2D Drawing | 2/4 | In Progress|  |
| 64. Collision | 0/? | Not started | - |
| 65. raymath | 0/? | Not started | - |
| 66. Textures & Images | 0/? | Not started | - |
| 67. Text & Fonts | 0/? | Not started | - |
| 68. Audio System | 0/? | Not started | - |
| 69. 3D Drawing & Camera3D | 0/? | Not started | - |
| 70. Models, Meshes, Materials | 0/? | Not started | - |
| 71. Shaders | 0/? | Not started | - |
| 72. File I/O & Utilities | 0/? | Not started | - |
| 73. Idiomatic API Polish & Showcase | 0/? | Not started | - |

## Coverage Summary

| Category | REQs | Phase |
|----------|------|-------|
| TYPE-01..32 | 32 | 60 |
| ENUM-01..22 | 22 | 60 |
| WIN-01..13 | 13 | 61 |
| FILE-07 | 1 | 61 |
| INPUT-01..13 | 13 | 62 |
| DRAW2D-01..16 | 16 | 63 |
| COLL-01..02 | 2 | 64 |
| MATH-01..08 | 8 | 65 |
| TEX-01..14 | 14 | 66 |
| TEXT-01..13 | 13 | 67 |
| AUDIO-01..12 | 12 | 68 |
| DRAW3D-01..04 | 4 | 69 |
| MODEL-01..10 | 10 | 70 |
| SHADER-01..04 | 4 | 71 |
| FILE-01..06 | 6 | 72 |
| API-01..13 | 13 | 73 |
| **Total** | **183** | **14 phases** |

**Coverage: 183/183 v2.0.0-alpha requirements mapped ✓ — 0 unmapped**

---
*Roadmap created: 2026-04-13*
