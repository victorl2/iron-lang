<!-- doctest-imports: raylib -->
# Raylib

Iron's standard library binds [raylib 6.0](https://www.raylib.com/): every
2D primitive, 3D camera, texture, font, sound and shader of upstream raylib
is callable as idiomatic Iron (`Draw.rectangle(...)`, `Texture.load(...)`,
`Camera3D(...)`). Native (`iron build`) and web (`iron build --target=web`)
programs compile against the same binding, and raylib itself is compiled
into your program: there is nothing to install besides Iron.

This guide is published at [ironlang.dev/raylib](https://ironlang.dev/raylib/),
generated from this file; every Iron example in it is compiled in CI. The
binding is organized in fourteen categories, each with a reference page:
[Types](https://ironlang.dev/raylib/reference/types.html),
[Enums](https://ironlang.dev/raylib/reference/enums.html),
[Window](https://ironlang.dev/raylib/reference/window.html),
[Input](https://ironlang.dev/raylib/reference/input.html),
[2D Drawing](https://ironlang.dev/raylib/reference/draw2d.html),
[Collision](https://ironlang.dev/raylib/reference/coll.html),
[Textures](https://ironlang.dev/raylib/reference/tex.html),
[Text](https://ironlang.dev/raylib/reference/text.html),
[Audio](https://ironlang.dev/raylib/reference/audio.html),
[3D Drawing](https://ironlang.dev/raylib/reference/draw3d.html),
[Models](https://ironlang.dev/raylib/reference/model.html),
[Shaders](https://ironlang.dev/raylib/reference/shader.html),
[Math](https://ironlang.dev/raylib/reference/math.html) and
[Files](https://ironlang.dev/raylib/reference/file.html). Every entry there
lists the signature, a short example and links to the raylib.h declaration,
the Iron source and a test that uses it.

## Your first window

Install Iron (see [INSTALL.md](../INSTALL.md) or
[ironlang.dev/install](https://ironlang.dev/install/)), then put this in
`hello.iron`:

```iron
import raylib

func main() {
    Window.init(800, 600, "first window")
    Window.set_target_fps(60)

    while not Window.should_close() {
        Draw.begin()
        Draw.clear(BLACK)
        Draw.text("Hello, Iron + raylib", Int32(200), Int32(280), Int32(24), WHITE)
        Draw.end()
    }

    Window.close()
}
```

`iron run hello.iron` opens a window with the text and exits cleanly on
ESC or when the window is closed. Every raylib program has this shape:

- `Window.init(width, height, title)` opens the window; raylib handles the
  platform specifics.
- `Window.set_target_fps(60)` caps the main loop at 60 frames per second;
  delta time is handled internally.
- `while not Window.should_close()` loops until the user closes the window
  or presses ESC.
- `Draw.begin()` and `Draw.end()` bracket every frame's render commands;
  `Draw.clear(BLACK)` wipes the framebuffer. `BLACK`, `WHITE`, `RED` and
  the other raylib color constants come with the binding.
- `Draw.text(text, x, y, size, color)` draws with the default font. raylib's
  C ABI takes `int`, so arguments that are not literals are written as
  `Int32(...)`; integer literals narrow on their own.
- `Window.close()` releases the GL context, the audio device and the window.

## Pong, a complete 2D game

[examples/pong/pong.iron](../examples/pong/pong.iron) exercises the whole
2D surface: a state machine, paddles, a ball, collision and audio. It is
the best small reference game in the repository for seeing how the pieces
fit together.

### State and setup

The top of the file sets up the title, play and game-over state machine,
the colors, the key bindings and the starting positions:

```iron
import raylib

enum GameState {
    TITLE    = 0,
    PLAYING  = 1,
    GAME_OVER = 2,
}

func main() {
    val title: String = "Iron Pong"
    val bg: Color = BLACK
    val fg: Color = WHITE
    val accent: Color = RED
    val divider: Color = DARKGRAY
    val ball_origin: Vector2 = Vector2(Float32(400.0), Float32(300.0))
    val start_key: KeyboardKey = KeyboardKey.SPACE
    val left_up: KeyboardKey    = KeyboardKey.W
    val left_down: KeyboardKey  = KeyboardKey.S
    val right_up: KeyboardKey   = KeyboardKey.UP
    val right_down: KeyboardKey = KeyboardKey.DOWN
    val initial_state: GameState = GameState.TITLE

    Window.init(800, 600, title)
    Window.set_target_fps(60)
    Window.close()
}
```

Scores are formatted with `to_string()` and `pad_left(3, " ")` so the
scoreboard keeps its width as they grow:

```iron
val left_score = 7
val right_score = 12
val left_str = left_score.to_string().pad_left(3, " ")
val right_str = right_score.to_string().pad_left(3, " ")

Window.init(800, 600, "scores")
Draw.begin()
Draw.text(left_str, Int32(200), Int32(20), Int32(40), WHITE)
Draw.text(right_str, Int32(600), Int32(20), Int32(40), WHITE)
Draw.end()
Window.close()
```

### The ball and the paddles

The ball's position is a `Vector2`; its velocity is integrated each frame
and it is drawn as a filled rectangle. `Vector2` takes `Float32`
components, raylib is float-first at the ABI boundary:

```iron
val ball_origin: Vector2 = Vector2(Float32(400.0), Float32(300.0))
Window.init(800, 600, "ball")
Draw.begin()
Draw.rectangle(0, 250, 10, 100, WHITE)
Draw.line(400, 0, 400, 600, DARKGRAY)
Draw.end()
Window.close()
```

Input is polled with `Keyboard.is_down` (held) and `Keyboard.is_pressed`
(edge), using the `KeyboardKey` enum:

```iron
Window.init(800, 600, "input")
val start: Bool      = Keyboard.is_pressed(KeyboardKey.SPACE)
val left_up: Bool    = Keyboard.is_down(KeyboardKey.W)
val left_down: Bool  = Keyboard.is_down(KeyboardKey.S)
val right_up: Bool   = Keyboard.is_down(KeyboardKey.UP)
val right_down: Bool = Keyboard.is_down(KeyboardKey.DOWN)
Window.close()
```

In the game loop these drive the paddles' `y` positions, and the SPACE
press moves `GameState.TITLE` to `PLAYING`. Paddles are drawn with
`Draw.rectangle(x, y, w, h, color)`, the same primitive as the ball.

### Collision

Pong bounces the ball off the paddles and walls with axis-aligned
rectangle intersection:

```iron
val paddle = Rectangle(Float32(0), Float32(250), Float32(10), Float32(100))
val ball   = Rectangle(Float32(395), Float32(295), Float32(10), Float32(10))

if Rectangle.collides(paddle, ball) {
    println("bounce")
}
```

The [Collision reference](https://ironlang.dev/raylib/reference/coll.html)
has the full set: rectangle against rectangle, rectangle against circle,
circle against circle, point in rectangle, and the 3D sphere, box and ray
tests.

### Audio

Audio is two steps: initialize the device, then load and play sounds:

```iron
Window.init(800, 600, "audio")
Audio.init()
val bounce: Sound = Sound.load("tests/assets/bounce.wav")

if Sound.is_valid(bounce) {
    val played = Sound.play(bounce)
}

Sound.unload(bounce)
Audio.close()
Window.close()
```

The `Sound.is_valid` guard is the pattern for "the asset did not load"
(missing file, wrong format, a headless machine): a missing `bounce.wav`
must not crash the game. For music streams and the mixer see the
[Audio reference](https://ironlang.dev/raylib/reference/audio.html).

## Rotating cube, a first 3D scene

[examples/rotating_cube/rotating_cube.iron](../examples/rotating_cube/rotating_cube.iron)
is the Iron version of raylib's "hello 3D": a window, an orbital camera,
a cube with a wireframe over a grid, in about sixty lines. It is the
cleanest template for any 3D program.

A `Camera3D` is built from a position, a target, an up vector, the field of
view and the projection. It is a `var`: the orbital update rebinds it every
frame:

```iron
var cam = Camera3D(
    Vector3(Float32(10.0), Float32(10.0), Float32(10.0)),
    Vector3(Float32(0.0),  Float32(0.0),  Float32(0.0)),
    Vector3(Float32(0.0),  Float32(1.0),  Float32(0.0)),
    Float32(45.0),
    CameraProjection.PERSPECTIVE
)
val origin = Vector3(Float32(0.0), Float32(0.0), Float32(0.0))
val size   = Float32(2.0)

Window.init(800, 600, "Rotating Cube")
Window.set_target_fps(60)
while not Window.should_close() {
    cam = Camera3D.update(cam, CameraMode.ORBITAL)

    Draw.begin()
    Draw.clear(SKYBLUE)
    Draw.begin_mode_3d(cam)

    Draw.cube(origin, size, size, size, RED)
    Draw.cube_wires(origin, size, size, size, BLACK)
    Draw.grid(Int32(10), Float32(1.0))

    Draw.end_mode_3d()
    Draw.end()
}
Window.close()
```

3D drawing is bracketed by `Draw.begin_mode_3d(cam)` and
`Draw.end_mode_3d()` inside the normal frame; between them every `Draw.*`
call renders in 3D space. `Camera3D.update(cam, CameraMode.ORBITAL)`
supplies the rotation, so there is no angle math to write; `Draw.grid`
takes no color, raylib draws it in a fixed gray. Build and run it with:

```sh
iron build examples/rotating_cube/rotating_cube.iron
./rotating_cube
```

For cylinders, capsules, planes, rays and the other camera modes see the
[3D Drawing reference](https://ironlang.dev/raylib/reference/draw3d.html).

## Post-FX, the shader pipeline

[examples/post_fx/post_fx.iron](../examples/post_fx/post_fx.iron) renders a
3D scene into an offscreen texture and draws that texture through a
fragment shader: the blueprint for screen-space effects, bloom, color
grading and any post-processing. SPACE toggles between a grayscale and an
invert shader at run time; the shaders are in
[tests/assets/shaders](../tests/assets/shaders/).

Two `Shader` resources are loaded at startup; each frame renders the scene
into the `RenderTexture`, then draws that texture full screen with the
active shader:

```iron
var cam = Camera3D(
    Vector3(Float32(4.0), Float32(4.0), Float32(4.0)),
    Vector3(Float32(0.0), Float32(0.0), Float32(0.0)),
    Vector3(Float32(0.0), Float32(1.0), Float32(0.0)),
    Float32(45.0),
    CameraProjection.PERSPECTIVE
)
val origin = Vector3(Float32(0.0), Float32(0.0), Float32(0.0))

Window.init(800, 600, "post fx")
val rt = RenderTexture.load(Int32(800), Int32(600))

-- An empty vertex shader path uses raylib's default full-screen quad stage.
val fx_grayscale = Shader.load("", "tests/assets/shaders/grayscale.fs")
val fx_invert    = Shader.load("", "tests/assets/shaders/invert.fs")
var active_shader = fx_grayscale

while not Window.should_close() {
    if Keyboard.is_pressed(KeyboardKey.SPACE) {
        active_shader = fx_invert
    }

    -- Pass 1: the 3D scene into the offscreen render texture.
    Draw.begin_texture_mode(rt)
    Draw.clear(RAYWHITE)
    Draw.begin_mode_3d(cam)
    Draw.cube(origin, Float32(2.0), Float32(2.0), Float32(2.0), RED)
    Draw.end_mode_3d()
    Draw.end_texture_mode()

    -- Pass 2: the render texture through the active shader. The negative
    -- height flips it: a render texture's origin is bottom left in OpenGL.
    Draw.begin()
    Draw.clear(BLACK)
    Draw.begin_shader_mode(active_shader)
    Texture.draw_rec(
        rt.texture,
        Rectangle(Float32(0.0), Float32(0.0), Float32(800.0), Float32(-600.0)),
        Vector2(Float32(0.0), Float32(0.0)),
        WHITE
    )
    Draw.end_shader_mode()
    Draw.end()
}
Window.close()
```

Shader uniforms are resolved by name once at load time and written per
frame with `Shader.set_value`. The invert shader takes a `u_intensity`
float, passed as its little-endian IEEE-754 bytes:

```iron
Window.init(800, 600, "uniforms")
val fx_invert = Shader.load("", "tests/assets/shaders/invert.fs")
val loc_intensity = Shader.get_location(fx_invert, "u_intensity")

-- Float32(1.0) is 0x3F800000: [0x00, 0x00, 0x80, 0x3F] little endian.
val intensity_bytes: [UInt8] = [UInt8(0), UInt8(0), UInt8(128), UInt8(63)]

-- raylib ignores the write when the uniform was not found (location -1).
if loc_intensity >= Int32(0) {
    Shader.set_value(fx_invert, loc_intensity, intensity_bytes, ShaderUniformDataType.FLOAT)
}
Window.close()
```

Matrix uniforms, texture samplers and the full `ShaderUniformDataType`
enum are in the [Shader reference](https://ironlang.dev/raylib/reference/shader.html).

## Examples

Five programs under [examples/](../examples/) exercise every category of
the binding. Each builds with `iron build examples/<name>/<name>.iron` and
runs natively and on the web target.

![pong](https://ironlang.dev/raylib/examples/screenshots/pong.png)

[pong](../examples/pong/pong.iron): a two-player paddle game. W and S move
the left paddle, the arrow keys the right one, collisions play a bounce
sound, and one main loop moves through the title, play and game-over
states. Window, input, 2D drawing, collision, audio.

![rotating_cube](https://ironlang.dev/raylib/examples/screenshots/rotating_cube.png)

[rotating_cube](../examples/rotating_cube/rotating_cube.iron): an orbital
`Camera3D` over a colored cube with a wireframe on a 10 by 10 grid. A
compact 3D starting point. Window, 3D drawing, math.

![model_viewer](https://ironlang.dev/raylib/examples/screenshots/model_viewer.png)

[model_viewer](../examples/model_viewer/model_viewer.iron): loads a 3D
model with a procedural fallback when the asset is missing, draws its
bounding box and lets the camera orbit it. Models, 3D drawing, files.

![post_fx](https://ironlang.dev/raylib/examples/screenshots/post_fx.png)

[post_fx](../examples/post_fx/post_fx.iron): the shader pipeline above,
with a render texture and interactive uniforms. Shaders, textures.

![raylib_showcase](https://ironlang.dev/raylib/examples/screenshots/raylib_showcase.png)

[raylib_showcase](../examples/raylib_showcase/raylib_showcase.iron): a
single file touching the whole binding, useful as a lookup of real call
sites.

```sh
iron build examples/pong/pong.iron && ./pong
iron build --target=web examples/pong/pong.iron    # dist/web/index.html
```

## Contributing to the binding

The binding is `src/stdlib/raylib.iron` (the Iron declarations) with the C
shim in `src/stdlib/iron_raylib.c` and `iron_raylib.h`. When a function
is added, its category's reference page under
`docs/site/raylib/reference/` gets an entry with the signature, a short
description, an example and the three links (raylib.h, the Iron source
line, a test); a function worth a tutorial gets a section in this guide,
and a showcase-level feature gets an example and a screenshot under
`docs/site/raylib/examples/screenshots/`.
