# Iron Language Definition

**Iron** — a compiled, performant programming language focused on clarity, legibility, and performance.

- File extension: `.iron`
- CLI: `iron build`, `iron run`, `iron test`

---

## Philosophy

- Concise: few keywords that express great meaning
- Strong types, no implicit conversions
- Manual memory management with built-in helpers (defer, ref counting)
- Brace-delimited blocks
- Positional arguments only, no named arguments at call sites
- Unicode-first strings
- Null safety by default
- Legibility over magic

---

## Primitive Types

All primitive types start with uppercase.

```
Int       -- platform int (64-bit)
Int8
Int16
Int32
Int64
UInt      -- platform unsigned
UInt8     -- byte
UInt16
UInt32
UInt64
Float     -- f64
Float32
Float64
Bool      -- true / false
String    -- always unicode, always iterable
```

There is no `Char` type. A single character is a `String` of length 1.

---

## Variables — val / var

```
val name = "Victor"          -- immutable, type inferred
var hp = 100                 -- mutable, type inferred
val speed: Float = 200.0     -- immutable, type explicit

name = "Other"               -- COMPILE ERROR: val cannot be reassigned
hp = 90                      -- ok
```

- `val` = immutable binding
- `var` = mutable binding
- Type is inferred by default, can be explicit with `: Type`
- `:=` is NOT used; `val`/`var` replaces it

---

## Nullable Types

Non-nullable by default. Opt in with `?`.

```
val name: String = "Victor"
name = null                    -- COMPILE ERROR: String is not nullable

var target: Enemy? = null      -- explicitly nullable
target.attack()                -- COMPILE ERROR: must check first

if target != null {
  target.attack()              -- ok, compiler narrows type
}
```

---

## Strings — Unicode First, Immutable References

Strings are **immutable reference types**. Passed by reference like objects (no copy of character data), but the characters can never be modified — operations always create new strings.

### Basics

```
val greeting = "Hello World"

-- length = number of unicode codepoints
print(len(greeting))

-- iterate characters
for c in greeting {
  print(c)
}

-- iterate with index
for i in range(len(greeting)) {
  print(greeting[i])
}

-- string interpolation
val name = "Victor"
val hp = 100
print("{name} has {hp} HP")

-- multiline
val text = """
  multi
  line
  string
"""

-- common operations
val upper = greeting.upper()
val sub = greeting[0..5]
val has = greeting.contains("World")
val parts = greeting.split(" ")
```

### Passing Semantics

Strings are passed by reference — no character data is copied.

```
func greet(name: String) {
  print("hello {name}")     -- no copy, reference to original data
}

val name = "Victor"
greet(name)                  -- cheap, just passes a reference
```

Strings are immutable. `var` rebinds the reference, it does not mutate the characters.

```
func process(var name: String) {
  name = name.upper()        -- rebinds to new string, original unchanged
}

var n = "Victor"
process(n)
print(n)                     -- still "Victor"
```

### Compiler Optimizations

- **Interning:** identical string literals share the same memory at compile time
- **Small string optimization:** short strings stored inline, no heap allocation
- **Zero-copy passing:** only the reference struct is copied (pointer + length), never the characters

```
-- interned: same memory, zero cost
val a = "hello"
val b = "hello"       -- points to same data as a

-- operations create new strings
val upper = a.upper() -- new allocation: "HELLO"
print(a)              -- still "hello", unchanged
```

---

## Objects

Declared with the `object` keyword.

```
object Player {
  var pos:    Vec2
  var hp:     Int
  val speed:  Float
  val name:   String
}
```

### Init — Mandatory Constructors (v3.0+)

Every object must declare at least one `init`. Fieldless objects receive a
synthesized empty init automatically. Two forms exist:

**Anonymous init** — constructed via `Type(args)`:

```
object Vec2 {
  val x: Float
  val y: Float

  init(x: Float, y: Float) {
    self.x = x
    self.y = y
  }
}

val v = Vec2(3.0, 4.0)    -- anonymous init
```

**Named init** — constructed via `Type.name(args)`:

```
object Vec2 {
  val x: Float
  val y: Float

  init(x: Float, y: Float) {
    self.x = x
    self.y = y
  }

  init zero() {
    self.x = 0.0
    self.y = 0.0
  }
}

val a = Vec2(3.0, 4.0)    -- anonymous
val b = Vec2.zero()       -- named
```

The compiler enforces definite assignment: every field must be assigned on
every exit path through the `init` body. `val` fields may only be assigned
once. Inline field defaults (`var x: Int = 0`) are a parse error in v3.0;
assign in `init` instead.

**Fieldless objects** need no init declaration:

```
object Marker {}    -- synthesized empty init; Marker() is valid
```

### Construction (pre-v3 reference)

In v2.x, construction used positional arguments matching declaration order
and field defaults were allowed inline. This syntax is removed in v3.0.

```
-- v2.2 style (removed in v3.0):
-- val p = Player(vec2(100.0, 100.0), 100, 200.0, "Victor")
```

### Inheritance — `extends`

Single inheritance only. Child objects inherit all fields and methods from the parent.

```
object Entity {
  var pos: Vec2
  var hp:  Int
}

object Player extends Entity {
  val name:   String
  val speed:  Float
  val sprite: rc Texture
}

object Enemy extends Entity {
  val damage:   Int
  var ai_state: AIState
}

-- Player has: pos, hp, name, speed, sprite
-- Enemy has: pos, hp, damage, ai_state
```

Parent methods are available on children. Children can override by defining the same method inside their own `object` block.

```
object Entity {
  var pos: Vec2
  var hp:  Int

  func take_damage(amount: Int) {
    self.hp = self.hp - amount
  }
}

object Player extends Entity {
  val name:  String
  val speed: Float

  -- override: no keyword needed, compiler knows
  func take_damage(amount: Int) {
    self.hp = self.hp - amount / 2
  }
}

object Knight extends Entity {
  val armor: Int

  -- call parent method with super
  func take_damage(amount: Int) {
    val reduced = amount - self.armor
    super.take_damage(reduced)
  }
}
```

### Interfaces — `implements`

Interfaces define a contract of methods that an object must implement.

```
interface Drawable {
  func draw()
}

interface Updatable {
  func update(dt: Float)
}

interface Collidable {
  func get_bounds() -> Rect
}

object Player extends Entity implements Drawable, Updatable, Collidable {
  val name:   String
  val speed:  Float
  val sprite: rc Texture

  pub init(name: String, speed: Float, sprite: rc Texture) {
    self.name   = name
    self.speed  = speed
    self.sprite = sprite
  }

  pub func draw() {
    draw_texture(self.sprite, self.pos)
  }

  pub func update(dt: Float) {
    if is_key_down(.RIGHT) { self.pos.x += self.speed * dt }
  }

  pub func get_bounds() -> Rect {
    return Rect(self.pos.x, self.pos.y, 32.0, 32.0)
  }
}
```

Missing an interface method is a compile error:

```
object Rock implements Drawable {
  val pos: Vec2

  -- COMPILE ERROR: Rock implements Drawable but missing func draw()
}
```

Interfaces can be used as types for polymorphism:

```
func draw_all(items: [Drawable]) {
  for item in items {
    item.draw()
  }
}

-- mix different types in one collection
val drawables: [Drawable] = [player, enemy, particle, ui_element]
draw_all(drawables)
```

### Patch — Open Extension (v3.0+)

`patch object T { ... }` adds methods and named inits to any type,
including types you do not own and built-in primitives. Patches are
program-wide and may not add fields.

```
patch object Int {
  pub readonly func double() -> Int {
    return self * 2
  }

  pub readonly func clamp(lo: Int, hi: Int) -> Int {
    if self < lo { return lo }
    if self > hi { return hi }
    return self
  }
}

func main() {
  println("{5.double()}")           -- 10
  println("{200.clamp(0, 100)}")    -- 100
}
```

Patches may also declare retroactive interface conformance:

```
interface Describable {
  readonly func describe() -> String
}

patch object Int implements Describable {
  pub readonly func describe() -> String {
    return "Int(" + self.to_string() + ")"
  }
}
```

<!-- Locked as a fixture at tests/integration/v3_str_describable_example.iron (Phase 96 STR-03). -->

Rules: patches may not add fields or change the visibility of existing
members. Duplicate method signatures across patches for the same type
are a compile error (E03XX).

### Runtime type checking — `is`

```
func handle_collision(a: Entity, b: Entity) {
  if a is Player and b is Enemy {
    a.take_damage(b.damage)
  }
}
```

---

## Functions and Methods

All declared with the `func` keyword.

### Standalone Functions

```
func add(a: Float, b: Float) -> Float {
  return a + b
}
```

### In-block methods (v3.0+)

Instance methods are declared inside the `object` block. The receiver is
always named `self` and is implicit — no receiver parameter is declared.
Accessing a field inside a method requires the explicit `self.` prefix;
bare field names without `self.` are a compile error.

```
object Player {
  var health: Int
  var name:   String

  init(health: Int, name: String) {
    self.health = health
    self.name   = name
  }

  func take_damage(n: Int) {
    self.health = self.health - n
  }

  readonly func is_alive() -> Bool {
    return self.health > 0
  }
}
```

Call sites are unchanged: `player.take_damage(5)` works exactly as before.

**Removed in v3.0:** The receiver-method form `func (p: Player) name()`
and the mutable receiver form `func (mut p: Player) name()` are parse
errors in v3.0. Use `ironc migrate --from v2 --to v3` to convert existing
code automatically.

### Mutation tiers

Every in-block method belongs to one of three tiers. The tier is declared
at the `func` keyword:

| Tier | Declaration | Can write `self.field`? | Can do I/O / globals? |
|------|-------------|------------------------|-----------------------|
| Default | `func name()` | Yes | Yes |
| Read-only | `readonly func name()` | No | Yes |
| Pure | `pure func name()` | No | No |

The default tier is mutating, matching v2.2 mutable-receiver behavior.
Non-mutating methods opt in with `readonly` or `pure`.

```
object Counter {
  var value: Int

  init(start: Int) {
    self.value = start
  }

  func increment() {
    self.value = self.value + 1    -- default tier: may write fields
  }

  readonly func current() -> Int {
    return self.value              -- no writes allowed
  }

  pure func doubled() -> Int {
    return self.value * 2          -- no writes, no I/O
  }
}
```

The compiler enforces tiers transitively. A `readonly` method may only
call other `readonly` or `pure` methods on `self`. A `pure` method may
only call other `pure` methods.

```
-- error[E03F1]: readonly func cannot write to self.value
readonly func reset() {
  self.value = 0    -- rejected
}

-- error[E03F2]: readonly func cannot call mutating method
readonly func reset_via_call() {
  self.increment()  -- rejected
}
```

### Static/factory methods

Factory and utility helpers that do not need an instance receiver are
written as standalone functions or named inits (see Init section below).
The `func TypeName.method_name(...)` static-form syntax from v2.x is
no longer supported for instance methods; it was replaced by in-block
declarations in v3.0.

### Passing Convention

Positional arguments only. No named arguments at call sites.

```
add(10.0, 20.0)              -- ok
add(a: 10.0, b: 20.0)        -- COMPILE ERROR
```

**Primitives** (`Int`, `Float`, `Bool`) are always **copied**.
**Objects** and **Strings** are always passed by **reference**, immutable by default. Use `var` to allow rebinding.

```
-- object parameter: immutable reference by default
func print_stats(player: Player) {
  print("{player.name}: {player.hp}")    -- ok, reading
  player.hp = 0                          -- COMPILE ERROR: immutable
}

-- var parameter: mutable reference
func heal(var player: Player, amount: Int) {
  player.hp += amount                    -- ok, modifies original
}

-- primitive parameter: copied
func double(x: Int) -> Int {
  return x * 2                           -- original unchanged
}
```

### Multiple Return Values

```
func divide(a: Float, b: Float) -> Float, Err? {
  if b == 0.0 {
    return 0.0, Err("division by zero")
  }
  return a / b, null
}

val result, val err = divide(10.0, 0.0)

if err != null {
  log(err.msg)
}

-- discard a value
val result, _ = divide(10.0, 3.0)
```

---

## Project manifest

Iron projects are declared by an `iron.toml` at the project root. The
`[package]` table carries metadata; `[dependencies]` lists local-path or
git-source libraries.

```toml
[package]
name = "my_game"
version = "0.1.0"
type = "bin"               # "bin" (default) or "lib"
iron = ">= 3.2.0"          # optional, minimum iron compiler version

[dependencies]
mylib = { path = "../mylib" }
```

The `iron` field is an optional Cargo-style semver constraint enforced
by `iron build` and `iron run` before any compile work begins. Supported
operators: `>=`, `>`, `<=`, `<`, `=` (or no operator = exact), `^`
(compatible-with: same major), `~` (compatible-with: same minor).
Comma-separated AND ranges are supported.

Examples:

```toml
iron = ">= 3.2.0"                  # any version 3.2.0 or newer
iron = ">= 3.0.0, < 4.0.0"         # any 3.x version
iron = "^3.2"                       # any 3.x version (>= 3.2.0)
```

A version mismatch produces a clear error pointing at the install script
with a `--version` argument. A package with no `iron` field is not
version-checked.

---

## Module System

### One file = one module

No module declaration needed. The file path is the module name.

```
my_game/
  main.iron              -- entry point (always "main" in project root)
  player.iron            -- module: player
  enemy.iron             -- module: enemy
  physics/
    collision.iron       -- module: physics.collision
    rigid_body.iron      -- module: physics.rigid_body
  ui/
    menu.iron            -- module: ui.menu
    hud.iron             -- module: ui.hud
```

### Imports

```
-- import by path, flat access
import player
import physics.collision

-- import with alias, prefixed access
import physics.rigid_body as rb
import ui.menu as menu

val body = rb.create_body(pos, mass)
menu.show()
```

- `import x` = flat, everything available directly
- `import x as y` = prefixed, access through alias
- Import path matches file path with dots as separators

### Visibility

**v3.0:** Everything is private by default. Add `pub` to expose a field,
method, or object across module boundaries.

```
-- player.iron (v3.0)
pub object Player {            -- public: visible to importers
  var health: Int              -- private field
  pub var name: String         -- public field (getter + setter)

  pub init(health: Int, name: String) {
    self.health = health
    self.name   = name
  }

  pub func take_damage(n: Int) {
    self.health = self.health - n
  }

  func recalc_stats() {        -- private: only callable inside Player
    self.health = clamp(self.health, 0, 100)
  }
}
```

`pub var field` synthesizes a getter and a setter; `pub val field`
synthesizes a read-only getter. Call sites use property syntax:
`player.name` rather than an explicit getter call.

The codemod does not add `pub` automatically after migration. You must
audit your exported API and add `pub` to any declaration that must cross
module boundaries.

**v2.x (removed in v3.0):** The old model was public-by-default with an
explicit `private` keyword to restrict scope. That behavior is reversed
in v3.0.

### Entry Point

The project entry point is always a file called `main` in the root of the project containing `func main()`.

```
-- main.iron
import player
import ui.menu as menu

func main() {
  val window = init_window(800, 600, "Game")
  defer close_window(window)
  var p = Player.new()
  menu.show()
}
```

---

## Control Flow

Brace-delimited blocks.

```
if player.hp > 0 {
  player.update(dt)
}

if player.hp <= 0 {
  player.die()
} elif player.hp < 20 {
  player.warn_low_hp()
} else {
  player.update(dt)
}

for i in range(len(bullets)) {
  bullets[i].update(dt)
}

for c in name {
  print(c)
}

while not window_should_close() {
  val dt = get_frame_time()
}

match state {
  GameState.RUNNING { player.update(dt) }
  GameState.PAUSED  { draw_pause_menu() }
  GameState.MENU    { draw_main_menu() }
  else { log.warn("unknown state") }
}
```

---

## Memory Management

### Stack — Default

No keyword needed. Values live on the stack and are freed when the scope ends.
**Stack values can never escape their block.** Returning or storing them in an outer scope is a compile error. Use `heap` if the value needs to outlive its scope.

```
val pos = vec2(10.0, 20.0)   -- stack, freed when scope ends

-- COMPILE ERROR: stack value cannot escape
func make_grid() -> [Vec2; 16] {
  var grid = [Vec2; 16]
  return grid                  -- ERROR
}

-- correct: use heap to return
func make_grid() -> [Vec2; 16] {
  val grid = heap [Vec2; 16]
  return grid                  -- ok
}

-- correct: create in caller's scope
val grid = [Vec2; 16]
```

### Heap — Manual with `heap` / `free`

Use `heap` to allocate on the heap.

```
val enemies = heap [Enemy; 64]    -- heap array of 64 enemies
val boss = heap Enemy(Vec2(0.0, 0.0), 1000)  -- single heap object
```

### Auto-Free — Block-Scoped

Heap values that don't escape their block are automatically freed at block exit. No `defer free` needed.

```
func process() {
  val data = heap [UInt8; 4096]
  parse(data)
  -- auto-freed here, data doesn't escape
}

if is_key_pressed(.SPACE) {
  val particles = heap [Particle; 100]
  -- ... use particles ...
  -- auto-freed here, end of if block
}

while running {
  val buf = heap [UInt8; 1024]
  -- auto-freed here, end of each iteration
}
```

A value "escapes" when it is returned or stored in an outer scope. Escaped values are not auto-freed and produce a compiler **warning**.

```
func create_boss() -> Enemy {
  val boss = heap Enemy(Vec2(0.0, 0.0), 1000)
  return boss    -- escapes: no auto-free, compiler warning
}

func spawn(var game: Game) {
  val e = heap Enemy(Vec2(0.0, 0.0), 100)
  game.enemies.add(e)    -- escapes: no auto-free, compiler warning
}
```

### Manual `free`

Still available for explicit control, especially for escaped values.

```
val boss = heap Enemy(Vec2(0.0, 0.0), 1000)
-- ... use boss ...
free boss    -- explicit free
```

### `leak` — Intentional Permanent Allocation

Silences the compiler warning. Signals that a heap value is meant to live forever.

```
val atlas = heap load_texture("atlas.png")
leak atlas    -- no warning, lives for entire program

val sin_table = heap [Float; 360]
precompute_sin(sin_table)
leak sin_table
```

Compiler rules:
- `leak` on a non-heap value = **COMPILE ERROR**
- `leak` on an `rc` value = **COMPILE ERROR**
- Leaked values are still usable, they just never get freed

### Compiler Diagnostics

| Situation | Result |
|-----------|--------|
| `heap` without `free`, value doesn't escape | Auto-freed, no warning |
| `heap` without `free`, value escapes | **Warning**: possible memory leak |
| `heap` with `leak` | No warning, intentional |
| `free` on a non-heap value | **COMPILE ERROR** |
| `leak` on a non-heap value | **COMPILE ERROR** |
| `leak` on an `rc` value | **COMPILE ERROR** |

### Defer

Runs at scope exit. Used for non-memory resource cleanup.

```
val window = init_window(800, 600, "Game")
defer close_window(window)

val file = open_file("save.dat")
defer close_file(file)
```

Defer works at any scope level. Can still be used with `free` for explicit heap cleanup if preferred.

### Reference Counting — Opt-in with `rc`

For shared ownership. Automatically freed when the last reference dies.

```
val sprite = rc load_texture("hero.png")  -- ref count = 1
var other = sprite                         -- ref count = 2
other = null                               -- ref count = 1
-- when last ref dies, texture is freed automatically
```

No `free` or `leak` needed for `rc` values.

### No Pointers

There are no pointer types in the language. The compiler handles all reference-to-pointer translation when generating C code for FFI/raylib interop.

| Language | Generated C |
|---|---|
| `player: Player` | `const Player *player` |
| `var player: Player` | `Player *player` |
| `x: Int` | `int64_t x` (copied) |
| `data: [UInt8; 64]` | `const uint8_t *data` |

---

## Concurrency

### Thread Pools

```
-- create a pool with N threads
val compute = pool("compute", 4)
val io = pool("io", 1)
val physics = pool("physics", 2)

-- pin a pool to specific CPU cores
physics.pin(2, 3)
```

### Spawn — Launch a Thread

`spawn(name, pool?) { }` — name is required, pool is optional (uses default runtime pool).

```
-- spawn on default pool
spawn("autosave") {
  save_game(state)
}

-- spawn on a specific pool
spawn("physics-step", physics) {
  physics_step(world)
}

-- get a handle to await the result
val handle = spawn("asset-loader", io) {
  return load_texture("hero.png")
}

-- await: block until done
val tex = await handle

-- non-blocking check
if handle.done() {
  val tex = handle.result()
}
```

### Channels — Thread Communication

Typed, optionally buffered.

```
-- unbuffered channel
val ch = channel[String]()

-- buffered channel (holds up to N values before send blocks)
val ch = channel[Texture](4)

spawn("loader", io) {
  ch.send(load_texture("hero.png"))
}

-- blocking receive
val tex = ch.recv()

-- non-blocking receive (returns T?)
val tex = ch.try_recv()
if tex != null {
  register_texture(tex)
}

-- close a channel
ch.close()
```

### Mutex — Shared State

Wraps a value. Must lock to access.

```
val score = mutex(0)

spawn("scorer") {
  score.lock(func(var s) {
    s += 10
  })
}

-- read also requires lock
score.lock(func(val s) {
  print("score: {s}")
})
```

### Parallel For

Add `parallel` after the range to split a loop across cores. Each iteration must be independent — no mutating outer variables. An implicit barrier at the end ensures all work completes before the next line runs.

```
-- parallel on default runtime pool
for i in range(len(particles)) parallel {
  particles[i].update(dt)
}

-- parallel on a specific pool
for i in range(len(particles)) parallel(compute) {
  particles[i].update(dt)
}

-- sequential: no parallel keyword
for i in range(len(particles)) {
  particles[i].update(dt)
}

-- COMPILE ERROR: can't mutate outer var in parallel for
var total = 0
for i in range(len(enemies)) parallel {
  total += enemies[i].hp
}

-- correct: use mutex
val total = mutex(0)
for i in range(len(enemies)) parallel {
  total.lock(func(var t) { t += enemies[i].hp })
}
```

### C Translation

The compiler generates a small runtime (~500 lines of C) using pthreads.

| Language | Generated C |
|---|---|
| `pool("name", N)` | `thread_pool_t` with N pthreads + work queue |
| `pool.pin(cores...)` | `pthread_setaffinity_np` / `thread_policy_set` |
| `spawn("name", pool) { }` | Extract block into function, submit to pool |
| `await handle` | `pthread_cond_wait` on handle's condition var |
| `channel[T](N)` | Ring buffer + mutex + condition variables |
| `ch.send() / ch.recv()` | Lock, write/read buffer, signal condition |
| `mutex(val)` | `pthread_mutex_t` wrapping value |
| `for ... parallel(pool)` | Split range into chunks, submit to pool, barrier wait |

### Full Threading Example

```
func main() {
  val window = init_window(800, 600, "Game")
  defer close_window(window)

  val io = pool("io", 1)
  val compute = pool("compute", 4)
  val physics_pool = pool("physics", 2)
  physics_pool.pin(2, 3)

  val asset_ch = channel[Texture](8)

  -- background asset loading on io pool
  spawn("asset-loader", io) {
    val files = list_files("assets/textures/")
    for f in files {
      asset_ch.send(load_texture(f))
    }
    asset_ch.close()
  }

  -- physics on its own pool
  val world = mutex(PhysicsWorld.new())
  spawn("physics", physics_pool) {
    while not window_should_close() {
      world.lock(func(var w) {
        w.step(1.0 / 60.0)
      })
    }
  }

  -- main game loop
  while not window_should_close() {
    -- grab loaded assets as they arrive
    val tex = asset_ch.try_recv()
    if tex != null {
      register_texture(tex)
    }

    -- update particles in parallel on compute pool
    for i in range(len(particles)) parallel(compute) {
      particles[i].update(get_frame_time())
    }

    draw {
      clear(DARKGRAY)
      draw_world()
    }
  }
}
```

---

## Managed Blocks

Managed blocks that auto-wrap begin/end pairs.

```
draw {
  clear(DARKGRAY)
  player.draw()
}
```

---

## Access Control

**v3.0:** All declarations are private by default. Use `pub` to opt into
cross-module visibility. This is enforced by the compiler — omitting `pub`
on a referenced declaration is a hard error, not a convention.

```
-- v3.0 access control
pub object Player {
  var health: Int              -- private field
  pub var name: String         -- public property

  pub init(health: Int, name: String) {
    self.health = health
    self.name   = name
  }

  pub func take_damage(n: Int) {
    self.health = self.health - n
    self.recalc_stats()
  }

  func recalc_stats() {        -- private: not exported
    self.health = clamp(self.health, 0, 100)
  }
}

-- private implementation detail: not importable
object InternalMetrics {
  var tick_count: Int
  var debug_mode: Bool
}
```

**v2.x reference (removed):** Public-by-default with `private` keyword.
Replace `private func` with an unprefixed in-block method and remove the
`private` keyword. Replace any "public by default" field with `pub var` or
`pub val` as needed.

---

## Enums

### Plain Enums

Simple C-style enums. Always accessed with the type prefix.

```
enum GameState {
  PAUSED,
  RUNNING,
  MENU,
}

var state = GameState.RUNNING

if state == GameState.PAUSED {
  draw_text("PAUSED", 300, 250, 40, WHITE)
}
```

### Algebraic Data Types (ADTs)

Enum variants can carry data payloads, making them full algebraic data types.

```
enum Shape {
  Circle(Float),
  Rect(Float, Float),
  Point,
}

val s = Shape.Circle(5.0)
val r = Shape.Rect(10.0, 20.0)
val p = Shape.Point
```

### Pattern Matching

Use `match` with `->` arrow syntax to destructure enum variants. The compiler enforces exhaustiveness — all variants must be covered or an `else` arm provided.

```
match state {
  GameState.RUNNING -> player.update(dt)
  GameState.PAUSED  -> draw_pause_menu()
  GameState.MENU    -> draw_main_menu()
  else -> log.warn("unknown state")
}

-- destructuring payloads
match shape {
  Shape.Circle(radius) -> {
    val area = 3.14159 * radius * radius
    println("circle area: {area}")
  }
  Shape.Rect(w, h) -> println("rect area: {w * h}")
  Shape.Point -> println("point")
}

-- wildcard _ ignores a binding
match shape {
  Shape.Rect(_, h) -> println("height: {h}")
  else -> println("not a rect")
}
```

### Methods on Enums

Methods on enum types are declared inside the `enum` block and use
`match self` to dispatch on variants.

```
enum Shape {
  Circle(Float),
  Rect(Float, Float),
  Point,

  readonly func area() -> Float {
    match self {
      Shape.Circle(r)  -> return 3.14159 * r * r
      Shape.Rect(w, h) -> return w * h
      Shape.Point      -> return 0.0
    }
  }
}

val s = Shape.Circle(5.0)
println("area: {s.area()}")
```

### Generic Enums

Enums support type parameters with monomorphization, just like objects.

```
enum Option[T] {
  Some(T),
  None,
}

enum Result[T, E] {
  Ok(T),
  Err(E),
}

val x = Option.Some(42)
match x {
  Option.Some(v) -> println("got {v}")
  Option.None -> println("nothing")
}
```

### Recursive Enums

Enum variants can reference their own type. Recursive fields are automatically heap-allocated (auto-boxed) — no manual `heap` annotation needed.

```
enum Expr {
  IntLit(Int),
  BinOp(Expr, Op, Expr),
}

func eval(e: Expr) -> Int {
  match e {
    Expr.IntLit(n) -> return n
    Expr.BinOp(left, op, right) -> {
      return eval(left) + eval(right)
    }
  }
}
```

---

## Generics

Full generics using `[T]` syntax. Works on functions, objects, and methods.

```
-- generic function
func find[T](items: [T], check: func(T) -> Bool) -> T? {
  for item in items {
    if check(item) {
      return item
    }
  }
  return null
}

-- generic object with in-block methods
object Pool[T] {
  var items: [T]
  var count: Int

  init(items: [T], count: Int) {
    self.items = items
    self.count = count
  }

  func get() -> T? {
    if self.count > 0 {
      self.count -= 1
      return self.items[self.count]
    }
    return null
  }

  func put(item: T) {
    self.items[self.count] = item
    self.count += 1
  }
}

-- usage
var bullet_pool = Pool[Bullet](heap [Bullet; 256], 0)
val b = bullet_pool.get()
```

---

## Lambdas

Anonymous functions use `func() { }` — same keyword as named functions, just without a name.

```
-- zero-arg lambda
val greet = func() { print("hello") }

-- with parameters
val add = func(a: Int, b: Int) -> Int { return a + b }

-- single expression: implicit return
val double = func(x: Int) -> Int { x * 2 }

-- multiline
val on_collision = func(entity: Entity, other: Entity) {
  entity.take_damage(10)
  spawn_particles(entity.pos)
}

-- no params, return type inferred
val get_value = func() {
  val a = 100
  return a
}
```

### Inference

The compiler infers return type and captures. Parameter types can be inferred from context.

```
-- compiler knows find expects func(Enemy) -> Bool
val enemy = find[Enemy](enemies, func(e) { e.hp < 50 })

-- compiler infers return type as Int
val get_hp = func() { player.hp }
```

### Capture

Lambdas automatically capture outer variables. `val` bindings are captured by value (snapshot), `var` bindings are captured by reference (shared mutation).

```
var score = 0
val on_kill = func() { score += 1 }
on_kill()
print(score)                     -- 1, var captured by reference

val threshold = 50
val check = func(hp: Int) -> Bool { hp < threshold }
-- threshold captured by value (snapshot at creation time)
```

### Lambdas as Types

Use `func(...)` syntax for lambda types in object fields and parameters.

```
object Button {
  val pos:      Vec2
  val size:     Vec2
  val label:    String
  val on_click: func()
}

val btn = Button(Vec2(300.0, 200.0), Vec2(200.0, 50.0), "Start", func() { start_game() })

-- passing lambdas to functions
func on_key_pressed(key: Key, callback: func()) {
  if is_key_pressed(key) {
    callback()
  }
}

on_key_pressed(.SPACE, func() { player.shoot() })
```

---

## Compile-Time Evaluation

Any pure function can be evaluated at compile time using `comptime` at the call site. The result is baked into the binary with zero runtime cost.

```
-- a normal function
func build_sin_table() -> [Float; 360] {
  var table = [Float; 360]
  for i in range(360) {
    table[i] = sin(Float(i) * 3.14159 / 180.0)
  }
  return table
}

-- comptime at call site: evaluated during compilation
val SIN_TABLE = comptime build_sin_table()

-- same function can also run at runtime
val dynamic_table = build_sin_table()

-- embed files at compile time
val SHADER_SOURCE = comptime read_file("shaders/main.glsl")
val SPRITE_DATA = comptime read_file("assets/hero.png")

-- any pure function works
func fibonacci(n: Int) -> Int {
  if n <= 1 { return n }
  return fibonacci(n - 1) + fibonacci(n - 2)
}

val FIB_20 = comptime fibonacci(20)    -- baked into binary
val fib_n = fibonacci(user_input)       -- computed at runtime
```

### Comptime Restrictions

Comptime functions **can**:
- Use math, loops, conditionals — anything pure
- Read files (embed assets)
- Build lookup tables
- Manipulate strings

Comptime functions **cannot**:
- Use `heap` or `free` (no heap allocation)
- Use `rc` (no reference counting)
- Call runtime APIs (raylib, OS, network)
- Perform I/O beyond file reads

---

## Comments

```
-- single line comment
```

---

## Full Example

```
import raylib
import math as m

pub object Player {
  var pos:    Vec2
  var hp:     Int
  val speed:  Float
  pub val name:   String
  val sprite: rc Texture

  pub init(name: String) {
    self.pos    = vec2(100.0, 100.0)
    self.hp     = 100
    self.speed  = 200.0
    self.name   = name
    self.sprite = rc load_texture("hero.png")
  }

  pub func update(dt: Float) {
    if is_key_down(.RIGHT) { self.pos.x += self.speed * dt }
    if is_key_down(.LEFT)  { self.pos.x -= self.speed * dt }
    if is_key_down(.UP)    { self.pos.y -= self.speed * dt }
    if is_key_down(.DOWN)  { self.pos.y += self.speed * dt }
  }

  pub readonly func draw() {
    draw_texture(self.sprite, self.pos)
  }

  pub readonly func is_alive() -> Bool {
    return self.hp > 0
  }
}

func main() {
  val window = init_window(800, 600, "My Game")
  defer close_window(window)

  var player = Player("Victor")

  val bullets = heap [Bullet; 256]
  var bullet_count = 0

  while not window_should_close() {
    val dt = get_frame_time()

    player.update(dt)

    if is_key_pressed(.SPACE) {
      bullets[bullet_count] = player.shoot()
      bullet_count += 1
    }

    for i in range(bullet_count) {
      bullets[i].update(dt)
    }

    draw {
      clear(DARKGRAY)
      player.draw()
      for i in range(bullet_count) {
        if bullets[i].alive {
          draw_circle(bullets[i].pos, 4.0, RED)
        }
      }
      draw_text("{player.name}: {bullet_count} bullets", 10, 10, 20, WHITE)
    }
  }
}
```

---

## Keywords Summary

```
val        -- immutable binding
var        -- mutable binding
func       -- function/method declaration
object     -- data structure declaration
enum       -- enumeration declaration
interface  -- interface declaration
patch      -- open extension of existing types (v3.0+)
init       -- constructor declaration inside object block (v3.0+)
pub        -- opt-in cross-module visibility (v3.0+)
readonly   -- method modifier: no field writes (v3.0+)
pure       -- method modifier: no field writes, no I/O (v3.0+)
self       -- implicit method receiver (required in-block, v3.0+)
import     -- module import
if         -- conditional
elif       -- else-if
else       -- else branch
for        -- loop (sequential), or parallel with `parallel` modifier
while      -- loop with condition
parallel   -- modifier on for loop for parallel execution
match      -- pattern match on enums and values (-> arrow syntax)
return     -- return from function
heap       -- heap allocation
free       -- heap deallocation
leak       -- intentional permanent allocation
defer      -- execute at scope exit
rc         -- reference-counted wrapper
extends    -- single inheritance
implements -- interface implementation
super      -- call parent method
is         -- runtime type check
spawn      -- launch a thread
await      -- wait for thread result
pool       -- create a thread pool
true       -- boolean literal
false      -- boolean literal
null       -- nullable empty value
not        -- logical negation
and        -- logical and
or         -- logical or
comptime   -- compile-time evaluation at call site
```

---

## Standard Library

### Built-in — No Import Needed

#### Printing

```
print("hello")
println("hello")
```

#### Type Conversions

```
val f = Float(42)
val i = Int(3.14)
val s = String(100)
```

#### Common Functions

```
len(array)
len(string)
len(list)

range(end)              -- 0..end-1
range(start, end)       -- start..end-1

abs(x)
min(a, b)
max(a, b)
clamp(val, min, max)

assert(condition)
assert(condition, "message")
```

#### Collections

```
-- List: dynamic array
var enemies = List[Enemy]()
enemies.add(enemy)
enemies.remove(0)
enemies.insert(2, enemy)
val e = enemies.get(0)
val n = enemies.len()
enemies.clear()

for e in enemies {
  e.update(dt)
}

-- Map: key-value store
var scores = Map[String, Int]()
scores.set("victor", 100)
val s = scores.get("victor")       -- Int?
val has = scores.has("victor")
scores.remove("victor")
val keys = scores.keys()

-- Set: unique values
var tags = Set[String]()
tags.add("enemy")
tags.add("flying")
val has = tags.has("enemy")
tags.remove("flying")
```

#### String Methods

Methods directly on the String type.

```
val upper = name.upper()
val lower = name.lower()
val trimmed = text.trim()
val parts = text.split(",")
val joined = ",".join(parts)
val sub = text.substring(0, 5)
val has = text.contains("hello")
val starts = text.starts_with("http")
val ends = text.ends_with(".png")
val replaced = text.replace("old", "new")
val padded = "42".pad_left(5, "0")       -- "00042"
val idx = text.index_of("world")         -- -1 if not found
val ch = text.char_at(0)
val n = text.to_int()
val f = text.to_float()
val repeated = "ha".repeat(3)            -- "hahaha"
val cnt = text.count("o")
val rpad = "42".pad_right(5, " ")        -- "42   "
```

#### Concurrency Primitives

```
channel[T]()
channel[T](n)
mutex(val)
pool(name, count)
```

### `math` — Import Required

```
import math

-- constants
math.PI
math.TAU
math.E

-- trig
math.sin(x)
math.cos(x)
math.tan(x)
math.asin(x)
math.acos(x)
math.atan2(y, x)

-- common
math.floor(x)
math.ceil(x)
math.round(x)
math.sqrt(x)
math.pow(base, exp)
math.lerp(a, b, t)
math.sign(x)
math.log(x)
math.log2(x)
math.exp(x)
math.hypot(a, b)

-- random
math.random()                -- 0.0..1.0
math.random_int(min, max)
math.random_float(min, max)
math.seed(n)
```

### `io` — Import Required

```
import io

-- file operations
val data = io.read_file("save.dat")
io.write_file("save.dat", data)
io.append_file("log.txt", "entry\n")
val exists = io.file_exists("save.dat")
val files = io.list_files("assets/")
val lines = io.read_lines("data.csv")
io.create_dir("saves/")
io.delete_file("temp.dat")

-- console
val input = io.read_line()

-- path utilities
val name = io.basename("/home/user/file.txt")     -- "file.txt"
val dir = io.dirname("/home/user/file.txt")        -- "/home/user"
val full = io.join_path("assets", "hero.png")      -- "assets/hero.png"
val ext = io.extension("hero.png")                 -- ".png"
val is_d = io.is_dir("assets/")
```

### `time` — Import Required

```
import time

val now = time.now()              -- Float, seconds since program start
val ms = time.now_ms()            -- Int, milliseconds
time.sleep(0.5)                   -- sleep 500ms
val elapsed = time.since(start)   -- seconds since timestamp

-- timer utility
var timer = time.Timer(2.0)       -- 2 second timer
timer.update(dt)
if timer.done() {
  spawn_wave()
  timer.reset()
}
```

### `log` — Import Required

```
import log

log.info("game started")
log.warn("low memory")
log.error("failed to load texture")
log.debug("player pos: {player.pos}")

log.set_level(log.WARN)
```

### Raylib — Import Required

Raylib ships with the compiler but is not part of the stdlib. It's the primary external library.

```
import raylib

val window = init_window(800, 600, "Game")
```

---

## Resolved Design Decisions

- **Operator overloading:** No. Operators (`+`, `-`, `*`, `/`) only work on primitives. Use explicit functions for custom types (e.g., `vec_add`, `vec_scale`). Legibility over magic.
- **String methods:** On the type directly (`name.upper()`), not in a separate module.
- **Collections:** Built-in, no import needed (`List[T]`, `Map[K,V]`, `Set[T]`).
- **Math basics:** `min`, `max`, `clamp`, `abs` are built-in. Trig and advanced math require `import math`.

---

## Open Design Questions

- **Package manager / dependency system:** how to pull in external libraries?
- **Testing:** built-in test runner or external?
- **Build configuration:** project file format?
