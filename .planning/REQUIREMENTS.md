# Requirements: Iron v2.0.0-alpha — Iron Builds Real Games

**Defined:** 2026-04-13
**Core Value:** Every Iron feature compiles to correct, working C code; the stdlib exposes raylib in idiomatic Iron form.
**Milestone goal:** Close every gap in the raylib binding so any 2D or 3D game is writable in pure Iron.
**Coverage rule:** 100% of public raylib API (raylib.h + raymath.h) in every in-scope category. Each requirement is a *capability*; the implementation must bind **all** raylib functions backing that capability, not a representative subset.

## v2.0.0-alpha Requirements

### Type Foundation

Iron `object` definitions whose memory layout matches the corresponding C struct exactly. Float fields use `Float32` (C `float` ABI). Each type lives in `src/stdlib/raylib.iron` and is constructible from Iron code.

- [x] **TYPE-01**: `Vector2` exists with `x: Float32, y: Float32` matching C `Vector2` layout (already present — verify and lock)
- [x] **TYPE-02**: `Vector3` exists with `x, y, z: Float32` matching C `Vector3` layout
- [x] **TYPE-03**: `Vector4` (alias `Quaternion`) exists with `x, y, z, w: Float32` matching C `Vector4` layout
- [x] **TYPE-04**: `Matrix` exists as a 4x4 column-major matrix of `Float32` matching C `Matrix` layout
- [x] **TYPE-05**: `Rectangle` exists with `x, y, width, height: Float32` matching C `Rectangle` layout
- [x] **TYPE-06**: `Color` exists with `r, g, b, a: UInt8` (already present — verify and lock)
- [x] **TYPE-07**: `Image` exists with `data` opaque pointer + `width, height, mipmaps, format` fields matching C `Image` layout
- [x] **TYPE-08**: `Texture` (alias `Texture2D`) exists with `id, width, height, mipmaps, format` matching C `Texture` layout
- [x] **TYPE-09**: `RenderTexture` (alias `RenderTexture2D`) exists with `id` + nested `texture: Texture` + `depth: Texture`
- [x] **TYPE-10**: `NPatchInfo` exists with `source: Rectangle, left, top, right, bottom: Int, layout: NPatchLayout`
- [x] **TYPE-11**: `GlyphInfo` exists with `value, offsetX, offsetY, advanceX: Int, image: Image`
- [x] **TYPE-12**: `Font` exists with `baseSize, glyphCount, glyphPadding: Int, texture: Texture2D`, plus opaque pointers for recs/glyphs
- [x] **TYPE-13**: `Camera2D` exists with `offset: Vector2, target: Vector2, rotation: Float32, zoom: Float32`
- [x] **TYPE-14**: `Camera3D` (alias `Camera`) exists with `position, target, up: Vector3, fovy: Float32, projection: CameraProjection`
- [x] **TYPE-15**: `Mesh` exists with all C `Mesh` fields (vertexCount, triangleCount, vertices/normals/texcoords/colors pointers, vaoId/vboId GPU handles)
- [x] **TYPE-16**: `Shader` exists with `id: Int` + locations array
- [x] **TYPE-17**: `MaterialMap` exists with `texture: Texture2D, color: Color, value: Float32`
- [x] **TYPE-18**: `Material` exists with `shader: Shader, maps` (array of MaterialMap), and shader params
- [x] **TYPE-19**: `Transform` exists with `translation: Vector3, rotation: Quaternion, scale: Vector3`
- [x] **TYPE-20**: `BoneInfo` exists with `name` + `parent: Int`
- [x] **TYPE-21**: `Model` exists with `transform: Matrix, meshCount, materialCount: Int, meshes/materials/meshMaterial/boneCount/bones/bindPose` matching C layout
- [x] **TYPE-22**: `ModelAnimation` exists with `boneCount, frameCount: Int, bones, framePoses, name` matching C layout
- [x] **TYPE-23**: `Ray` exists with `position, direction: Vector3`
- [x] **TYPE-24**: `RayCollision` exists with `hit: Bool, distance: Float32, point, normal: Vector3`
- [x] **TYPE-25**: `BoundingBox` exists with `min, max: Vector3`
- [x] **TYPE-26**: `Wave` exists with `frameCount, sampleRate, sampleSize, channels: Int, data` opaque pointer
- [x] **TYPE-27**: `AudioStream` exists with stream metadata + opaque audio buffer/processor pointers
- [x] **TYPE-28**: `Sound` exists with `stream: AudioStream, frameCount: Int`
- [x] **TYPE-29**: `Music` exists with `stream: AudioStream, frameCount, looping: Int, ctxType: Int, ctxData` opaque pointer
- [x] **TYPE-30**: `FilePathList` exists with `capacity, count: Int, paths` (string array)
- [x] **TYPE-31**: All types have constructors that take their fields in C-struct order so direct interop works
- [x] **TYPE-32**: All types whose C struct contains opaque pointers (Image data, Mesh vertex pointers, Font glyphs, Sound stream, Music ctxData) hide those pointers from Iron code so users cannot accidentally dereference them

### Enum Foundation

raylib's enums become Iron enums with explicit ordinal values that match the C constants exactly. All enums passed to extern functions use the typed enum (not raw `Int`).

- [x] **ENUM-01**: `KeyboardKey` enum covers all 101 raylib `KEY_*` constants (alphanumeric, function keys F1–F25, modifiers, special keys, keypad, Android volume/back/menu)
- [x] **ENUM-02**: `MouseButton` enum covers `LEFT, RIGHT, MIDDLE, SIDE, EXTRA, FORWARD, BACK`
- [x] **ENUM-03**: `MouseCursor` enum covers all 11 cursor shapes (`DEFAULT, ARROW, IBEAM, CROSSHAIR, POINTING_HAND, RESIZE_*` variants, `NOT_ALLOWED`)
- [x] **ENUM-04**: `GamepadButton` enum covers all 18 buttons (`UNKNOWN, LEFT_FACE_*, RIGHT_FACE_*, LEFT_TRIGGER_*, RIGHT_TRIGGER_*, MIDDLE_*, LEFT_THUMB, RIGHT_THUMB`)
- [x] **ENUM-05**: `GamepadAxis` enum covers all 6 axes (`LEFT_X/Y, RIGHT_X/Y, LEFT_TRIGGER, RIGHT_TRIGGER`)
- [x] **ENUM-06**: `ConfigFlags` enum covers all 15 window/graphics config bits (`VSYNC_HINT, FULLSCREEN_MODE, WINDOW_RESIZABLE, WINDOW_UNDECORATED, WINDOW_HIDDEN, WINDOW_MINIMIZED, WINDOW_MAXIMIZED, WINDOW_UNFOCUSED, WINDOW_TOPMOST, WINDOW_ALWAYS_RUN, WINDOW_TRANSPARENT, WINDOW_HIGHDPI, WINDOW_MOUSE_PASSTHROUGH, BORDERLESS_WINDOWED_MODE, MSAA_4X_HINT, INTERLACED_HINT`)
- [x] **ENUM-07**: `TraceLogLevel` enum covers all 8 log levels (`ALL, TRACE, DEBUG, INFO, WARNING, ERROR, FATAL, NONE`)
- [x] **ENUM-08**: `BlendMode` enum covers all 8 blend modes (`ALPHA, ADDITIVE, MULTIPLIED, ADD_COLORS, SUBTRACT_COLORS, ALPHA_PREMULTIPLY, CUSTOM, CUSTOM_SEPARATE`)
- [x] **ENUM-09**: `PixelFormat` enum covers all 25 pixel formats (uncompressed grayscale through compressed ASTC variants)
- [x] **ENUM-10**: `TextureFilter` enum covers all 6 filters (`POINT, BILINEAR, TRILINEAR, ANISOTROPIC_4X/8X/16X`)
- [x] **ENUM-11**: `TextureWrap` enum covers all 4 wrap modes (`REPEAT, CLAMP, MIRROR_REPEAT, MIRROR_CLAMP`)
- [x] **ENUM-12**: `CubemapLayout` enum covers all 5 layouts (`AUTO_DETECT, LINE_VERTICAL, LINE_HORIZONTAL, CROSS_3X4, CROSS_4X3`)
- [x] **ENUM-13**: `FontType` enum covers all 3 font types (`DEFAULT, BITMAP, SDF`)
- [x] **ENUM-14**: `CameraMode` enum covers all 5 modes (`CUSTOM, FREE, ORBITAL, FIRST_PERSON, THIRD_PERSON`)
- [x] **ENUM-15**: `CameraProjection` enum covers `PERSPECTIVE, ORTHOGRAPHIC`
- [x] **ENUM-16**: `MaterialMapIndex` enum covers all 12 material maps (`ALBEDO, METALNESS, NORMAL, ROUGHNESS, OCCLUSION, EMISSION, HEIGHT, CUBEMAP, IRRADIANCE, PREFILTER, BRDF`)
- [x] **ENUM-17**: `ShaderLocationIndex` enum covers all 26 shader locations (`VERTEX_POSITION/NORMAL/TANGENT/COLOR/TEXCOORD01/02`, `MATRIX_MVP/VIEW/PROJECTION/MODEL/NORMAL`, `VECTOR_VIEW`, `COLOR_DIFFUSE/SPECULAR/AMBIENT`, `MAP_ALBEDO/METALNESS/NORMAL/...`, etc.)
- [x] **ENUM-18**: `ShaderUniformDataType` enum covers all 9 uniform types (`FLOAT, VEC2/3/4, INT, IVEC2/3/4, SAMPLER2D`)
- [x] **ENUM-19**: `ShaderAttributeDataType` enum covers all 4 attribute types (`FLOAT, VEC2/3/4`)
- [x] **ENUM-20**: `Gesture` enum covers all 11 gestures (`NONE, TAP, DOUBLETAP, HOLD, DRAG, SWIPE_RIGHT/LEFT/UP/DOWN, PINCH_IN, PINCH_OUT`)
- [x] **ENUM-21**: `NPatchLayout` enum covers all 3 layouts (`NINE_PATCH, THREE_PATCH_VERTICAL, THREE_PATCH_HORIZONTAL`)
- [x] **ENUM-22**: All enum values match raylib C constant ordinals exactly so they pass through `extern func` calls correctly without translation

### Window & System (`rcore.c` — ~35 functions)

- [x] **WIN-01**: User can initialize a window with `Window.init(width, height, title)` and close it via `Window.close()` (or method on a Window receiver if Iron supports it)
- [x] **WIN-02**: User can query whether the window should close (`Window.shouldClose()`), whether it is ready, fullscreen, hidden, minimized, maximized, focused, or resized
- [x] **WIN-03**: User can toggle fullscreen, borderless windowed, maximized, minimized, restore, set window state from `ConfigFlags`, clear window state, and toggle a single config flag
- [x] **WIN-04**: User can change window properties at runtime — title, position, size, min/max size, opacity, monitor, focus, icon (single + multi-image)
- [x] **WIN-05**: User can query screen geometry — `getScreenWidth/Height`, `getRenderWidth/Height`, `getWindowPosition`, `getWindowScaleDPI`
- [x] **WIN-06**: User can enumerate monitors — `getMonitorCount, getCurrentMonitor, getMonitorPosition, getMonitorWidth/Height, getMonitorPhysicalWidth/Height, getMonitorRefreshRate, getMonitorName`
- [x] **WIN-07**: User can read and write the system clipboard (`getClipboardText, setClipboardText`) and read clipboard images (`getClipboardImage`)
- [x] **WIN-08**: User can enable/disable event waiting (`enableEventWaiting, disableEventWaiting`)
- [x] **WIN-09**: User can take a screenshot to a PNG file (`takeScreenshot`) and capture the screen to an Image
- [x] **WIN-10**: User can set the trace log level (`setTraceLogLevel`) and configure global flags before init via `setConfigFlags`
- [x] **WIN-11**: User can control the cursor — show, hide, lock to window, check visibility/onscreen state
- [x] **WIN-12**: User can drive the frame loop — `setTargetFPS, getFPS, getFrameTime, getTime`
- [x] **WIN-13**: User can open URLs in the default browser (`openURL`)

### Input (`rcore.c` input section — ~40 functions)

- [ ] **INPUT-01**: User can check key state with the `KeyboardKey` enum — `isKeyPressed, isKeyPressedRepeat, isKeyDown, isKeyReleased, isKeyUp`
- [ ] **INPUT-02**: User can read the input buffer — `getKeyPressed, getCharPressed`
- [ ] **INPUT-03**: User can configure the exit key (`setExitKey`)
- [x] **INPUT-04**: User can check mouse button state with `MouseButton` enum — `isMouseButtonPressed/Down/Released/Up`
- [x] **INPUT-05**: User can read mouse position as `Vector2` — `getMousePosition, getMouseX, getMouseY, getMouseDelta`
- [x] **INPUT-06**: User can configure mouse — `setMousePosition, setMouseOffset, setMouseScale, getMouseWheelMove, getMouseWheelMoveV`
- [x] **INPUT-07**: User can set the mouse cursor with the `MouseCursor` enum (`setMouseCursor`)
- [x] **INPUT-08**: User can detect connected gamepads — `isGamepadAvailable, getGamepadName, getGamepadButtonPressed, getGamepadAxisCount`
- [x] **INPUT-09**: User can read gamepad button state — `isGamepadButtonPressed/Down/Released/Up` with `GamepadButton` enum
- [x] **INPUT-10**: User can read gamepad axes with `GamepadAxis` enum (`getGamepadAxisMovement`) and trigger vibration (`setGamepadVibration`) and remap mappings (`setGamepadMappings`)
- [x] **INPUT-11**: User can detect touch — `getTouchX, getTouchY, getTouchPosition, getTouchPointId, getTouchPointCount`
- [x] **INPUT-12**: User can detect gestures with `Gesture` enum — `setGesturesEnabled, isGestureDetected, getGestureDetected, getGestureHoldDuration, getGestureDragVector, getGestureDragAngle, getGesturePinchVector, getGesturePinchAngle`
- [x] **INPUT-13**: User can detect file drops — `isFileDropped, loadDroppedFiles, unloadDroppedFiles` (returns `FilePathList`)

### 2D Drawing (`rshapes.c` + `rcore.c` draw section — ~55 functions)

- [x] **DRAW2D-01**: User can begin/end a frame — `Draw.begin()`, `Draw.end()` (or `beginDrawing`/`endDrawing`)
- [x] **DRAW2D-02**: User can clear the background — `Draw.clear(color)` / `clearBackground(color)`
- [x] **DRAW2D-03**: User can begin/end 2D camera mode — `beginMode2D(camera)`, `endMode2D` taking `Camera2D`
- [x] **DRAW2D-04**: User can begin/end render-to-texture — `beginTextureMode(target)`, `endTextureMode` taking `RenderTexture2D`
- [x] **DRAW2D-05**: User can begin/end shader mode and blend mode — `beginShaderMode(shader)`, `endShaderMode`, `beginBlendMode(mode)`, `endBlendMode`
- [x] **DRAW2D-06**: User can begin/end scissor mode — `beginScissorMode(x, y, w, h)`, `endScissorMode`
- [x] **DRAW2D-07**: User can draw pixels — `drawPixel(x, y, color)`, `drawPixelV(position, color)`
- [x] **DRAW2D-08**: User can draw lines in every variant — basic, V (Vector2 endpoints), Ex (thickness), Strip (multi-point), Bezier
- [x] **DRAW2D-09**: User can draw circles in every variant — `drawCircle, drawCircleSector, drawCircleSectorLines, drawCircleGradient, drawCircleV, drawCircleLines, drawCircleLinesV`
- [x] **DRAW2D-10**: User can draw ellipses in every variant — `drawEllipse, drawEllipseLines`
- [x] **DRAW2D-11**: User can draw rings — `drawRing, drawRingLines`
- [x] **DRAW2D-12**: User can draw rectangles in every variant — `drawRectangle, drawRectangleV, drawRectangleRec, drawRectanglePro, drawRectangleGradientV/H/Ex, drawRectangleLines, drawRectangleLinesEx, drawRectangleRounded, drawRectangleRoundedLines, drawRectangleRoundedLinesEx`
- [x] **DRAW2D-13**: User can draw triangles — `drawTriangle, drawTriangleLines, drawTriangleFan, drawTriangleStrip`
- [x] **DRAW2D-14**: User can draw regular polygons — `drawPoly, drawPolyLines, drawPolyLinesEx`
- [x] **DRAW2D-15**: User can draw splines — all 5 spline types (Linear, BasisB, CatmullRom, BezierQuadratic, BezierCubic) plus segment variants
- [x] **DRAW2D-16**: User can evaluate spline points — `getSplinePoint*` for all 5 spline types

### Collision (`rshapes.c` collision + `rmodels.c` 3D collision — ~15 functions)

- [x] **COLL-01**: User can test 2D rectangle/circle/point collisions — `checkCollisionRecs, checkCollisionCircles, checkCollisionCircleRec, checkCollisionCircleLine, checkCollisionPointRec, checkCollisionPointCircle, checkCollisionPointTriangle, checkCollisionPointLine, checkCollisionPointPoly, checkCollisionLines, getCollisionRec` — invoked as methods on `Rectangle`/`Vector2` where idiomatic
- [x] **COLL-02**: User can test 3D collisions — `checkCollisionSpheres, checkCollisionBoxes, checkCollisionBoxSphere, getRayCollisionSphere, getRayCollisionBox, getRayCollisionMesh, getRayCollisionTriangle, getRayCollisionQuad` — invoked as methods on `Ray`/`BoundingBox` where idiomatic

### Textures & Images (`rtextures.c` — ~65 functions)

- [x] **TEX-01**: User can load an `Image` from a file — `Image.load(path)` (PNG, JPG, BMP, TGA, GIF, QOI, PSD, DDS, HDR, KTX, PIC, PVR, PKM, ASTC) — and unload via `image.unload()`
- [x] **TEX-02**: User can load an `Image` from raw memory, file data buffer, screen, or compressed data
- [x] **TEX-03**: User can generate procedural images — `Image.color(w, h, color), Image.gradientLinear/Radial/Square, Image.checked, Image.whiteNoise, Image.perlinNoise, Image.cellular, Image.text, Image.textEx`
- [x] **TEX-04**: User can save an `Image` — `image.export(path), image.exportToMemory, image.exportAsCode`
- [x] **TEX-05**: User can transform an image in place — `image.toPOT, image.format, image.toPOT, image.crop, image.alphaCrop, image.alphaClear, image.alphaMask, image.alphaPremultiply, image.blurGaussian, image.kernelConvolution, image.resize, image.resizeNN, image.resizeCanvas, image.mipmaps, image.dither, image.flipVertical/Horizontal, image.rotate, image.rotateCW/CCW, image.colorTint, image.colorInvert, image.colorGrayscale, image.colorContrast, image.colorBrightness, image.colorReplace`
- [x] **TEX-06**: User can extract image data — `image.loadColors, image.loadPalette, image.getAlphaBorder, image.getColor`
- [x] **TEX-07**: User can draw onto an `Image` (CPU-side) — `image.drawPixel, image.drawLine, image.drawCircle, image.drawRectangle, image.draw, image.drawText` (full set of `ImageDraw*` functions)
- [x] **TEX-08**: User can load a `Texture2D` from a file or `Image` — `Texture.load(path), image.toTexture()` — and unload via `texture.unload()`
- [x] **TEX-09**: User can load cubemaps and `RenderTexture2D` — `Texture.loadCubemap(image, layout), RenderTexture.load(width, height)` and unload them
- [x] **TEX-10**: User can update texture data — `texture.update(pixels), texture.updateRec(rec, pixels)`
- [x] **TEX-11**: User can configure texture filter and wrap — `texture.setFilter(TextureFilter), texture.setWrap(TextureWrap), texture.genMipmaps()`
- [x] **TEX-12**: User can draw a texture in every variant — `drawTexture, drawTextureV, drawTextureEx, drawTextureRec, drawTexturePro, drawTextureNPatch` invoked as methods on `Texture` where idiomatic
- [x] **TEX-13**: User can manipulate `Color` values — `colorIsEqual, fade, colorToInt, colorNormalize, colorFromNormalized, colorToHSV, colorFromHSV, colorTint, colorBrightness, colorContrast, colorAlpha, colorAlphaBlend, colorLerp, getColor, getPixelColor, setPixelColor, getPixelDataSize` — exposed as methods/constructors on `Color`
- [x] **TEX-14**: All raylib `Color` palette constants are present (LIGHTGRAY, GRAY, DARKGRAY, YELLOW, GOLD, ORANGE, PINK, RED, MAROON, GREEN, LIME, DARKGREEN, SKYBLUE, BLUE, DARKBLUE, PURPLE, VIOLET, DARKPURPLE, BEIGE, BROWN, DARKBROWN, WHITE, BLACK, BLANK, MAGENTA, RAYWHITE)

### Text & Fonts (`rtext.c` — ~30 functions)

- [x] **TEXT-01**: User can get the default font — `Font.default()` / `getFontDefault`
- [x] **TEXT-02**: User can load a `Font` from file (TTF, BMP, FNT, etc.) — `Font.load(path)` — and unload via `font.unload()`
- [ ] **TEXT-03**: User can load a font with explicit size, codepoint set, and SDF support — `Font.loadEx(path, size, codepoints), Font.loadFromImage(image, key, firstChar), Font.loadFromMemory(type, data, size, codepoints)`
- [x] **TEXT-04**: User can check whether a font is ready (`isFontValid`)
- [ ] **TEXT-05**: User can load and unload font glyph data (`loadFontData, unloadFontData`) and generate a font atlas (`genImageFontAtlas`)
- [x] **TEXT-06**: User can export a font as code (`exportFontAsCode`)
- [ ] **TEXT-07**: User can draw the FPS overlay — `drawFPS(x, y)`
- [ ] **TEXT-08**: User can draw text in every variant — `drawText, drawTextEx, drawTextPro, drawTextCodepoint, drawTextCodepoints` — invoked as methods on `Font` or as freestanding draws
- [ ] **TEXT-09**: User can set text line spacing — `setTextLineSpacing`
- [ ] **TEXT-10**: User can measure text — `measureText, measureTextEx` returning `Vector2` for width/height
- [ ] **TEXT-11**: User can look up glyphs by codepoint — `getGlyphIndex, getGlyphInfo, getGlyphAtlasRec`
- [ ] **TEXT-12**: User can manipulate UTF-8 / codepoints — `loadUTF8, unloadUTF8, loadCodepoints, unloadCodepoints, getCodepointCount, getCodepoint, getCodepointNext, getCodepointPrevious, codepointToUTF8`
- [ ] **TEXT-13**: User can manipulate text strings — `textCopy, textIsEqual, textLength, textFormat, textSubtext, textReplace, textInsert, textJoin, textSplit, textAppend, textFindIndex, textToUpper, textToLower, textToPascal, textToSnake, textToCamel, textToInteger, textToFloat` — note: where Iron's native String methods already cover these, expose as no-op aliases or shadow

### Audio (`raudio.c` — ~35 functions)

- [ ] **AUDIO-01**: User can initialize and shut down the audio device — `Audio.init()`, `Audio.close()` / `initAudioDevice, closeAudioDevice, isAudioDeviceReady, setMasterVolume, getMasterVolume`
- [ ] **AUDIO-02**: User can load and unload a `Wave` from file or memory — `Wave.load(path)`, `wave.unload()`, `Wave.loadFromMemory(type, data)`
- [ ] **AUDIO-03**: User can convert a wave to/from a sound — `wave.toSound(), wave.copy(), wave.crop(initFrame, finalFrame), wave.format(sampleRate, sampleSize, channels), wave.loadSamples(), wave.unloadSamples()`
- [ ] **AUDIO-04**: User can export waves — `wave.export(path), wave.exportAsCode(path)`
- [ ] **AUDIO-05**: User can load a `Sound` — `Sound.load(path), Sound.fromWave(wave), Sound.loadAlias(source)` — and unload via `sound.unload()`, `sound.unloadAlias()`
- [ ] **AUDIO-06**: User can play, stop, pause, resume sounds — `sound.play(), sound.stop(), sound.pause(), sound.resume(), sound.isPlaying()`
- [ ] **AUDIO-07**: User can configure sound playback — `sound.setVolume(v), sound.setPitch(p), sound.setPan(p)`
- [ ] **AUDIO-08**: User can update sound buffer data — `sound.update(data, sampleCount)`
- [ ] **AUDIO-09**: User can load `Music` streams from file or memory — `Music.load(path), Music.loadFromMemory(type, data)` — and unload via `music.unload()`
- [ ] **AUDIO-10**: User can play, stop, pause, resume, update music — `music.play(), music.update(), music.stop(), music.pause(), music.resume(), music.isPlaying()`
- [ ] **AUDIO-11**: User can configure music playback — `music.setVolume(v), music.setPitch(p), music.setPan(p), music.seek(position), music.getTimeLength(), music.getTimePlayed()`
- [ ] **AUDIO-12**: User can work with `AudioStream` directly — `AudioStream.load(sampleRate, sampleSize, channels), stream.unload(), stream.update(data, frameCount), stream.isProcessed(), stream.play/pause/resume/stop/isPlaying, stream.setVolume/Pitch/Pan, stream.setBufferSizeDefault, stream.setCallback, stream.attachProcessor, stream.detachProcessor, attachAudioMixedProcessor, detachAudioMixedProcessor`

### 3D Drawing (`rmodels.c` 3D draw section — ~25 functions)

- [ ] **DRAW3D-01**: User can begin/end 3D camera mode — `beginMode3D(camera), endMode3D` taking `Camera3D`
- [ ] **DRAW3D-02**: User can update a `Camera3D` — `camera.update(mode), camera.updatePro(movement, rotation, zoom)` taking `CameraMode`
- [ ] **DRAW3D-03**: User can convert screen positions to rays — `getScreenToWorldRay, getScreenToWorldRayEx, getWorldToScreen, getWorldToScreenEx, getCameraMatrix`
- [ ] **DRAW3D-04**: User can draw 3D primitives — `drawLine3D, drawPoint3D, drawCircle3D, drawTriangle3D, drawTriangleStrip3D, drawCube, drawCubeV, drawCubeWires, drawCubeWiresV, drawSphere, drawSphereEx, drawSphereWires, drawCylinder, drawCylinderEx, drawCylinderWires, drawCylinderWiresEx, drawCapsule, drawCapsuleWires, drawPlane, drawRay, drawGrid`

### Models, Meshes, Materials (`rmodels.c` — ~45 functions)

- [ ] **MODEL-01**: User can load and unload a `Model` — `Model.load(path), Model.fromMesh(mesh), model.unload(), model.isReady()`
- [ ] **MODEL-02**: User can query model bounds — `model.getBoundingBox()`
- [ ] **MODEL-03**: User can draw a model in every variant — `model.draw(position, scale, tint), model.drawEx(position, axis, angle, scale, tint), model.drawWires(...), model.drawWiresEx(...), model.drawPoints(...), model.drawPointsEx(...)`
- [ ] **MODEL-04**: User can manipulate meshes — `mesh.upload(dynamic), mesh.updateBuffer(index, data, offset), mesh.unload(), mesh.export(path), mesh.exportAsCode(path), mesh.getBoundingBox(), mesh.genTangents()`
- [ ] **MODEL-05**: User can draw individual meshes — `mesh.draw(material, transform), mesh.drawInstanced(material, transforms, instances)`
- [ ] **MODEL-06**: User can generate procedural meshes — `Mesh.poly(sides, radius), Mesh.plane(width, length, resX, resZ), Mesh.cube(w, h, l), Mesh.sphere(radius, rings, slices), Mesh.hemiSphere(...), Mesh.cylinder(...), Mesh.cone(...), Mesh.torus(...), Mesh.knot(...), Mesh.heightmap(image, size), Mesh.cubicmap(image, size)`
- [ ] **MODEL-07**: User can load and use materials — `Material.load(path), Material.default(), material.unload(), material.setTexture(mapType, texture)` and apply via `model.setMeshMaterial(meshIndex, materialIndex)`
- [ ] **MODEL-08**: User can load and play model animations — `ModelAnimation.load(path)` returns array, `model.updateAnimation(anim, frame), model.updateAnimationBones(anim, frame), animation.isValid(), ModelAnimation.unload(animations)`
- [ ] **MODEL-09**: User can draw billboards — `drawBillboard(camera, texture, position, size, tint), drawBillboardRec, drawBillboardPro`
- [ ] **MODEL-10**: User can draw bounding boxes — `drawBoundingBox(box, color)`

### Shaders (`rcore.c` shader section — ~15 functions)

- [ ] **SHADER-01**: User can load a `Shader` from file or memory — `Shader.load(vsPath, fsPath), Shader.loadFromMemory(vsCode, fsCode), shader.unload(), shader.isValid()`
- [ ] **SHADER-02**: User can resolve shader locations — `shader.getLocation(uniformName), shader.getLocationAttrib(attribName), shader.setLocation(index, location)`
- [ ] **SHADER-03**: User can set shader uniforms by type — `shader.setValue(loc, value, dataType), shader.setValueV(loc, values, dataType, count), shader.setValueMatrix(loc, mat), shader.setValueTexture(loc, texture)` — supports all `ShaderUniformDataType` values
- [ ] **SHADER-04**: User can begin/end shader mode (covered by DRAW2D-05) and combine shaders with render textures (covered by DRAW2D-04, TEX-09)

### Math — raymath helpers (`raymath.h` — 143 functions)

raymath functions become **methods on Vector2/Vector3/Vector4/Matrix/Quaternion** where the receiver is natural. Free functions only when no sensible receiver exists (`Lerp(a, b, t)`, `Clamp(v, lo, hi)`, `Wrap(v, lo, hi)`, `Normalize(v, lo, hi)`).

- [x] **MATH-01**: Scalar utilities — `Lerp, Clamp, Normalize, Wrap, FloatEquals, Remap` exist as freestanding functions
- [x] **MATH-02**: `Vector2` has methods for **all** raymath `Vector2*` operations: `zero, one, add, addValue, subtract, subtractValue, length, lengthSqr, dotProduct, distance, distanceSqr, angle, lineAngle, scale, multiply, negate, divide, normalize, transform, lerp, reflect, min, max, clamp, clampValue, equals, refract, rotate, moveTowards, invert`
- [x] **MATH-03**: `Vector3` has methods for **all** raymath `Vector3*` operations: `zero, one, add, addValue, subtract, subtractValue, scale, multiply, crossProduct, perpendicular, length, lengthSqr, dotProduct, distance, distanceSqr, angle, negate, divide, normalize, project, reject, orthoNormalize, transform, rotateByQuaternion, rotateByAxisAngle, moveTowards, lerp, cubicHermite, reflect, min, max, barycenter, unproject, toFloatV, invert, clamp, clampValue, equals, refract, distanceSqr`
- [x] **MATH-04**: `Vector4` has methods for **all** raymath `Vector4*` operations: `zero, one, add, addValue, subtract, subtractValue, length, lengthSqr, dotProduct, distance, distanceSqr, scale, multiply, negate, divide, normalize, min, max, lerp, moveTowards, invert, equals`
- [x] **MATH-05**: `Matrix` has methods for **all** raymath `Matrix*` operations: `determinant, trace, transpose, invert, identity, add, subtract, multiply, translate, rotate, rotateX, rotateY, rotateZ, rotateXYZ, rotateZYX, scale, frustum, perspective, ortho, lookAt, toFloatV, decompose`
- [x] **MATH-06**: `Quaternion` (alias for `Vector4`) has methods for **all** raymath `Quaternion*` operations: `add, addValue, subtract, subtractValue, identity, length, normalize, invert, multiply, scale, divide, lerp, nlerp, slerp, cubicHermiteSpline, fromVector3ToVector3, fromMatrix, toMatrix, fromAxisAngle, toAxisAngle, fromEuler, toEuler, transform, equals`
- [x] **MATH-07**: All 143 raymath functions are individually testable via Iron code; each function in raymath.h has at least one Iron-side test that exercises it with non-trivial values
- [x] **MATH-08**: Math operations preserve C ABI — Iron `Float32` round-trips through raymath without precision loss or alignment issues

### File I/O & Utilities (`rcore.c` file/data section — ~40 functions)

- [ ] **FILE-01**: User can load and save raw file data — `loadFileData(path), unloadFileData, saveFileData(path, data, size), exportDataAsCode(data, size, path)`
- [ ] **FILE-02**: User can load and save text — `loadFileText(path), unloadFileText, saveFileText(path, text)`
- [ ] **FILE-03**: User can query the filesystem — `fileExists, directoryExists, isFileExtension, getFileLength, getFileExtension, getFileName, getFileNameWithoutExt, getDirectoryPath, getPrevDirectoryPath, getWorkingDirectory, getApplicationDirectory, changeDirectory, isPathFile, isFileNameValid, getFileModTime, makeDirectory`
- [ ] **FILE-04**: User can list directories — `loadDirectoryFiles(path), loadDirectoryFilesEx(path, filter, scanSubdirs), unloadDirectoryFiles` returning `FilePathList`
- [ ] **FILE-05**: User can compress/decompress data and encode/decode base64 — `compressData, decompressData, encodeDataBase64, decodeDataBase64, computeCRC32, computeMD5, computeSHA1`
- [ ] **FILE-06**: User can interact with random — `setRandomSeed, getRandomValue, loadRandomSequence, unloadRandomSequence`
- [x] **FILE-07**: User can drive timing — `waitTime(seconds)` (covered by WIN-12 for the rest)

### Idiomatic API Layer (cross-cutting)

- [ ] **API-01**: Where a raylib function takes a struct as its primary subject (Texture, Sound, Music, Image, Mesh, Model, Shader, Font, Wave, AudioStream, Camera2D, Camera3D, Material, Vector2/3/4, Matrix, Rectangle, Ray, BoundingBox), the binding exposes that operation as a **method on the type** — not a freestanding function — when the receiver is unambiguous
- [ ] **API-02**: Resource lifecycle uses the idiomatic Iron form: types with `Load*` / `Unload*` C pairs become `Type.load(...)` constructors and `instance.unload()` methods
- [ ] **API-03**: Constructor sugar: `Color.rgb(r, g, b)` defaults `a=255`; `Color.rgba(r, g, b, a)` is explicit; `Vector2.of(x, y)`, `Vector3.of(x, y, z)`, `Rectangle.of(x, y, w, h)` are short constructors that use `Float32` literals
- [ ] **API-04**: All raylib enum parameters in the Iron API use the typed Iron enum, never raw `Int` (e.g., `texture.setFilter(.bilinear)` not `texture.setFilter(1)`)
- [ ] **API-05**: Functions returning multiple `Float`/`Int` values use Iron tuple returns where natural (e.g., `getMousePosition() -> (Float32, Float32)` if a Vector2 wrapper is awkward, otherwise return `Vector2`)
- [ ] **API-06**: Functions that take out-params in C use Iron return values (e.g., raylib's `MeasureTextEx` writes to a `Vector2*`; Iron's version returns the `Vector2`)
- [ ] **API-07**: A `Window` type wraps the implicit window state where it improves ergonomics — e.g., `Window.init(...)` returning a value that supports `.shouldClose()`, `.close()` — without breaking the freestanding form for users who prefer it
- [x] **API-08**: A new C shim file `src/stdlib/iron_raylib.c` is introduced when (and only when) raylib's API cannot be called directly from Iron — e.g., for `TextFormat` varargs, struct-by-value returns that don't fit Iron's ABI, or out-param translation. The shim keeps wrappers minimal and is built into both native and web targets
- [x] **API-09**: Both native and web (`iron build` and `iron build --target=web`) targets compile and link successfully against the expanded binding; no native-only or web-only binding is introduced
- [ ] **API-10**: A canonical example program (`examples/raylib_showcase/showcase.iron` or similar) exercises a representative slice of every category — window, input, 2D draw, texture, font, audio, 3D draw, model, shader, math — and builds + runs on both targets
- [x] **API-11** (override, closed by Phase 60 Plan 08): Existing raylib users (`examples/pong/pong.iron`, `tests/manual/game_raylib.iron`, `tests/integration/web/hello_raylib.iron`) MAY be rewritten during the v2.0.0-alpha clean break. Original text said "continue to compile without modification" — Phase 60 (60-CONTEXT.md "Backward Compatibility — Clean Break") relaxes this to "may be rewritten in the same commit that lands the new binding surface" because the new API is strictly better, the existing files are few and under our control, and maintaining deprecated names as aliases would fork the docs. Closed by Plan 60-08 which rewrote all three files to use canonical Phase 60 names (Vector2 not Vec2, KeyboardKey.SPACE not Key.SPACE, Color(r,g,b,a) not extern constants).
- [ ] **API-12**: The integration test suite gains end-to-end coverage for at least one function from every in-scope raylib category, asserting compile + link + symbol resolution succeeds
- [ ] **API-13**: All bound functions appear in `src/stdlib/raylib.iron` (or are included from a sibling `.iron` file imported by it) and are discoverable through the existing `import raylib` mechanism in `src/cli/build.c`

## Out of Scope

| Feature | Reason |
|---------|--------|
| VR stereo rendering (`VrDeviceInfo`, `VrStereoConfig`, `BeginVrStereoMode`, `EndVrStereoMode`, `LoadVrStereoConfig`, `UnloadVrStereoConfig`) | Niche use case; defer to a later milestone when there's user demand |
| Automation events (`AutomationEvent`, `AutomationEventList`, `LoadAutomationEventList`, `PlayAutomationEvent`, `StartAutomationEventRecording`, `StopAutomationEventRecording`, `SetAutomationEventBaseFrame`, `SetAutomationEventList`) | Tooling/replay system, not a "real games" blocker |
| `rlgl.h` (low-level OpenGL abstraction) | Wraps the layer below raylib; not needed for application code |
| `rcamera.h` (separate camera helpers if any beyond what raylib.h exposes) | Already covered by raylib.h `UpdateCamera*` |
| Custom raylib version bump (5.5 → 5.x) | Vendored 5.5 stays put for this milestone |
| Re-architecting `src/cli/build.c` raylib pipeline | Current per-source clang/emcc compilation is correct; no work needed |
| Iron language changes to support raylib better | This milestone is binding work; if a language gap blocks a binding, it's surfaced as a phase blocker, not solved here |
| OpenGL ES / mobile-only raylib features | Web target (Emscripten) is the only "non-desktop" target in scope |

## Traceability

Which phases cover which requirements. Filled in by roadmapper on 2026-04-13.

| Requirement | Phase | Status |
|-------------|-------|--------|
| TYPE-01 | Phase 60 | Complete |
| TYPE-02 | Phase 60 | Complete |
| TYPE-03 | Phase 60 | Complete |
| TYPE-04 | Phase 60 | Complete |
| TYPE-05 | Phase 60 | Complete |
| TYPE-06 | Phase 60 | Complete |
| TYPE-07 | Phase 60 | Complete |
| TYPE-08 | Phase 60 | Complete |
| TYPE-09 | Phase 60 | Complete |
| TYPE-10 | Phase 60 | Complete |
| TYPE-11 | Phase 60 | Complete |
| TYPE-12 | Phase 60 | Complete |
| TYPE-13 | Phase 60 | Complete |
| TYPE-14 | Phase 60 | Complete |
| TYPE-15 | Phase 60 | Complete |
| TYPE-16 | Phase 60 | Complete |
| TYPE-17 | Phase 60 | Complete |
| TYPE-18 | Phase 60 | Complete |
| TYPE-19 | Phase 60 | Complete |
| TYPE-20 | Phase 60 | Complete |
| TYPE-21 | Phase 60 | Complete |
| TYPE-22 | Phase 60 | Complete |
| TYPE-23 | Phase 60 | Complete |
| TYPE-24 | Phase 60 | Complete |
| TYPE-25 | Phase 60 | Complete |
| TYPE-26 | Phase 60 | Complete |
| TYPE-27 | Phase 60 | Complete |
| TYPE-28 | Phase 60 | Complete |
| TYPE-29 | Phase 60 | Complete |
| TYPE-30 | Phase 60 | Complete |
| TYPE-31 | Phase 60 | Complete |
| TYPE-32 | Phase 60 | Complete |
| ENUM-01 | Phase 60 | Complete |
| ENUM-02 | Phase 60 | Complete |
| ENUM-03 | Phase 60 | Complete |
| ENUM-04 | Phase 60 | Complete |
| ENUM-05 | Phase 60 | Complete |
| ENUM-06 | Phase 60 | Complete |
| ENUM-07 | Phase 60 | Complete |
| ENUM-08 | Phase 60 | Complete |
| ENUM-09 | Phase 60 | Complete |
| ENUM-10 | Phase 60 | Complete |
| ENUM-11 | Phase 60 | Complete |
| ENUM-12 | Phase 60 | Complete |
| ENUM-13 | Phase 60 | Complete |
| ENUM-14 | Phase 60 | Complete |
| ENUM-15 | Phase 60 | Complete |
| ENUM-16 | Phase 60 | Complete |
| ENUM-17 | Phase 60 | Complete |
| ENUM-18 | Phase 60 | Complete |
| ENUM-19 | Phase 60 | Complete |
| ENUM-20 | Phase 60 | Complete |
| ENUM-21 | Phase 60 | Complete |
| ENUM-22 | Phase 60 | Complete |
| WIN-01 | Phase 61 | Complete |
| WIN-02 | Phase 61 | Complete |
| WIN-03 | Phase 61 | Complete |
| WIN-04 | Phase 61 | Complete |
| WIN-05 | Phase 61 | Complete |
| WIN-06 | Phase 61 | Complete |
| WIN-07 | Phase 61 | Complete |
| WIN-08 | Phase 61 | Complete |
| WIN-09 | Phase 61 | Complete |
| WIN-10 | Phase 61 | Complete |
| WIN-11 | Phase 61 | Complete |
| WIN-12 | Phase 61 | Complete |
| WIN-13 | Phase 61 | Complete |
| INPUT-01 | Phase 62 | Pending |
| INPUT-02 | Phase 62 | Pending |
| INPUT-03 | Phase 62 | Pending |
| INPUT-04 | Phase 62 | Pending |
| INPUT-05 | Phase 62 | Pending |
| INPUT-06 | Phase 62 | Pending |
| INPUT-07 | Phase 62 | Pending |
| INPUT-08 | Phase 62 | Pending |
| INPUT-09 | Phase 62 | Pending |
| INPUT-10 | Phase 62 | Pending |
| INPUT-11 | Phase 62 | Pending |
| INPUT-12 | Phase 62 | Pending |
| INPUT-13 | Phase 62 | Pending |
| DRAW2D-01 | Phase 63 | Complete |
| DRAW2D-02 | Phase 63 | Complete |
| DRAW2D-03 | Phase 63 | Complete |
| DRAW2D-04 | Phase 63 | Complete |
| DRAW2D-05 | Phase 63 | Complete |
| DRAW2D-06 | Phase 63 | Complete |
| DRAW2D-07 | Phase 63 | Complete |
| DRAW2D-08 | Phase 63 | Complete |
| DRAW2D-09 | Phase 63 | Complete |
| DRAW2D-10 | Phase 63 | Complete |
| DRAW2D-11 | Phase 63 | Complete |
| DRAW2D-12 | Phase 63 | Complete |
| DRAW2D-13 | Phase 63 | Complete |
| DRAW2D-14 | Phase 63 | Complete |
| DRAW2D-15 | Phase 63 | Complete |
| DRAW2D-16 | Phase 63 | Complete |
| COLL-01 | Phase 64 | Complete |
| COLL-02 | Phase 64 | Complete |
| MATH-01 | Phase 65 | Complete |
| MATH-02 | Phase 65 | Complete |
| MATH-03 | Phase 65 | Complete |
| MATH-04 | Phase 65 | Complete |
| MATH-05 | Phase 65 | Complete |
| MATH-06 | Phase 65 | Complete |
| MATH-07 | Phase 65 | Complete |
| MATH-08 | Phase 65 | Complete |
| TEX-01 | Phase 66 | Complete |
| TEX-02 | Phase 66 | Complete |
| TEX-03 | Phase 66 | Complete |
| TEX-04 | Phase 66 | Complete |
| TEX-05 | Phase 66 | Complete |
| TEX-06 | Phase 66 | Complete |
| TEX-07 | Phase 66 | Complete |
| TEX-08 | Phase 66 | Complete |
| TEX-09 | Phase 66 | Complete |
| TEX-10 | Phase 66 | Complete |
| TEX-11 | Phase 66 | Complete |
| TEX-12 | Phase 66 | Complete |
| TEX-13 | Phase 66 | Complete |
| TEX-14 | Phase 66 | Complete |
| TEXT-01 | Phase 67 | Complete |
| TEXT-02 | Phase 67 | Complete |
| TEXT-03 | Phase 67 | Pending |
| TEXT-04 | Phase 67 | Complete |
| TEXT-05 | Phase 67 | Pending |
| TEXT-06 | Phase 67 | Complete |
| TEXT-07 | Phase 67 | Pending |
| TEXT-08 | Phase 67 | Pending |
| TEXT-09 | Phase 67 | Pending |
| TEXT-10 | Phase 67 | Pending |
| TEXT-11 | Phase 67 | Pending |
| TEXT-12 | Phase 67 | Pending |
| TEXT-13 | Phase 67 | Pending |
| AUDIO-01 | Phase 68 | Pending |
| AUDIO-02 | Phase 68 | Pending |
| AUDIO-03 | Phase 68 | Pending |
| AUDIO-04 | Phase 68 | Pending |
| AUDIO-05 | Phase 68 | Pending |
| AUDIO-06 | Phase 68 | Pending |
| AUDIO-07 | Phase 68 | Pending |
| AUDIO-08 | Phase 68 | Pending |
| AUDIO-09 | Phase 68 | Pending |
| AUDIO-10 | Phase 68 | Pending |
| AUDIO-11 | Phase 68 | Pending |
| AUDIO-12 | Phase 68 | Pending |
| DRAW3D-01 | Phase 69 | Pending |
| DRAW3D-02 | Phase 69 | Pending |
| DRAW3D-03 | Phase 69 | Pending |
| DRAW3D-04 | Phase 69 | Pending |
| MODEL-01 | Phase 70 | Pending |
| MODEL-02 | Phase 70 | Pending |
| MODEL-03 | Phase 70 | Pending |
| MODEL-04 | Phase 70 | Pending |
| MODEL-05 | Phase 70 | Pending |
| MODEL-06 | Phase 70 | Pending |
| MODEL-07 | Phase 70 | Pending |
| MODEL-08 | Phase 70 | Pending |
| MODEL-09 | Phase 70 | Pending |
| MODEL-10 | Phase 70 | Pending |
| SHADER-01 | Phase 71 | Pending |
| SHADER-02 | Phase 71 | Pending |
| SHADER-03 | Phase 71 | Pending |
| SHADER-04 | Phase 71 | Pending |
| FILE-01 | Phase 72 | Pending |
| FILE-02 | Phase 72 | Pending |
| FILE-03 | Phase 72 | Pending |
| FILE-04 | Phase 72 | Pending |
| FILE-05 | Phase 72 | Pending |
| FILE-06 | Phase 72 | Pending |
| FILE-07 | Phase 61 | Complete |
| API-01 | Phase 73 | Pending |
| API-02 | Phase 73 | Pending |
| API-03 | Phase 73 | Pending |
| API-04 | Phase 73 | Pending |
| API-05 | Phase 73 | Pending |
| API-06 | Phase 73 | Pending |
| API-07 | Phase 73 | Pending |
| API-08 | Phase 73 | Complete |
| API-09 | Phase 73 | Complete |
| API-10 | Phase 73 | Pending |
| API-11 | Phase 60 (Plan 08, override) | Complete |
| API-12 | Phase 73 | Pending |
| API-13 | Phase 73 | Pending |

**Coverage:**
- v2.0.0-alpha requirements: 183 total (explicit REQ-ID entries in this document)
- Mapped to phases: 183 (100%)
- Unmapped: 0 ✓

**Note:** The earlier "159 total" figure from the initial draft of this file was incorrect; the actual count of `- [ ] **XXX-NN**:` entries is 183. The 14-phase roadmap covers all of them.

---
*Requirements defined: 2026-04-13*
*Last updated: 2026-04-13 after roadmap creation — traceability filled in, count corrected 159 → 183*
