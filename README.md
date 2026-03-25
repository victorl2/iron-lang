# Iron

A compiled, performant programming language built for game development.

Iron compiles to C. It's designed to be legible, strongly typed, and give you full control over memory — without the complexity of a borrow checker.

## Quick Look

```iron
object Player {
  var pos:    Vec2
  var hp:     Int
  val speed:  Float
  val name:   String
  val sprite: rc Texture
}

func Player.new(name: String) -> Player {
  return Player(vec2(100.0, 100.0), 100, 200.0, name, rc load_texture("hero.png"))
}

func Player.update(dt: Float) {
  if is_key_down(.RIGHT) { self.pos.x += self.speed * dt }
  if is_key_down(.LEFT)  { self.pos.x -= self.speed * dt }
  if is_key_down(.UP)    { self.pos.y -= self.speed * dt }
  if is_key_down(.DOWN)  { self.pos.y += self.speed * dt }
}

func Player.draw() {
  draw_texture(self.sprite, self.pos)
}

func main() {
  val window = init_window(800, 600, "My Game")
  defer close_window(window)

  var player = Player.new("Victor")

  while not window_should_close() {
    val dt = get_frame_time()
    player.update(dt)

    draw {
      clear(DARKGRAY)
      player.draw()
      draw_text("{player.name}: {player.hp} HP", 10, 10, 20, WHITE)
    }
  }
}
```

## Features

### Strong Types, Concise Syntax

```iron
val name = "Victor"          -- immutable, type inferred
var hp = 100                 -- mutable, type inferred
val speed: Float = 200.0     -- explicit type

var target: Enemy? = null    -- nullable types are explicit
if target != null {
  target.attack()            -- compiler narrows type after null check
}
```

### Memory Management — You're in Control

No garbage collector. No borrow checker. Instead, a gradient of tools that cover every use case:

```iron
-- stack: default, freed at scope exit
val pos = Vec2(10.0, 20.0)

-- heap: explicit, auto-freed if it doesn't escape the block
val enemies = heap [Enemy; 64]

-- ref counting: opt-in shared ownership
val sprite = rc load_texture("hero.png")

-- intentional permanent allocation
val atlas = heap load_texture("atlas.png")
leak atlas

-- resource cleanup
val window = init_window(800, 600, "Game")
defer close_window(window)
```

### Concurrency Built In

```iron
val compute = pool("compute", 4)
val io = pool("io", 1)

-- background loading
val handle = spawn("loader", io) {
  return load_texture("hero.png")
}
val tex = await handle

-- channels for thread communication
val ch = channel[Texture](8)
spawn("loader", io) {
  ch.send(load_texture("hero.png"))
}
val tex = ch.recv()

-- parallel loops
for i in range(len(particles)) parallel(compute) {
  particles[i].update(dt)
}
```

### OOP — Simple and Explicit

```iron
object Entity {
  var pos: Vec2
  var hp:  Int
}

object Player extends Entity {
  val name:  String
  val speed: Float
}

interface Drawable {
  func draw()
}

object Player extends Entity implements Drawable {
  val name:   String
  val sprite: rc Texture
}

func Player.draw() {
  draw_texture(self.sprite, self.pos)
}
```

### Compile-Time Evaluation

```iron
func build_sin_table() -> [Float; 360] {
  var table = [Float; 360]
  for i in range(360) {
    table[i] = sin(Float(i) * 3.14159 / 180.0)
  }
  return table
}

val SIN_TABLE = comptime build_sin_table()  -- baked into binary
val SHADER = comptime read_file("shaders/main.glsl")
```

### Lambdas

```iron
val on_click = func() { start_game() }
val double = func(x: Int) -> Int { x * 2 }
val enemy = find[Enemy](enemies, func(e) { e.hp < 50 })
```

## Design Principles

- **Legibility over magic.** No operator overloading, no implicit conversions, no hidden control flow. When you read Iron code, you know what it does.
- **Manual memory with safety nets.** You control allocation. The compiler warns about leaks and auto-frees locals. `rc` is there when you need shared ownership.
- **Game-dev first.** Every feature was designed with game development patterns in mind — from `draw {}` blocks to `parallel` for loops to thread pools with core pinning.
- **Compiles to C.** Iron transpiles to C and uses your system's C compiler. Performance matches hand-written C because it *is* C at the end.
- **No pointers in the language.** Objects are passed by reference, primitives by value. The compiler generates pointers in the C output — you never see them.

## Keywords

```
val  var  func  object  enum  interface  import
if  elif  else  for  while  match  return
heap  free  leak  defer  rc
private  extends  implements  super  is
spawn  await  parallel  pool
true  false  null  not  and  or
self  comptime
```

## Standard Library

**Built-in (no import):** `print`, `println`, `len`, `range`, `min`, `max`, `clamp`, `abs`, `assert`, `List[T]`, `Map[K,V]`, `Set[T]`, String methods, concurrency primitives

**Import required:**
- `math` — trig, sqrt, pow, lerp, random
- `io` — file read/write
- `time` — timestamps, timers, sleep
- `log` — leveled logging

## Documentation

See [docs/language_definition.md](docs/language_definition.md) for the complete language specification.

## License

MIT
