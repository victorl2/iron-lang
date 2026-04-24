# Migrating from Iron v2.2 to v3.0

Iron v3.0 removes receiver-method syntax and replaces it with methods
declared inside `object` blocks. The codemod handles the mechanical
transformation. This guide walks you through the process step by step
and covers patterns the codemod does not automate.

---

## Before you start

- **Pin to v2.2.0-alpha if you are not ready to migrate.** Confirm your current version:
  ```bash
  ironc --version   # should read: 2.2.0-alpha
  ```
- **Back up your source or use version control.** The codemod rewrites files in place.
- **The codemod is idempotent.** Running it twice on already-migrated code produces no changes and is safe.

---

## Step 1: Install v3.0.0-alpha

```bash
curl -fsSL https://ironlang.org/install.sh | bash -s -- --version v3.0.0-alpha
ironc --version   # should read: 3.0.0-alpha
```

---

## Step 2: Run the codemod

```bash
ironc migrate --from v2 --to v3 src/
```

The codemod runs recursively over the directory you pass and rewrites
files in place. It covers all three mechanical transforms described
below. Before applying changes, you can preview the diff:

```bash
# Preview: prints a unified diff to stderr, does not write files
ironc migrate --from v2 --to v3 src/ 2>&1 | less

# Apply changes (rewrites in place)
ironc migrate --from v2 --to v3 src/
```

Each modified file is reported on stdout. Files with no receiver-method
syntax are untouched.

---

## Step 3: Verify

```bash
ironc build .
```

The codemod covers all mechanical transforms. Any pattern it could not
handle automatically is annotated with a `-- TODO(migrate)` comment so
you can find it with a quick grep:

```bash
grep -rn "TODO(migrate)" src/
```

---

## What the codemod transforms

### Receiver methods to in-block methods

Before (v2.2):
```iron
object Player {
    var health: Int
    var name: String
}

func (p: Player) take_damage(n: Int) {
    p.health = p.health - n
}

func (p: Player) is_alive() -> Bool {
    return p.health > 0
}
```

After (codemod output):
```iron
object Player {
    var health: Int
    var name: String

    func take_damage(n: Int) {
        self.health = self.health - n
    }

    func is_alive() -> Bool {
        return self.health > 0
    }
}
```

The receiver variable (`p` above) is renamed to `self` in the body.
Field access requires the `self.` prefix inside methods. Call sites are
unchanged: `player.take_damage(5)` works exactly as before.

---

### Mutable receivers to default-tier methods

In v2.2, methods that modified the receiver used the `mut` keyword:

```iron
func (mut c: Counter) bump(n: Int) {
    c.value = c.value + n
}
```

In v3.0, mutation is the default tier. Plain `func` methods may write
to `self`. The codemod removes the `mut` marker:

```iron
object Counter {
    var value: Int

    func bump(n: Int) {      -- default = mutating in v3
        self.value = self.value + n
    }
}
```

If you want to declare a method that promises not to mutate, use
`readonly func`. That is optional and not inserted by the codemod.

---

### Inline field defaults to init bodies

Before (v2.2):
```iron
object Player {
    var health: Int = 100
    var name: String = "unknown"
}
```

After (codemod output):
```iron
object Player {
    var health: Int
    var name: String

    init(health: Int, name: String) {
        self.health = health
        self.name = name
    }
}
```

The codemod generates an `init` from the defaults and updates every
`Player(...)` call site to pass the fields explicitly. If a field had a
constant default that makes sense as a named init, you can add a named
form manually after migration:

```iron
init default() {
    self.health = 100
    self.name = "unknown"
}
```

Named init form: `Player.default()` at call sites.

---

## New v3 patterns (not automated -- adopt at your pace)

The following features are new in v3.0. The codemod does not add them
to your code, but you can adopt them incrementally as you refactor.

### Mutation tiers

Methods may declare how much they mutate:

| Declaration | Can write self fields? | Can do I/O? |
|---|---|---|
| `func name()` | Yes | Yes |
| `readonly func name()` | No | Yes |
| `pure func name()` | No | No |

The default tier matches what v2.2 mutable receivers did. You do not
need to change anything. Add `readonly` or `pure` when you want the
compiler to enforce the contract:

```iron
object Counter {
    var value: Int

    init(start: Int) {
        self.value = start
    }

    func increment() {
        self.value = self.value + 1
    }

    readonly func current() -> Int {
        return self.value
    }

    pure func doubled() -> Int {
        return self.value * 2
    }
}
```

### Patch for open extension

`patch object T { ... }` adds methods to a type you do not own,
including built-in primitives:

```iron
patch object Int {
    pub readonly func double() -> Int {
        return self * 2
    }
}

func main() {
    println("{5.double()}")   -- 10
}
```

This replaces monkey-patching idioms from v2.2. Adopt it when you need
to extend third-party or built-in types without modifying their source.

### pub visibility

In v3.0, all declarations default to private. To expose a field, method,
or object across module boundaries, add `pub`:

```iron
object Account {
    var balance: Int          -- private
    pub var owner: String     -- public

    pub readonly func summary() -> String {
        return "{self.owner}: {self.balance}"
    }
}
```

**The codemod does not add `pub` automatically.** This requires
intentional review. After running the codemod, audit your exported API
and add `pub` to anything that needs to be visible from other modules.
Start with objects and methods referenced across module boundaries --
the compiler will surface missing `pub` declarations as errors.

---

## Troubleshooting

**`E0101: receiver-method syntax removed in v3.0`**

The file was not passed to the codemod, or the pattern was not
recognized. Run the codemod on the specific file:

```bash
ironc migrate --from v2 --to v3 src/player.iron
```

**`E03XC: inline default forbidden; assign in init`**

The codemod should have handled this. If you see it, the file may have
been skipped or contains a field default the codemod did not recognize.
Add an `init` manually and assign the field there.

**`E03XA: object has no init`**

The object has at least one field but no `init` declaration. The codemod
inserts a default init; if you see this after migration, the file was
not processed. Add an `init` manually:

```iron
object Marker {
    var tag: String

    init(tag: String) {
        self.tag = tag
    }
}
```

Objects with no fields get a synthesized empty `init` automatically --
`Marker()` is valid without any declaration.

**`Field not visible from this module`**

v3.0 is default-private. Add `pub` to any field or method that needs to
be exported. Check each error and add `pub` where access is intentional.

**`E03F1: readonly func cannot write to self.field`**

You declared a method `readonly` but the body writes to a field. Either
remove `readonly` if mutation is intended, or remove the write.

**`E03F2: readonly func cannot call mutating method`**

A `readonly` method calls a default-tier (mutating) method on `self`.
The callee must be `readonly` or `pure` for this to compile.

---

## Summary checklist

- [ ] Pin and verify v2.2.0-alpha before starting
- [ ] Run `ironc migrate --from v2 --to v3 src/` (preview first with `2>&1 | less`)
- [ ] Run `ironc build .` and fix any remaining errors
- [ ] Grep for `TODO(migrate)` comments the codemod could not resolve
- [ ] Audit exported API and add `pub` to anything that must cross module boundaries
- [ ] Optionally: add `readonly` / `pure` modifiers where mutation should be forbidden
- [ ] Optionally: adopt `patch` for extending types you do not own
